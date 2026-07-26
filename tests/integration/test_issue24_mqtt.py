#!/usr/bin/env python3
"""Hardware-test per-sensor MQTT discovery without an external broker.

The script starts a minimal MQTT 3.1.1 broker on the test host, temporarily
points the gateway at it, adds a synthetic supported CLIMA sensor, and verifies
that the add handler immediately publishes retained Home Assistant discovery
configuration. No discovery metadata leaves the test LAN. The fixture and the
gateway's previous MQTT settings are restored in ``finally``; a blank password
submission preserves any stored password.
"""

from __future__ import annotations

import argparse
import html.parser
import json
import re
import socket
import threading
import time
import uuid
from dataclasses import dataclass
from typing import Optional

import requests


FIXTURE_SERIAL = "EFFE24"
FIXTURE_NAME = "issue24-mqtt-fixture"


class TestFailure(RuntimeError):
    pass


@dataclass
class Publish:
    topic: str
    payload: bytes
    retained: bool


class MqttFormParser(html.parser.HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.inputs: dict[str, dict[str, str]] = {}

    def handle_starttag(
        self, tag: str, attrs: list[tuple[str, Optional[str]]]
    ) -> None:
        if tag != "input":
            return
        values = {key: value or "" for key, value in attrs}
        name = values.get("name")
        if name:
            self.inputs[name] = values


class MiniBroker:
    def __init__(self, host: str, port: int) -> None:
        self._server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._server.bind((host, port))
        self._server.listen(2)
        self._server.settimeout(0.25)
        self.port = self._server.getsockname()[1]
        self.publishes: list[Publish] = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._connected = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self._thread.start()

    def close(self) -> None:
        self._stop.set()
        self._server.close()
        self._thread.join(timeout=2)

    def wait_connected(self, timeout: float) -> bool:
        return self._connected.wait(timeout)

    def snapshot(self) -> list[Publish]:
        with self._lock:
            return list(self.publishes)

    @staticmethod
    def _read_exact(client: socket.socket, length: int) -> bytes:
        data = bytearray()
        while len(data) < length:
            chunk = client.recv(length - len(data))
            if not chunk:
                raise ConnectionError("MQTT peer closed")
            data.extend(chunk)
        return bytes(data)

    @classmethod
    def _read_packet(cls, client: socket.socket) -> tuple[int, bytes]:
        first = cls._read_exact(client, 1)[0]
        remaining = 0
        multiplier = 1
        for _ in range(4):
            encoded = cls._read_exact(client, 1)[0]
            remaining += (encoded & 0x7F) * multiplier
            if not encoded & 0x80:
                return first, cls._read_exact(client, remaining)
            multiplier *= 128
        raise TestFailure("malformed MQTT remaining length")

    def _serve(self, client: socket.socket) -> None:
        client.settimeout(1)
        while not self._stop.is_set():
            try:
                first, body = self._read_packet(client)
            except socket.timeout:
                continue
            packet_type = first >> 4
            if packet_type == 1:  # CONNECT
                client.sendall(b"\x20\x02\x00\x00")
                self._connected.set()
            elif packet_type == 3:  # PUBLISH
                if len(body) < 2:
                    raise TestFailure("short MQTT PUBLISH")
                topic_length = int.from_bytes(body[:2], "big")
                topic_end = 2 + topic_length
                topic = body[2:topic_end].decode("utf-8")
                qos = (first >> 1) & 0x03
                payload_start = topic_end + (2 if qos else 0)
                with self._lock:
                    self.publishes.append(
                        Publish(topic, body[payload_start:], bool(first & 0x01))
                    )
                if qos == 1:
                    packet_id = body[topic_end : topic_end + 2]
                    client.sendall(b"\x40\x02" + packet_id)
            elif packet_type == 12:  # PINGREQ
                client.sendall(b"\xD0\x00")
            elif packet_type == 14:  # DISCONNECT
                return

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                client, _ = self._server.accept()
            except (socket.timeout, OSError):
                continue
            try:
                with client:
                    self._serve(client)
            except (ConnectionError, OSError, TestFailure):
                continue


def post_form(
    session: requests.Session, base: str, path: str, data: dict[str, str]
) -> requests.Response:
    return session.post(
        base + path,
        data=data,
        headers={"Origin": base},
        allow_redirects=False,
        timeout=8,
    )


def read_mqtt_form(
    session: requests.Session, base: str
) -> tuple[dict[str, str], set[str]]:
    response = session.get(base + "/mqtt", timeout=5)
    response.raise_for_status()
    parser = MqttFormParser()
    parser.feed(response.text)
    required = ("host", "port", "user", "prefix", "haPrefix")
    if any(name not in parser.inputs for name in required):
        raise TestFailure("MQTT configuration form is incomplete")
    fields = {name: parser.inputs[name].get("value", "") for name in required}
    checked = {
        name
        for name in ("enabled", "tls", "haEnabled")
        if "checked" in parser.inputs.get(name, {})
    }
    return fields, checked


def write_mqtt_form(
    session: requests.Session,
    base: str,
    fields: dict[str, str],
    checked: set[str],
) -> None:
    data = dict(fields)
    data["password"] = ""
    for name in checked:
        data[name] = "1"
    response = post_form(session, base, "/mqtt", data)
    if response.status_code not in (200, 302, 303):
        raise TestFailure(
            f"save MQTT settings: HTTP {response.status_code}, "
            f"body={response.text[:160]!r}"
        )


def find_fixture_index(
    session: requests.Session, base: str
) -> Optional[int]:
    pattern = re.compile(
        r"id='serialNumber'[^>]*value='" + re.escape(FIXTURE_SERIAL) + r"'"
    )
    for index in range(20):
        response = session.get(
            f"{base}/sensors/edit?index={index}",
            allow_redirects=False,
            timeout=5,
        )
        if response.status_code == 200 and pattern.search(response.text):
            return index
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True)
    parser.add_argument("--admin-username", default="admin")
    parser.add_argument("--admin-credential", required=True)
    parser.add_argument(
        "--broker-host",
        required=True,
        help="LAN address of this test host as reachable from the gateway",
    )
    parser.add_argument("--listen-host", default="0.0.0.0")
    parser.add_argument("--listen-port", type=int, default=0)
    args = parser.parse_args()

    base = args.url.rstrip("/")
    session = requests.Session()
    session.auth = (args.admin_username, args.admin_credential)
    session.headers.update({"User-Agent": "explora-issue24-mqtt-hw-test/1.0"})

    original_fields, original_checked = read_mqtt_form(session, base)
    broker = MiniBroker(args.listen_host, args.listen_port)
    broker.start()
    fixture_index: Optional[int] = None
    token = uuid.uuid4().hex[:10]
    prefix = f"codex_issue24_{token}"
    ha_prefix = f"codex_issue24_ha_{token}"

    try:
        print("[1/4] Connecting gateway to the isolated LAN MQTT broker")
        write_mqtt_form(
            session,
            base,
            {
                "host": args.broker_host,
                "port": str(broker.port),
                "user": "",
                "prefix": prefix,
                "haPrefix": ha_prefix,
            },
            {"enabled", "haEnabled"},
        )
        if not broker.wait_connected(15):
            raise TestFailure("gateway did not connect to the isolated MQTT broker")
        time.sleep(1)
        marker = len(broker.snapshot())

        print("[2/4] Adding a CLIMA sensor while MQTT is connected")
        added = post_form(
            session,
            base,
            "/sensors/add",
            {
                "name": FIXTURE_NAME,
                "deviceType": "1",
                "serialNumber": FIXTURE_SERIAL,
                "deviceKey": "A0B0C0D0",
            },
        )
        if added.status_code not in (200, 302, 303):
            raise TestFailure(
                f"add fixture: HTTP {added.status_code}, body={added.text[:160]!r}"
            )
        fixture_index = find_fixture_index(session, base)
        if fixture_index is None:
            raise TestFailure("temporary MQTT sensor was not persisted")

        print("[3/4] Verifying immediate retained discovery configuration")
        expected_types = {
            "temperature",
            "humidity",
            "pressure",
            "battery",
            "rssi",
        }
        expected_topics = {
            value_type: (
                f"{ha_prefix}/sensor/{prefix}_{FIXTURE_SERIAL}_"
                f"{value_type}/config"
            )
            for value_type in expected_types
        }
        deadline = time.monotonic() + 8
        matching: dict[str, Publish] = {}
        while time.monotonic() < deadline:
            for publish in broker.snapshot()[marker:]:
                for value_type, topic in expected_topics.items():
                    if publish.topic == topic:
                        matching[value_type] = publish
            if matching.keys() == expected_types:
                break
            time.sleep(0.1)
        if matching.keys() != expected_types:
            missing = sorted(expected_types - matching.keys())
            raise TestFailure(f"missing per-sensor discovery topics: {missing}")

        for value_type, publish in matching.items():
            if not publish.retained:
                raise TestFailure(f"{value_type} discovery was not retained")
            payload = json.loads(publish.payload)
            if payload.get("state_topic") != (
                f"{prefix}/{FIXTURE_SERIAL}/{value_type}"
            ):
                raise TestFailure(f"{value_type} state topic is incorrect")
            device = payload.get("device", {})
            if (
                device.get("name") != FIXTURE_NAME
                or device.get("model") != "CLIMA"
            ):
                raise TestFailure(f"{value_type} device metadata is incorrect")

        print("[4/4] Restoring the sensor set and MQTT configuration")
    finally:
        if fixture_index is None:
            fixture_index = find_fixture_index(session, base)
        if fixture_index is not None:
            deleted = post_form(
                session, base, "/sensors/delete", {"index": str(fixture_index)}
            )
            if deleted.status_code not in (200, 302, 303):
                raise TestFailure(
                    f"fixture cleanup: HTTP {deleted.status_code}, "
                    f"body={deleted.text[:160]!r}"
                )
        write_mqtt_form(
            session, base, original_fields, original_checked
        )
        broker.close()

    print("PASS: immediate per-sensor MQTT discovery verified on hardware")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
