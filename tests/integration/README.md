# expLORA Gateway integration tests

End-to-end Python smoke test for a running expLORA Gateway Lite device.
Complements the host-side `pio test -e native` unit tests (which only
cover pure logic in isolation) by exercising the full RX path on real
hardware: HTTP API, web UI, WiFi provisioning, LoRa decode, calibration.

## Quickstart (config-file mode, recommended)

```bash
# 1. Install Python deps
pip install requests pyserial

# 2. Copy the template, fill in your values
cp tests/integration/config.example.ini tests/integration/config.ini
$EDITOR tests/integration/config.ini

# 3. Plug the gateway into your Mac via USB and run
python3 tests/integration/test_gateway.py --config tests/integration/config.ini
```

`config.ini` is gitignored — your separate per-device AP/admin credentials,
home WiFi password, and sensor device key never get committed.

## What it tests

| Section | Cases | What it covers |
|---|---|---|
| Authentication/read-only smoke | 15 | unauthenticated 401 challenge on `/config` and `POST /firmware`, authenticated firmware version/manifest/status, rejected bad-signature cleanup, API, UI, diagnostics, logs, and 404 handling |
| POST validation/security | 5  | Empty SSID, short password, missing param, SSID > 32 chars, and authenticated cross-origin POST rejection |
| Live sensor round-trip | 4 | Adds the sensor → waits up to 20 min for a real LoRa packet → checks decoded values are in physical ranges → confirms sensor appears in `/sensors` HTML |
| Reboot recovery | 1 | authenticated `POST /reboot` → waits for device to drop off → waits up to 60 s for it to come back with the same version |

25 tests total when fully configured (sensor SN set, reboot enabled).

### Issue #23 storage/concurrency stress test

After flashing a build from the issue #23 branch, run the focused destructive
test separately:

```bash
python3 tests/integration/test_issue23.py \
  --url http://gateway-ip \
  --admin-credential '<admin credential>'
```

It temporarily fills all 20 sensor slots with large records, reboots and
checks persistence, races log snapshots against log clearing, and verifies
that the API remains responsive while a real sensor's custom HTTP request is
held open. It also verifies that oversized names and forwarding URLs are
rejected without changing stored configuration. Temporary sensors and the
custom URL are restored in `finally`; existing device keys are never read or
replaced.

If the gateway cannot route back to the test host (for example, across
isolated LAN subnets), add `--detect-forward-via-api`. The same real packet
is detected by polling its `lastSeen` reset while the temporary forwarding
request is in flight; the URL contains no sensor-value placeholders.

### Issue #24 correctness test

After flashing the issue #24 build, run:

```bash
python3 tests/integration/test_issue24.py \
  --url http://gateway-ip \
  --admin-credential '<admin credential>'
```

It checks add/edit validation, persists and renders a temporary VEML7700
sensor, verifies the public-CA TLS wording, and removes the fixture in
`finally`.

To verify immediate per-sensor Home Assistant discovery against an isolated
MQTT broker running on the test host:

```bash
python3 tests/integration/test_issue24_mqtt.py \
  --url http://gateway-ip \
  --admin-credential '<admin credential>' \
  --broker-host '<test-host LAN IP>'
```

The broker is local and isolated, so no discovery metadata leaves the test
LAN. The script verifies a synthetic sensor and restores both the fixture and
the previous MQTT settings in `finally`.

## Provisioning flow (first run, gateway with no STA config)

When `[gateway].ip` is empty or unreachable, the script provisions the
device automatically:

1. Reads UART to discover the actual AP SSID (`expLORA-GW-XXXXXX`) and any
   newly generated AP/admin credentials.
2. macOS: switches WiFi via
   `networksetup -setairportnetwork en0 <ssid> <ap-credential>`
   Linux/Windows: prints `Connect to <ssid>` and waits for ENTER
3. Authenticates as `admin` and POSTs `[wifi].ssid` + `[wifi].password` to
   `http://192.168.4.1/config`
4. Watches UART for `expLORA Gateway Lite starting up` (= reboot)
5. Watches UART for `WiFi connected! IP: x.x.x.x` (= STA joined)
6. Switches host WiFi back to home
7. Runs the test suite against the discovered IP

After the first successful run, paste the discovered IP into
`[gateway].ip` of `config.ini` and subsequent runs skip provisioning
entirely (sub-second startup).

## Config file format

See `config.example.ini` for the full template with comments. Minimal
required fields:

```ini
[gateway]
# Printed only when generated; persist them in this gitignored file.
ap_credential =
admin_credential =

[wifi]
ssid = my-home-wifi
password = my-home-wifi-password

[test_sensor]
sn = 475481
key = D43C2780
type = 1
```

If `[gateway].ip` is empty, the script will provision. For an already
provisioned IP, set both credentials in the gitignored config. Existing values
are deliberately not repeated over serial on each boot. If `[test_sensor].sn`
is empty, the live-packet test is skipped.

## CLI overrides

Any value can also be passed on the command line (CLI wins over config):

```bash
python3 tests/integration/test_gateway.py --config tests/integration/config.ini \
    --packet-timeout 600 \
    --skip-reboot
```

## Always start from current firmware

Use the `[flash]` config section (or CLI flags) to reflash the device at
the start of each run. Three independent toggles:

| Mode | Effect |
|---|---|
| `before_test = true` | `pio run -e <env> -t upload` — preserves LittleFS so saved WiFi config still works on reboot |
| `+ rebuild = true`   | `pio run -e <env>` first — picks up any source edits |
| `+ erase = true`     | `pio run -e <env> -t erase` first — wipes LittleFS and NVS, generates new AP/admin credentials, and **forces re-provisioning** |

CLI equivalents:

```bash
# Reflash with whatever firmware.bin is already built
python3 tests/integration/test_gateway.py --config tests/integration/config.ini --flash

# Rebuild + flash + run
python3 tests/integration/test_gateway.py --config tests/integration/config.ini --rebuild

# Truly fresh every run (re-provisions WiFi from scratch)
python3 tests/integration/test_gateway.py --config tests/integration/config.ini --rebuild --erase
```

`--rebuild` and `--erase` imply `--flash`. The script locates the `pio`
binary at `~/.platformio/penv/bin/pio` (or PATH), and project root by
walking up until it finds `platformio.ini`.

## Legacy mode (no config file)

Backwards-compatible CLI-only mode (assumes gateway already on the home
network, no UART, no WiFi switching):

```bash
python3 tests/integration/test_gateway.py \
    --url http://192.168.1.42 \
    --admin-credential '<value-read-from-USB-serial>' \
    --test-sensor-sn 475481 \
    --test-sensor-key D43C2780
```

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `no serial port — set [gateway].serial_port` | Device not plugged in / wrong USB cable (charge-only) / autodetect missed it | Plug in via data cable; or set `[gateway].serial_port = /dev/cu.usbmodem...` |
| `WiFi switched but http://192.168.4.1 unreachable` | Gateway didn't actually broadcast AP, or macOS associated with a different network | Power-cycle the gateway; check `[gateway].wifi_interface` (usually `en0`) |
| `could not obtain both per-device credentials` | Serial capture began too late or credentials were already provisioned | Save `New AP credential` and `New admin credential` on first boot; otherwise rotate them in the Security page or erase NVS over USB |
| `device rebooted but did not join home WiFi within 30 s` | Wrong WiFi password, SSID typo, or gateway out of range of router | Double-check `[wifi].ssid` + `[wifi].password` |
| `no fresh packet from sensor … within 1200s` | Sensor offline / wrong key / out of range / too aggressive `packet_timeout` | Confirm sensor is alive (CR2032 battery), key matches gateway-side, increase `[options].packet_timeout` |
| `networksetup` prompts for password | macOS hasn't seen this network before | Type your user password once; subsequent runs are passwordless |

## What the script never does

- **Never commits credentials.** `config.ini` is gitignored.
- **Never deletes sensors.** Adding the test sensor is idempotent — re-runs
  skip the add step. Remove the sensor manually via `/sensors` if you want
  it gone.
- **Flashes only when explicitly requested.** `--flash`, `--rebuild`, or
  `[flash].before_test` are opt-in. The normal suite never installs an image;
  it does verify that a fake signature is rejected and the OTA lock is freed.

All management requests made by the suite use HTTP Basic authentication.
Browser-style mutations are protected by the firmware's strict Origin/Host
check; the Python client omits `Origin`, which is the documented policy for
direct authenticated clients. Reboot, sensor deletion, and log clear are
POST-only. Release validation should additionally exercise WPA2/AP shutdown,
the public CA chain, successful signed manual and URL OTA, and rollback on the
target hardware.
