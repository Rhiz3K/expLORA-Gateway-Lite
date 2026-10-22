# expLORA Gateway

![expLORA Logo](https://pajenicko.cz/image/catalog/explora/explora-logo-sm.png)

## Overview

expLORA Gateway is a versatile IoT gateway designed for wireless sensor networks. Built on ESP32 with LoRa capabilities, it serves as a bridge between low-power sensor nodes and home automation systems or custom applications.

One firmware, two boards:

| | Lite | Pro |
|---|---|---|
| MCU | ESP32-S3 | ESP32-WROOM-32E (16 MB flash, no PSRAM) |
| LoRa | RFM95W (SX1276) | E22-900M22S (SX1262, TCXO) |
| Network | WiFi | Ethernet (LAN8720A), WiFi, LTE (SIMCom A7682E, optional) |
| Power | USB | PoE or USB |
| PlatformIO env | `lite` | `pro` |

The gateway receives data from various wireless sensors via LoRa, processes the information, and can forward it to:
- Home Assistant via MQTT
- Custom HTTP endpoints
- Local web interface

## Features

- **Multi-Sensor Support**: Compatible with various sensor types:
  - CLIMA (BME280) - Temperature, humidity, pressure
  - CARBON (SCD40) - Temperature, humidity, CO2
  - METEO - Weather station (temperature, humidity, pressure, wind, rain)
  - and more to come (you can also add new sensor, pull request are welcomed)

- **Web Interface**:
  - Responsive design
  - Real-time sensor monitoring
  - Sensor management (add, edit, delete)
  - Configuration settings
  - Per-device HTTP administrator authentication

- **Network Connectivity**:
  - Dual-mode operation (AP and client)
  - Captive portal for easy initial setup
  - Automatic reconnection
  - Pro: Ethernet, WiFi and LTE with automatic failover (see [Network](#network-pro))

- **MQTT Integration**:
  - Home Assistant auto-discovery
  - Configurable broker settings
  - Real-time data publication
  - Gateway diagnostics (uplink, IP address; Pro: power source, LTE signal and state)

- **Data Correction**:
  - Calibration options for sensor readings
  - Independent corrections for each measurement type
  - Original values logged for reference

- **Logging System**:
  - Multiple log levels (ERROR, WARNING, INFO, DEBUG, VERBOSE)
  - Web-based log viewer

- **API Access**:
  - JSON and CSV formats
  - Query parameters for filtering

## Hardware

- **Lite**: ESP32-S3 with an RFM95W LoRa module, see `hw/` for the schematic
- **Pro**: EXPLORA GATEWAY PRO board with ESP32-WROOM-32E-N16, E22-900M22S LoRa
  module, LAN8720A Ethernet PHY with PoE, and a slot for the A7682E LTE carrier

Pin assignments live in `variants/<board>/board_config.h`.

## Installation

1. **Clone the repository**:
   ```bash
   git clone https://github.com/Pajenicko/expLORA-Gateway.git
   cd expLORA-Gateway
   ```

2. **Build and flash** with [PlatformIO](https://platformio.org/) (Arduino core 3.x via pioarduino):
   ```bash
   pio run -e lite -t upload     # expLORA Gateway Lite
   pio run -e pro -t upload      # expLORA Gateway Pro
   ```
   Libraries (ArduinoJson, ESPAsyncWebServer, AsyncTCP, PubSubClient and RadioLib,
   which drives both radios) are installed automatically.

3. **Firmware updates** can also be installed from the web interface (Firmware page):
   upload a binary, or download one over HTTPS (the server certificate is
   checked against the built-in CA bundle). Images for the other board are
   refused.

4. **Unit tests** run on the host: `pio test -e native`. An end-to-end test against
   a real device is in `tests/integration`.

## Initial Setup

1. **First Boot**:
   - Connect the gateway to USB and open a 115200-baud serial monitor.
   - The gateway creates separate random per-device credentials in ESP32 NVS
     for its WPA2 recovery AP and HTTP administrator.
   - Record the printed `AP SSID`, `New AP credential`, and
     `New admin credential`. Existing secrets are not
     repeated on later boots.
   - Connect to the WPA2 network named `expLORA-GW-XXXXXX` using the AP
     credential. The firmware fails closed if NVS cannot persist either
     credential; it never falls back to an open AP or shared password.

2. **Log in**:
   - Open a browser and navigate to 192.168.4.1
   - Enter the administrator credential printed on the USB serial console
     in the login form. Existing administrator passwords are preserved when
     upgrading and stored as a salted PBKDF2 hash.

3. **Configure WiFi**:
   - Enter your home WiFi credentials
   - Save and reboot

4. **Access the Web Interface**:
   - Once connected to your network, find the gateway's IP from your router
   - Access the web interface by entering the IP in a browser
   - Use the same administrator password on the home network.

Keep both credentials private. The authenticated **Security** page rotates
either one and displays its replacement once; AP rotation reboots the gateway.
Erasing NVS creates new credentials and requires physical USB access to
retrieve them. Erasing only LittleFS does not intentionally reset them.

With Ethernet connected (Pro), the gateway gets an address from DHCP right away;
WiFi setup is optional. The access point stays up for 5 minutes after the
gateway reaches the local network.

## Login

The setup access point uses a per-device WPA2 password separate from the
administrator password. Both are generated and persisted before networking
starts; a storage failure never falls back to an open access point.

The web interface requires the admin password. Fresh devices print their new
administrator password once on USB serial. Gateways updated from this PR's
older Basic-auth firmware keep their existing password, migrated to a salted
hash. Sessions last until logout, 24 hours of inactivity or a restart; five
wrong passwords lock the login for a minute.

The sensor data API (`/api`) and `/firmware/version` stay public (read-only).

**Forgotten password:** connect the gateway over USB and send `reset-password`
on the serial console (115200 Bd). A replacement password is printed over USB;
sensors and settings are kept.

## Network (Pro)

The Pro board can use three uplinks. The first one that has an IP address carries
the traffic:

1. **Ethernet** - plug in a cable (PoE powers the board as well); DHCP by
   default, a static address can be set on the Network page
2. **WiFi** - configured on the Network page
3. **LTE** - needs the A7682E module and a SIM card. LTE is disabled by
   default; enable it on the Network page. The APN is detected from the network
   when left empty - set it only if the operator requires a specific one. Enter
   the SIM PIN if the card has one

All configured uplinks stay connected, so switching takes only a moment when a
cable is unplugged or WiFi drops. The LTE connection is used only when neither
Ethernet nor WiFi is available. The access point and captive portal come back
when the gateway loses its local network (Ethernet and WiFi), so it can always
be reached for configuration; the Network page is available on the access point
too (http://192.168.4.1/network).

If a wrong static Ethernet address makes the gateway unreachable, restart it:
the access point runs for 5 minutes after boot, so the address can be fixed on
its Network page.

The Network page and `/diag/network` (JSON) show the state of each interface,
the LTE operator and signal, and the power source (PoE or USB).

## Adding Sensors

1. Navigate to the "Sensors" page in the web interface
2. Click "Add New Sensor"
3. Enter the required details:
   - Name: A friendly name for the sensor
   - Device Type: Select the appropriate sensor type
   - Serial Number: The unique identifier of the sensor (in hexadecimal)
   - Device Key: The encryption key used by the sensor (in hexadecimal)
4. Optionally configure:
   - Custom URL: For forwarding data to external services (http:// or https://;
     HTTPS peers must present a certificate trusted by the built-in public CA bundle)
   - Altitude: For pressure sensors, to adjust for elevation
   - Correction values: To calibrate sensor readings

The daily rain total of a meteo station restarts at local midnight. That needs
the clock (NTP): without a synchronized time the total keeps adding up.

## Sensor Calibration

The gateway supports calibration of sensor readings through correction values:

- **Temperature, Humidity, Pressure, CO2, Light**: Offset corrections (±)
  - Example: A correction of +1.5 for temperature adds 1.5°C to the reported value

- **Wind Speed, Rain Amount, Rain Rate**: Multiplier corrections
  - Example: A correction of 1.1 for wind speed multiplies the value by 1.1 (10% increase)

- **Wind Direction**: Degree offset (±)
  - Example: A correction of +10 rotates the direction 10 degrees clockwise

Original (uncorrected) values are logged for reference while corrected values are stored and used for all operations.

## MQTT Integration

To connect to a Home Assistant MQTT broker:

1. Navigate to the "MQTT" page in the web interface
2. Enable MQTT integration
3. Enter your MQTT broker details:
   - Host: The IP or hostname of your MQTT broker
   - Port: Usually 1883 (default) or 8883 (TLS with public CA certificate verification)
   - Username and Password: If your broker requires authentication
4. Save the configuration

The gateway will automatically generate discovery information for Home Assistant and publish sensor data to appropriate topics.

## API Usage

The gateway provides a read-only REST API for sensor data (no login needed):

- **List all sensors**: `/api?format=json`
- **Get specific sensor**: `/api?sensor=XXXXXX&format=json` (where XXXXXX is the sensor's serial number in hex)
- **CSV format**: Replace `json` with `csv` in the URL

The public read-only API permits cross-origin reads. Management and OTA
responses do not emit wildcard CORS headers.
Web management routes require a valid login session, regardless of which
network interface serves them. `/api` and `/firmware/version` remain public
read-only endpoints, matching the upstream API contract.

State-changing operations use POST. In particular:

```text
POST /reboot
POST /sensors/delete   (form fields: index, sn)
POST /logs/clear
```

Browser POSTs carrying an `Origin` header are accepted only when its authority
matches the request `Host`. Both direct HTTP and HTTPS terminated by a reverse
proxy are supported; an explicitly supplied port must still match. Authenticated
non-browser clients that omit `Origin` remain supported. This Origin/Host check
supplements session authentication with a practical CSRF defense. Login
credentials are only transport-protected when the management connection itself
uses HTTPS, so remote deployments should terminate TLS at a trusted local proxy.

Stored WiFi and MQTT passwords and sensor device keys are never rendered back
into HTML. Blank password/key fields preserve an existing secret. WiFi and
MQTT forms provide an explicit clear-password checkbox; a nonblank sensor key
explicitly replaces the existing key.

## Firmware update security

Both manual firmware uploads and URL-based updates require administrator
authentication, the browser same-origin check, a newer semantic version, and
SHA-256 integrity before the candidate image is activated. Manual uploads are
therefore available to developers building the GPL source without a separate
release signing key.

URL updates accept only verified HTTPS URLs on exactly `pajenicko.cz`, below
the board's firmware directory (`/webflash/explora-gw-lite/` for Lite);
redirects, userinfo, non-default ports, encoded
paths, and alternate hosts are rejected. TLS uses ESP-IDF's Mozilla CA bundle
instead of one pinned root. Downloads run in a dedicated FreeRTOS task so the
management server remains responsive, with progress exposed at
`GET /firmware/status`. OTA remains available through the protected
recovery AP.

Manual upload state is isolated per HTTP request. Oversize data, failed writes,
bad digests, and client disconnects abort and release the OTA slot,
so a failed attempt cannot wedge later updates. A newly installed image is
confirmed only after critical initialization; ESP32 bootloader rollback remains
available until then.

Generate the detached digest required by the manual upload page with:

```bash
shasum -a 256 .pio/build/lite/firmware.bin \
  > .pio/build/lite/firmware.sha256
```

The gateway itself serves HTTP on port 80. An HTTPS reverse proxy must forward
an explicit external port in `Host` (for example `gateway.local:443`) so the
Origin/Host CSRF check can compare the complete external origin.

Example API response:
```json
{
  "version": "1.2.0",
  "time": "2026-10-17 15:30:45",
  "status": "connected",
  "sensors": [
    {
      "name": "Living Room",
      "deviceType": 1,
      "typeName": "CLIMA",
      "serialNumber": "123ABC",
      "temperature": 21.50,
      "humidity": 45.20,
      "pressure": 1013.20,
      "batteryVoltage": 3.82,
      "rssi": -72,
      "lastSeen": 30
    }
  ]
}
```

## Troubleshooting

### Common Issues

1. **Gateway not receiving data from sensors**:
   - Verify the Serial Number and Device Key are correct
   - Check that the sensor is within range
   - Ensure the sensor is transmitting (check battery status)

2. **Web interface not accessible**:
   - Confirm the gateway is connected to your network
   - Check for IP address conflicts
   - Try rebooting the gateway

3. **MQTT not connecting**:
   - Verify broker address and credentials
   - Check network connectivity
   - Confirm MQTT ports are not blocked by firewall

### Logs

The logging system is a valuable troubleshooting tool:

1. Navigate to the "Logs" page in the web interface
2. Set the appropriate log level:
   - **ERROR**: Only critical issues
   - **WARNING**: Errors and warnings
   - **INFO**: General operational information
   - **DEBUG**: Detailed information for debugging
   - **VERBOSE**: All available information

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## Development Tips

1. Use a quality antenna for the LoRa module for maximum range
2. When developing custom sensors, stick to the existing [expLORA protocol](https://github.com/Pajenicko/expLORA-Sensors/blob/main/explora-protocol.md) or prepare appropriate modifications for the gateway

## Acknowledgments

- Thanks to all contributors and testers
- Special thanks to the ESP32, Arduino, and LoRa communities
