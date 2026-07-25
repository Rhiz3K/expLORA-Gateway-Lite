#!/usr/bin/env python3
"""Focused hardware regression test for GitHub issue #23.

The test is intentionally separate from the normal smoke suite because it:

* fills all 20 sensor slots with large persisted records,
* reboots the gateway and verifies every record survived,
* races log rendering against log clearing,
* holds a real custom-URL request open while checking API responsiveness.

All temporary sensors and configuration changes are removed in ``finally``.
Existing sensors and their device keys are never read, replaced, or deleted.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import html.parser
import re
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Dict, Iterable, List, Optional, Tuple
from urllib.parse import urlparse

import requests


MAX_SENSORS = 20
FAKE_SERIAL_BASE = 0xE00000


class TestFailure(RuntimeError):
    pass


def require_status(response: requests.Response, allowed: Iterable[int], label: str) -> None:
    if response.status_code not in allowed:
        raise TestFailure(
            f"{label}: HTTP {response.status_code}, body={response.text[:160]!r}"
        )


class EditFormParser(html.parser.HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.in_form = False
        self.select_name: Optional[str] = None
        self.values: Dict[str, str] = {}

    def handle_starttag(self, tag: str, attrs: List[Tuple[str, Optional[str]]]) -> None:
        attr = {key: (value or "") for key, value in attrs}
        if tag == "form" and attr.get("action") == "/sensors/update":
            self.in_form = True
            return
        if not self.in_form:
            return
        if tag == "input" and attr.get("name"):
            if attr.get("type", "text") not in {"submit", "button"}:
                self.values[attr["name"]] = attr.get("value", "")
        elif tag == "select" and attr.get("name"):
            self.select_name = attr["name"]
        elif tag == "option" and self.select_name and "selected" in attr:
            self.values[self.select_name] = attr.get("value", "")

    def handle_endtag(self, tag: str) -> None:
        if tag == "select":
            self.select_name = None
        elif tag == "form" and self.in_form:
            self.in_form = False


def edit_form(session: requests.Session, base: str, index: int) -> Optional[Dict[str, str]]:
    response = session.get(
        f"{base}/sensors/edit?index={index}", allow_redirects=False, timeout=5
    )
    if response.status_code in (302, 303, 404):
        return None
    require_status(response, (200,), f"read sensor index {index}")
    parser = EditFormParser()
    parser.feed(response.text)
    if "serialNumber" not in parser.values:
        return None
    parser.values.setdefault("index", str(index))
    parser.values.setdefault("deviceKey", "")
    return parser.values


def configured_forms(session: requests.Session, base: str) -> Dict[int, Dict[str, str]]:
    result: Dict[int, Dict[str, str]] = {}
    for index in range(MAX_SENSORS):
        form = edit_form(session, base, index)
        if form is not None:
            result[index] = form
    return result


def normal_serial(value: str) -> str:
    return value.strip().upper().lstrip("0") or "0"


def post_form(
    session: requests.Session, base: str, path: str, data: Dict[str, str]
) -> requests.Response:
    return session.post(
        base + path,
        data=data,
        headers={"Origin": base},
        timeout=8,
    )


def add_fake_sensors(
    session: requests.Session,
    base: str,
    existing_serials: set[str],
    required: int,
) -> List[str]:
    added: List[str] = []
    candidate = FAKE_SERIAL_BASE
    while len(added) < required:
        serial = f"{candidate:06X}"
        candidate += 1
        if normal_serial(serial) in existing_serials:
            continue
        suffix = len(added)
        data = {
            "name": f"issue23-storage-{suffix:02d}-" + ("N" * 72),
            "deviceType": "1",
            "serialNumber": serial,
            "deviceKey": f"A0{suffix:06X}",
            "customUrl": (
                "https://collector.example.invalid/issue23/" + ("u" * 260) +
                "?temperature=*TEMP*&humidity=*HUM*&serial=*SN*"
            ),
        }
        response = post_form(session, base, "/sensors/add", data)
        require_status(response, (200, 302, 303), f"add temporary sensor {suffix}")
        added.append(serial)
    return added


def delete_serials(
    session: requests.Session, base: str, serials: Iterable[str]
) -> None:
    wanted = {normal_serial(value) for value in serials}
    forms = configured_forms(session, base)
    targets = [
        index
        for index, form in forms.items()
        if normal_serial(form["serialNumber"]) in wanted
    ]
    for index in sorted(targets, reverse=True):
        response = post_form(
            session, base, "/sensors/delete", {"index": str(index)}
        )
        require_status(response, (200, 302, 303), f"delete temporary index {index}")


def verify_text_limits(
    session: requests.Session,
    base: str,
    original_forms: Dict[int, Dict[str, str]],
) -> None:
    oversized_add = {
        "name": "N" * 97,
        "deviceType": "1",
        "serialNumber": "EFFFFF",
        "deviceKey": "A0000000",
        "customUrl": "",
    }
    response = post_form(session, base, "/sensors/add", oversized_add)
    require_status(response, (400,), "reject oversized sensor name")

    if original_forms:
        sensor_index = sorted(original_forms)[0]
        original = dict(original_forms[sensor_index])
        oversized_update = dict(original)
        oversized_update["customUrl"] = "http://example.invalid/" + ("u" * 513)
        response = post_form(
            session, base, "/sensors/update", oversized_update
        )
        require_status(response, (400,), "reject oversized custom URL")
        current = edit_form(session, base, sensor_index)
        if current is None or current.get("customUrl") != original.get("customUrl"):
            raise TestFailure(
                "oversized custom URL changed the stored sensor configuration"
            )


def reboot_and_wait(session: requests.Session, base: str, expected_version: str) -> None:
    try:
        response = post_form(session, base, "/reboot", {})
        require_status(response, (200, 202, 302, 303), "reboot")
    except requests.RequestException:
        # A reset can close the socket before the deferred response is drained.
        pass

    time.sleep(2)
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        try:
            response = session.get(base + "/firmware/version", timeout=3)
            if response.status_code == 200:
                version = response.json().get("version")
                if version != expected_version:
                    raise TestFailure(
                        f"gateway returned after reboot with version {version!r}, "
                        f"expected {expected_version!r}"
                    )
                return
        except requests.RequestException:
            pass
        time.sleep(1)
    raise TestFailure("gateway did not return within 90 seconds after reboot")


def current_log_level(session: requests.Session, base: str) -> str:
    response = session.get(base + "/logs", timeout=5)
    require_status(response, (200,), "read log level")
    match = re.search(r"<option value='([^']+)' selected", response.text)
    return match.group(1) if match else "INFO"


def log_snapshot_race(
    base: str, username: str, credential: str, session: requests.Session
) -> None:
    previous_level = current_log_level(session, base)
    require_status(
        post_form(session, base, "/logs/level", {"level": "DEBUG"}),
        (200, 302, 303),
        "enable debug logging",
    )

    def request_once(sequence: int) -> float:
        started = time.monotonic()
        if sequence % 4 == 0:
            response = requests.post(
                base + "/logs/clear",
                auth=(username, credential),
                headers={"Origin": base},
                timeout=8,
            )
        else:
            response = requests.get(
                base + "/logs", auth=(username, credential), timeout=8
            )
        require_status(response, (200, 302, 303), f"log race request {sequence}")
        if response.status_code == 200 and sequence % 4:
            if "System Logs" not in response.text:
                raise TestFailure(f"log race request {sequence}: malformed page")
        return time.monotonic() - started

    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as executor:
            latencies = list(executor.map(request_once, range(72)))
        if max(latencies) > 7.5:
            raise TestFailure(f"log race stalled for {max(latencies):.2f}s")
    finally:
        post_form(session, base, "/logs/level", {"level": previous_level})


class SlowHandler(BaseHTTPRequestHandler):
    hit = threading.Event()

    def do_GET(self) -> None:  # noqa: N802
        type(self).hit.set()
        time.sleep(2.5)
        body = b"ok"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, _format: str, *_args: object) -> None:
        return


def route_local_ip(gateway_host: str) -> str:
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect((gateway_host, 80))
        return str(probe.getsockname()[0])
    finally:
        probe.close()


def forwarding_does_not_hold_sensor_lock(
    session: requests.Session,
    base: str,
    original_forms: Dict[int, Dict[str, str]],
    timeout: int,
) -> float:
    if not original_forms:
        raise TestFailure("no existing real sensor is available for forwarding test")

    sensor_index = sorted(original_forms)[0]
    original = dict(original_forms[sensor_index])
    gateway_host = urlparse(base).hostname
    if not gateway_host:
        raise TestFailure("gateway URL has no hostname")

    local_ip = route_local_ip(gateway_host)
    SlowHandler.hit.clear()
    server = ThreadingHTTPServer(("0.0.0.0", 0), SlowHandler)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()

    changed = dict(original)
    changed["customUrl"] = (
        f"http://{local_ip}:{server.server_address[1]}/issue23"
        "?serial=*SN*&temperature=*TEMP*"
    )
    try:
        require_status(
            post_form(session, base, "/sensors/update", changed),
            (200, 302, 303),
            "configure temporary slow forwarding URL",
        )
        if not SlowHandler.hit.wait(timeout):
            raise TestFailure(
                f"no real sensor packet reached the slow endpoint within {timeout}s"
            )

        started = time.monotonic()
        response = session.get(base + "/api?format=json", timeout=2)
        latency = time.monotonic() - started
        require_status(response, (200,), "API during slow forwarding")
        if latency > 1.0:
            raise TestFailure(
                f"/api blocked for {latency:.2f}s while custom HTTP was in progress"
            )
        return latency
    finally:
        try:
            post_form(session, base, "/sensors/update", original)
        finally:
            server.shutdown()
            server.server_close()
            server_thread.join(timeout=3)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True)
    parser.add_argument("--admin-username", default="admin")
    parser.add_argument("--admin-credential", required=True)
    parser.add_argument("--packet-timeout", type=int, default=300)
    args = parser.parse_args()

    base = args.url.rstrip("/")
    session = requests.Session()
    session.auth = (args.admin_username, args.admin_credential)
    session.headers.update({"User-Agent": "explora-issue23-hw-test/1.0"})

    version_response = session.get(base + "/firmware/version", timeout=5)
    require_status(version_response, (200,), "firmware version")
    version = str(version_response.json().get("version", ""))
    if "storage" not in version:
        raise TestFailure(f"expected storage test firmware, got {version!r}")

    originals = configured_forms(session, base)
    original_serials = {
        normal_serial(form["serialNumber"]) for form in originals.values()
    }
    if len(originals) >= MAX_SENSORS:
        raise TestFailure("gateway already has 20 sensors; cannot create isolated fixtures")

    temporary_serials: List[str] = []
    try:
        print("[0/5] Rejecting oversized persisted text fields")
        verify_text_limits(session, base, originals)

        print(f"[1/5] Filling {MAX_SENSORS - len(originals)} temporary sensor slots")
        temporary_serials = add_fake_sensors(
            session, base, original_serials, MAX_SENSORS - len(originals)
        )
        filled = configured_forms(session, base)
        if len(filled) != MAX_SENSORS:
            raise TestFailure(f"expected 20 configured sensors, found {len(filled)}")

        print("[2/5] Rebooting and verifying all 20 large records")
        reboot_and_wait(session, base, version)
        persisted = configured_forms(session, base)
        persisted_serials = {
            normal_serial(form["serialNumber"]) for form in persisted.values()
        }
        expected_serials = original_serials | {
            normal_serial(value) for value in temporary_serials
        }
        if len(persisted) != MAX_SENSORS or persisted_serials != expected_serials:
            raise TestFailure("sensor set changed across the full-capacity reboot")

        print("[3/5] Racing log snapshots against log clearing")
        log_snapshot_race(
            base, args.admin_username, args.admin_credential, session
        )

        print("[4/5] Verifying API responsiveness during slow custom HTTP forwarding")
        latency = forwarding_does_not_hold_sensor_lock(
            session, base, originals, args.packet_timeout
        )
        print(f"      /api latency while forwarding: {latency:.3f}s")

        print("[5/5] Checking gateway health after stress")
        health = session.get(base + "/firmware/version", timeout=5)
        require_status(health, (200,), "post-stress health")
    finally:
        print("[cleanup] Removing only issue #23 temporary sensors")
        try:
            delete_serials(session, base, temporary_serials)
            remaining = configured_forms(session, base)
            remaining_serials = {
                normal_serial(form["serialNumber"]) for form in remaining.values()
            }
            if remaining_serials != original_serials:
                raise TestFailure("cleanup did not restore the original sensor set")
        except requests.RequestException as error:
            raise TestFailure(f"cleanup failed: {error}") from error

    print("PASS: issue #23 hardware regression suite completed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
