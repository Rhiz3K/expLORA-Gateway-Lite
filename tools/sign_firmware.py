#!/usr/bin/env python3
"""Create detached expLORA firmware metadata signatures.

The private P-256 key is supplied explicitly or through EXPLORA_SIGNING_KEY.
It is never generated in, copied to, or read from the repository by default.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
from urllib.parse import urlsplit


KEY_ID = "explora-fw-p256-2026-07"
VERSION_RE = re.compile(
    r"^[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?(?:\+[A-Za-z0-9.-]+)?$"
)


def canonical_metadata(version: str, sha256: str) -> bytes:
    return (
        f"expLORA-Gateway-Lite\nversion={version}\nsha256={sha256}\n"
    ).encode("ascii")


def official_firmware_url(value: str) -> bool:
    parsed = urlsplit(value)
    return (
        parsed.scheme == "https"
        and parsed.hostname == "pajenicko.cz"
        and parsed.port in (None, 443)
        and parsed.username is None
        and parsed.password is None
        and parsed.query == ""
        and parsed.fragment == ""
        and parsed.path.startswith("/webflash/explora-gw-lite/")
        and parsed.path.endswith(".bin")
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("firmware", type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument(
        "--key",
        type=Path,
        default=(
            Path(os.environ["EXPLORA_SIGNING_KEY"])
            if os.environ.get("EXPLORA_SIGNING_KEY")
            else None
        ),
        help="P-256 private PEM key (or set EXPLORA_SIGNING_KEY)",
    )
    parser.add_argument("--url", help="official release .bin URL")
    parser.add_argument(
        "--output-prefix",
        type=Path,
        help="output prefix; defaults to the firmware path without .bin",
    )
    args = parser.parse_args()

    if args.key is None:
        parser.error("--key or EXPLORA_SIGNING_KEY is required")
    if not args.firmware.is_file():
        parser.error(f"firmware does not exist: {args.firmware}")
    if not args.key.is_file():
        parser.error(f"private key does not exist: {args.key}")
    if not VERSION_RE.fullmatch(args.version):
        parser.error("version is not valid semantic version metadata")
    if args.url and not official_firmware_url(args.url):
        parser.error("URL must be an official pajenicko.cz firmware .bin URL")

    if os.name == "posix":
        key_mode = stat.S_IMODE(args.key.stat().st_mode)
        if key_mode & 0o077:
            parser.error("private key permissions must not allow group/other access")

    image = args.firmware.read_bytes()
    sha256 = hashlib.sha256(image).hexdigest()
    metadata = canonical_metadata(args.version, sha256)
    result = subprocess.run(
        ["openssl", "dgst", "-sha256", "-sign", str(args.key)],
        input=metadata,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        raise SystemExit(
            "OpenSSL signing failed: " + result.stderr.decode(errors="replace")
        )
    signature = base64.b64encode(result.stdout).decode("ascii")

    prefix = args.output_prefix or args.firmware.with_suffix("")
    prefix.parent.mkdir(parents=True, exist_ok=True)
    signature_path = prefix.with_suffix(".sig")
    digest_path = prefix.with_suffix(".sha256")
    signature_path.write_text(signature + "\n", encoding="ascii")
    digest_path.write_text(sha256 + "\n", encoding="ascii")
    print(f"signature: {signature_path}")
    print(f"sha256:    {digest_path}")

    if args.url:
        manifest = {
            "version": args.version,
            "url": args.url,
            "sha256": sha256,
            "signature": signature,
            "keyId": KEY_ID,
        }
        manifest_path = prefix.with_suffix(".manifest.json")
        manifest_path.write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )
        print(f"manifest:  {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
