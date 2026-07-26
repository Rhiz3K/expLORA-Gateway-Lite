# expLORA Gateway Lite

![expLORA Logo](https://pajenicko.cz/image/catalog/explora/explora-logo-sm.png)

## Overview

expLORA Gateway Lite is a versatile IoT gateway designed for wireless sensor networks. Built on ESP32 with LoRa capabilities, it serves as a bridge between low-power sensor nodes and home automation systems or custom applications.

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

- **MQTT Integration**:
  - Home Assistant auto-discovery
  - Configurable broker settings
  - Real-time data publication

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

## Hardware Requirements

- ESP32 development board (with PSRAM recommended)
- RFM95W LoRa module
- Supporting components as per the wiring diagram

## Installation

1. **Clone the repository**:
   ```bash
   git clone https://github.com/Pajenicko/expLORA-Gateway-Lite.git
   cd expLORA-Gateway-Lite
   ```

2. **Install dependencies**:
   The project uses the following libraries:
   - Arduino ESP32 core
   - ArduinoJson
   - ESPAsyncWebServer
   - AsyncTCP
   - PubSubClient (for MQTT)
   - WiFi
   - SPI

3. **Configure your hardware**:
   - Check `config.h` for pin definitions
   - Adjust pins if necessary to match your wiring

4. **Build and flash**:
   - Use PlatformIO (recommended) or Arduino IDE
   - Select the appropriate ESP32 board
   - Upload the firmware

## Initial Setup

1. **First Boot**:
   - Connect the gateway to USB and open a 115200-baud serial monitor.
   - The gateway creates separate random per-device credentials in ESP32 NVS
     for its WPA2 recovery AP and HTTP administrator.
   - Record the printed `AP SSID`, `Admin username` (`admin`),
     `New AP credential`, and `New admin credential`. Existing secrets are not
     repeated on later boots.
   - Connect to the WPA2 network named `expLORA-GW-XXXXXX` using the AP
     credential. The firmware fails closed if NVS cannot persist either
     credential; it never falls back to an open AP or shared password.

2. **Configure WiFi**:
   - Open a browser and navigate to 192.168.4.1
   - Enter the serial-console administrator username and admin credential
     when the browser presents the HTTP Basic authentication prompt.
   - Enter your home WiFi credentials
   - Save and reboot

3. **Access the Web Interface**:
   - Once connected to your network, find the gateway's IP from your router
   - Access the web interface by entering the IP in a browser
   - Use the same HTTP administrator credentials on the home network.

Keep both credentials private. The authenticated **Security** page rotates
either one and displays its replacement once; AP rotation reboots the gateway.
Erasing NVS creates new credentials and requires physical USB access to
retrieve them. Erasing only LittleFS does not intentionally reset them.

## Adding Sensors

1. Navigate to the "Sensors" page in the web interface
2. Click "Add New Sensor"
3. Enter the required details:
   - Name: A friendly name for the sensor
   - Device Type: Select the appropriate sensor type
   - Serial Number: The unique identifier of the sensor (in hexadecimal)
   - Device Key: The encryption key used by the sensor (in hexadecimal)
4. Optionally configure:
   - Custom URL: For forwarding data to external services
   - Altitude: For pressure sensors, to adjust for elevation
   - Correction values: To calibrate sensor readings

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
   - Port: Usually 1883 (default) or 8883 (TLS)
   - Username and Password: If your broker requires authentication
4. Save the configuration

The gateway will automatically generate discovery information for Home Assistant and publish sensor data to appropriate topics.

## API Usage

The gateway provides an authenticated REST API for programmatic access to
sensor data. For example:

```bash
curl --user admin http://192.168.1.42/api?format=json
```

`curl` prompts for the per-device administrator credential when it is omitted from
the command line.

- **List all sensors**: `/api?format=json`
- **Get specific sensor**: `/api?sensor=XXXXXX&format=json` (where XXXXXX is the sensor's serial number in hex)
- **CSV format**: Replace `json` with `csv` in the URL

The API is same-origin by default and does not emit wildcard CORS headers.
All web management and telemetry routes require HTTP Basic authentication,
regardless of whether they are reached through the AP or STA interface.

State-changing operations use POST. In particular:

```text
POST /reboot
POST /sensors/delete   (form field: index)
POST /logs/clear
```

Browser POSTs carrying an `Origin` header are accepted only when its authority
matches the request `Host`. Both direct HTTP and HTTPS terminated by a reverse
proxy are supported; an explicitly supplied port must still match. Authenticated
non-browser clients that omit `Origin` remain supported. This Origin/Host check
supplements Basic authentication with a practical CSRF defense. HTTP Basic
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
`/webflash/explora-gw-lite/`; redirects, userinfo, non-default ports, encoded
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
shasum -a 256 .pio/build/esp32-s3-devkitc-1/firmware.bin \
  > .pio/build/esp32-s3-devkitc-1/firmware.sha256
```

The gateway itself serves HTTP on port 80. An HTTPS reverse proxy must forward
an explicit external port in `Host` (for example `gateway.local:443`) so the
Origin/Host CSRF check can compare the complete external origin.

Example API response:
```json
{
  "version": "1.0.6",
  "time": "2025-05-17 15:30:45",
  "status": "connected",
  "sensors": [
    {
      "name": "Living Room",
      "type": 1,
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
