#!/usr/bin/env python3
"""Focused hardware checks for GitHub issue #24.

The test validates add/edit input rejection, confirms that the reserved
VEML7700 protocol type stays out of the supported UI, exercises a temporary
CLIMA configuration through persistence and rendering, and checks the
truthful TLS UI copy. The fixture is deleted in ``finally``.
"""

from __future__ import annotations

import argparse
import re
from typing import Optional

import requests


FIXTURE_SERIAL = "EFFF24"
FIXTURE_NAME = "issue24-clima-fixture"


class TestFailure(RuntimeError):
    pass


def require_status(response: requests.Response, expected: int, label: str) -> None:
    if response.status_code != expected:
        raise TestFailure(
            f"{label}: HTTP {response.status_code}, body={response.text[:160]!r}"
        )


def serials(session: requests.Session, base: str) -> set[str]:
    response = session.get(base + "/api?format=json", timeout=5)
    require_status(response, 200, "read sensor API")
    return {
        str(sensor.get("serialNumber", "")).upper().lstrip("0") or "0"
        for sensor in response.json().get("sensors", [])
    }


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


def find_fixture_index(
    session: requests.Session, base: str
) -> Optional[int]:
    serial_pattern = re.compile(
        r"id='serialNumber'[^>]*value='" + re.escape(FIXTURE_SERIAL) + r"'"
    )
    for index in range(20):
        response = session.get(
            f"{base}/sensors/edit?index={index}",
            allow_redirects=False,
            timeout=5,
        )
        if response.status_code == 200 and serial_pattern.search(response.text):
            return index
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True)
    parser.add_argument("--admin-username", default="admin")
    parser.add_argument("--admin-credential", required=True)
    args = parser.parse_args()

    base = args.url.rstrip("/")
    session = requests.Session()
    session.auth = (args.admin_username, args.admin_credential)
    session.headers.update({"User-Agent": "explora-issue24-hw-test/1.0"})

    version = session.get(base + "/firmware/version", timeout=5).json()["version"]
    if "correctness" not in str(version):
        raise TestFailure(f"expected correctness firmware, got {version!r}")

    baseline = serials(session, base)
    fixture_index: Optional[int] = None
    try:
        print("[1/5] Rejecting invalid add inputs")
        invalid_serial = post_form(
            session,
            base,
            "/sensors/add",
            {
                "name": "invalid-serial-fixture",
                "deviceType": "1",
                "serialNumber": "GGGGGG",
                "deviceKey": "A0B0C0D0",
            },
        )
        require_status(invalid_serial, 400, "invalid add serial")
        for invalid_type_value in ("4", "99", "257", "1junk"):
            invalid_type = post_form(
                session,
                base,
                "/sensors/add",
                {
                    "name": "invalid-type-fixture",
                    "deviceType": invalid_type_value,
                    "serialNumber": "EFFF23",
                    "deviceKey": "A0B0C0D0",
                },
            )
            require_status(
                invalid_type,
                400,
                f"invalid add type {invalid_type_value!r}",
            )
        if serials(session, base) != baseline:
            raise TestFailure("invalid add changed configured sensors")

        print("[2/5] Adding and persisting a supported CLIMA fixture")
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
                f"CLIMA add: HTTP {added.status_code}, body={added.text[:160]!r}"
            )
        fixture_index = find_fixture_index(session, base)
        if fixture_index is None:
            raise TestFailure("CLIMA fixture has no editable persisted record")

        edit_page = session.get(
            f"{base}/sensors/edit?index={fixture_index}", timeout=5
        )
        require_status(edit_page, 200, "read CLIMA edit page")
        if not re.search(
            r"<option value='1' selected>CLIMA\s+-\s+Temperature, Humidity, Pressure",
            edit_page.text,
        ):
            raise TestFailure("CLIMA is not selected in the edit form")

        api_response = session.get(
            f"{base}/api?sensor={FIXTURE_SERIAL}&format=json", timeout=5
        )
        require_status(api_response, 200, "read CLIMA API")
        matching = [
            sensor
            for sensor in api_response.json().get("sensors", [])
            if str(sensor.get("serialNumber", "")).upper() == FIXTURE_SERIAL
        ]
        if len(matching) != 1 or matching[0].get("typeName") != "CLIMA":
            raise TestFailure("CLIMA capability is absent from the API")

        print("[3/5] Rejecting invalid edit inputs without mutation")
        invalid_edit_serial = post_form(
            session,
            base,
            "/sensors/update",
            {
                "index": str(fixture_index),
                "name": FIXTURE_NAME,
                "deviceType": "1",
                "serialNumber": "12X456",
                "deviceKey": "",
            },
        )
        require_status(invalid_edit_serial, 400, "invalid edit serial")
        for invalid_type_value in ("4", "99", "257", "1junk"):
            invalid_edit_type = post_form(
                session,
                base,
                "/sensors/update",
                {
                    "index": str(fixture_index),
                    "name": FIXTURE_NAME,
                    "deviceType": invalid_type_value,
                    "serialNumber": FIXTURE_SERIAL,
                    "deviceKey": "",
                },
            )
            require_status(
                invalid_edit_type,
                400,
                f"invalid edit type {invalid_type_value!r}",
            )
        if find_fixture_index(session, base) != fixture_index:
            raise TestFailure("invalid edit changed the CLIMA fixture")

        print("[4/5] Checking supported sensor options and verified-TLS UI")
        sensors_page = session.get(base + "/sensors", timeout=5)
        require_status(sensors_page, 200, "read sensors page")
        if FIXTURE_NAME not in sensors_page.text or "CLIMA" not in sensors_page.text:
            raise TestFailure("CLIMA fixture is not rendered on the sensors page")
        add_page = session.get(base + "/sensors/add", timeout=5)
        require_status(add_page, 200, "read add-sensor page")
        if re.search(r"<option value='4'(?:\s|>)", add_page.text):
            raise TestFailure("reserved VEML7700 type is exposed as supported")
        mqtt_page = session.get(base + "/mqtt", timeout=5)
        require_status(mqtt_page, 200, "read MQTT page")
        if "Enable TLS (public CA verification)" not in mqtt_page.text:
            raise TestFailure("MQTT page does not describe certificate verification")
        if "built-in public CA store" not in mqtt_page.text:
            raise TestFailure("MQTT page does not explain its trust source")

        print("[5/5] Checking post-test firmware health")
        require_status(
            session.get(base + "/firmware/version", timeout=5),
            200,
            "post-test firmware health",
        )
    finally:
        if fixture_index is None:
            fixture_index = find_fixture_index(session, base)
        if fixture_index is not None:
            deleted = post_form(
                session,
                base,
                "/sensors/delete",
                {"index": str(fixture_index)},
            )
            if deleted.status_code not in (200, 302, 303):
                raise TestFailure(
                    f"fixture cleanup: HTTP {deleted.status_code}, "
                    f"body={deleted.text[:160]!r}"
                )
        if serials(session, base) != baseline:
            raise TestFailure("cleanup did not restore the original sensor set")

    print("PASS: issue #24 hardware regression suite completed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
