/**
 * expLORA Gateway Lite
 *
 * Web portal implementation file
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2025
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "WebServer.h"
#include "HTMLGenerator.h"
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <ArduinoJson.h>
#include "../config.h"
#include "../Security/SecurityPolicy.h"

namespace
{
bool parseSupportedSensorType(const String &value, uint8_t &type)
{
    if (value.length() == 0 || value.length() > 3)
    {
        return false;
    }
    for (size_t index = 0; index < value.length(); ++index)
    {
        if (!isDigit(static_cast<unsigned char>(value.charAt(index))))
        {
            return false;
        }
    }

    const unsigned long parsed = strtoul(value.c_str(), nullptr, 10);
    if (parsed > UINT8_MAX || !isSupportedSensorType(static_cast<uint8_t>(parsed)))
    {
        return false;
    }
    type = static_cast<uint8_t>(parsed);
    return true;
}
} // namespace

// Constructor
WebPortal::WebPortal(SensorManager &sensors, Logger &log, String &ssid, String &password,
                     bool &config_mode, ConfigManager &config, String &tz)
    : mqttManager(nullptr), server(HTTP_PORT), sensorManager(sensors), logger(log), isAPMode(false),
      apRadioActive(false), wifiSSID(ssid), wifiPassword(password), configMode(config_mode),
      timezone(tz), configManager(config)
{
    otaStateMutex = xSemaphoreCreateMutex();
}

bool WebPortal::init()
{
    logger.info("Initializing web portal");

    // Derive the same AP name used at boot (see makeApName() in config.h)
    apName = makeApName();

    apRadioActive = WiFi.softAPIP() != IPAddress(0, 0, 0, 0);
    if (configMode || WiFi.status() != WL_CONNECTED)
    {
        setAPMode(true);
    }

    // Setup routes (idempotent)
    setupRoutes();

    // Start server once
    if (!serverStarted)
    {
        server.begin();
        serverStarted = true;
        logger.info("Web server started on port " + String(HTTP_PORT));
    }

    return true;
}

bool WebPortal::startAccessPoint(bool allowStation)
{
    if (apName.isEmpty())
    {
        apName = makeApName();
    }

    WiFi.mode(allowStation ? WIFI_AP_STA : WIFI_AP);
    return ensureAPUp();
}

void WebPortal::setupRoutes()
{
    if (routesRegistered)
    {
        return;
    }

    logger.info("Registering web server routes (once)");

    server.on("/", HTTP_GET, std::bind(&WebPortal::handleRoot, this, std::placeholders::_1));
    server.on("/config", HTTP_GET, std::bind(&WebPortal::handleConfig, this, std::placeholders::_1));
    server.on("/config", HTTP_POST, std::bind(&WebPortal::handleConfigPost, this, std::placeholders::_1));

    // Sensor management
    server.on("/sensors/add", HTTP_GET, std::bind(&WebPortal::handleSensorAdd, this, std::placeholders::_1));
    server.on("/sensors/add", HTTP_POST, std::bind(&WebPortal::handleSensorAddPost, this, std::placeholders::_1));
    server.on("/sensors/edit", HTTP_GET, std::bind(&WebPortal::handleSensorEdit, this, std::placeholders::_1));
    server.on("/sensors/update", HTTP_POST, std::bind(&WebPortal::handleSensorEditPost, this, std::placeholders::_1));
    server.on("/sensors/delete", HTTP_POST, std::bind(&WebPortal::handleSensorDelete, this, std::placeholders::_1));
    server.on("/sensors", HTTP_GET, std::bind(&WebPortal::handleSensors, this, std::placeholders::_1));

    // Logs
    server.on("/logs/clear", HTTP_POST, std::bind(&WebPortal::handleLogsClear, this, std::placeholders::_1));
    server.on("/logs/level", HTTP_POST, std::bind(&WebPortal::handleLogLevel, this, std::placeholders::_1));
    server.on("/logs", HTTP_GET, std::bind(&WebPortal::handleLogs, this, std::placeholders::_1));

    // MQTT
    server.on("/mqtt", HTTP_GET, std::bind(&WebPortal::handleMqtt, this, std::placeholders::_1));
    server.on("/mqtt", HTTP_POST, std::bind(&WebPortal::handleMqttPost, this, std::placeholders::_1));

    // Device access credentials
    server.on("/security", HTTP_GET, std::bind(&WebPortal::handleSecurity, this, std::placeholders::_1));
    server.on("/security/rotate", HTTP_POST, std::bind(&WebPortal::handleSecurityRotate, this, std::placeholders::_1));

    // API
    server.on("/api", HTTP_GET, std::bind(&WebPortal::handleAPI, this, std::placeholders::_1));

    // Diagnostics
    server.on("/diag/heap", HTTP_GET, std::bind(&WebPortal::handleDiagHeap, this, std::placeholders::_1));

    // Firmware update endpoints
    server.on("/firmware/version", HTTP_GET, std::bind(&WebPortal::handleFirmwareVersion, this, std::placeholders::_1));
    server.on("/firmware/check", HTTP_GET, std::bind(&WebPortal::handleFirmwareCheck, this, std::placeholders::_1));
    server.on("/firmware/status", HTTP_GET, std::bind(&WebPortal::handleFirmwareStatus, this, std::placeholders::_1));
    server.on("/firmware/update", HTTP_POST, std::bind(&WebPortal::handleFirmwareUpdate, this, std::placeholders::_1));
    server.on("/firmware", HTTP_GET, std::bind(&WebPortal::handleFirmwarePage, this, std::placeholders::_1));
    server.on("/firmware", HTTP_POST,
        [this](AsyncWebServerRequest *request) { this->handleFirmwareUploadComplete(request); },
        [this](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            this->handleFirmwareUploadChunk(request, filename, index, data, len, final);
        }
    );

    // Reboot
    server.on("/reboot", HTTP_POST, std::bind(&WebPortal::handleReboot, this, std::placeholders::_1));

    // Unknown pages (404)
    server.onNotFound(std::bind(&WebPortal::handleNotFound, this, std::placeholders::_1));

    routesRegistered = true;
    logger.info("Routes registered");
}

bool WebPortal::isRequestAuthorized(AsyncWebServerRequest *request, bool stateChanging) const
{
    if (!request->authenticate(HTTP_ADMIN_USERNAME,
                               configManager.getAdminCredential().c_str()))
    {
        return false;
    }

    if (!stateChanging || !request->hasHeader("Origin"))
    {
        return true;
    }

    const AsyncWebHeader *originHeader = request->getHeader("Origin");
    return originHeader != nullptr &&
           SecurityPolicy::isAllowedBrowserOrigin(originHeader->value().c_str(),
                                                  request->host().c_str());
}

bool WebPortal::authorizeRequest(AsyncWebServerRequest *request, bool stateChanging)
{
    if (!request->authenticate(HTTP_ADMIN_USERNAME,
                               configManager.getAdminCredential().c_str()))
    {
        request->requestAuthentication("expLORA Gateway", false);
        return false;
    }

    if (stateChanging && request->hasHeader("Origin"))
    {
        const AsyncWebHeader *originHeader = request->getHeader("Origin");
        if (originHeader == nullptr ||
            !SecurityPolicy::isAllowedBrowserOrigin(originHeader->value().c_str(),
                                                    request->host().c_str()))
        {
            request->send(403, "text/plain", "Cross-origin state-changing request rejected");
            return false;
        }
    }

    return true;
}

// Called from the main loop. Drives deferred restarts so handlers can safely
// schedule ESP.restart() without blocking the async response flush.
void WebPortal::handleClient()
{
    if (restartRequested && (long)(millis() - restartAt) >= 0)
    {
        logger.info("Deferred restart firing now");
        delay(50); // tiny window for any in-flight TCP to drain
        ESP.restart();
    }
}

// Ensure proper cleanup in the destructor
WebPortal::~WebPortal()
{
    // Stop DNS server
    dnsServer.stop();
    if (otaStateMutex != nullptr)
    {
        vSemaphoreDelete(otaStateMutex);
        otaStateMutex = nullptr;
    }
}

// Process DNS requests
void WebPortal::processDNS()
{
    dnsServer.processNextRequest();
    // called automatically in handleClient()
}

// Bring the AP up additively. Safe to call while STA is connected or trying
// to connect — does NOT tear down WiFi the way setupAP() does. Idempotent.
bool WebPortal::ensureAPUp()
{
    wifi_mode_t curMode = WiFi.getMode();
    bool needsApStart = !apRadioActive ||
                        WiFi.softAPIP() == IPAddress(0, 0, 0, 0);

    if (curMode == WIFI_OFF)
    {
        WiFi.mode(WIFI_AP);
        needsApStart = true;
    }
    else if (curMode == WIFI_STA)
    {
        // Add AP capability without dropping STA
        WiFi.mode(WIFI_AP_STA);
        needsApStart = true;
    }

    if (needsApStart)
    {
        WiFi.setTxPower(WIFI_POWER_19_5dBm);
        const String &credential = configManager.getApCredential();
        if (!SecurityPolicy::isAccessCredentialFormat(credential.c_str()))
        {
            logger.error("Refusing to start AP without a valid per-device credential");
            apRadioActive = false;
            return false;
        }

        bool ok = WiFi.softAP(apName.c_str(), credential.c_str(), 6, 0, 4);
        delay(100);
        apRadioActive = ok && WiFi.softAPIP() != IPAddress(0, 0, 0, 0);
        if (apRadioActive)
        {
            logger.info("Protected AP brought up: " + apName + " @ " + WiFi.softAPIP().toString());
        }
        else
        {
            logger.error("Failed to start protected AP: " + apName);
        }
    }

    return apRadioActive;
}

void WebPortal::shutdownAP()
{
    // Cleanup is deliberately unconditional. The captive UI flag may already
    // be false even while the physical AP remains active.
    dnsServer.stop();
    isAPMode = false;

    const bool hadAp = apRadioActive ||
                       WiFi.softAPIP() != IPAddress(0, 0, 0, 0) ||
                       WiFi.getMode() == WIFI_AP ||
                       WiFi.getMode() == WIFI_AP_STA;
    if (!hadAp)
    {
        apRadioActive = false;
        return;
    }

    // Removing only the AP mode bit from AP+STA leaves the established STA
    // association and stored STA credentials untouched.
    const bool stopped = WiFi.enableAP(false);
    apRadioActive = (WiFi.getMode() & WIFI_MODE_AP) != 0;
    if (stopped && !apRadioActive)
    {
        logger.info("Captive DNS stopped and AP radio shut down");
    }
    else
    {
        logger.error("AP shutdown failed; AP capability is still active");
    }
}

// Toggle the captive-portal UI/DNS layer. STA is intentionally left alone so
// the auto-reconnect path keeps working during transient outages.
void WebPortal::setAPMode(bool enable)
{
    if (enable)
    {
        if (!ensureAPUp())
        {
            isAPMode = false;
            return;
        }

        IPAddress apIP = WiFi.softAPIP();
        if (apIP != IPAddress(0, 0, 0, 0))
        {
            dnsServer.setTTL(30);
            dnsServer.start(DNS_PORT, "*", apIP);
            logger.info("DNS server (re)started on port " + String(DNS_PORT));
        }
        else
        {
            logger.warning("setAPMode(true): AP IP is 0.0.0.0, DNS not started");
        }
        isAPMode = true;
    }
    else
    {
        shutdownAP();
    }
}

void WebPortal::shutdownAccessPoint()
{
    shutdownAP();
}

// Get current mode
bool WebPortal::isInAPMode() const
{
    return isAPMode;
}

bool WebPortal::isAccessPointActive() const
{
    return apRadioActive;
}

// Restart web server
void WebPortal::restart()
{
    server.end();
    serverStarted = false;

    // Reset DNS server if it was running
    if (isAPMode)
    {
        dnsServer.stop();
    }

    // Re-initialize
    init();
}

// Process HTTP requests

// Root page
void WebPortal::handleRoot(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /");
    if (!authorizeRequest(request, false)) return;

    // In AP mode, redirect to config page
    if (isAPMode)
    {
        request->redirect("/config");
        return;
    }

    // Get list of sensors
    std::vector<SensorData> sensorsList = sensorManager.getActiveSensors();

    // Generate HTML
    String html = HTMLGenerator::generateHomePage(sensorsList);

    // Send response
    request->send(200, "text/html", html);
}

// Guard: if AP mode, redirect to /config
bool WebPortal::guardAPOnly(AsyncWebServerRequest *request)
{
    if (isAPMode)
    {
        request->redirect("/config");
        return true;
    }
    return false;
}

// WiFi configuration page
void WebPortal::handleConfig(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /config");
    if (!authorizeRequest(request, false)) return;

    String currentIP = isAPMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

    // Generate HTML (use isAPMode to drive UI state)
    String html = HTMLGenerator::generateConfigPage(
        wifiSSID, wifiPassword.length() > 0, isAPMode, currentIP, timezone);

    // Send response
    request->send(200, "text/html", html);
}

// Process WiFi configuration form
void WebPortal::handleConfigPost(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /config");
    if (!authorizeRequest(request, true)) return;

    if (!request->hasParam("ssid", true) || !request->hasParam("password", true))
    {
        request->send(400, "text/plain", "Missing parameters");
        return;
    }

    String newSsid = request->getParam("ssid", true)->value();
    newSsid.trim();
    const String submittedPassword = request->getParam("password", true)->value();
    const bool clearPassword = request->hasParam("clearPassword", true);
    String newPassword(SecurityPolicy::resolveSecret(
                           wifiPassword.c_str(), submittedPassword.c_str(), clearPassword)
                           .c_str());

    // Validate SSID: 1..32 chars (IEEE 802.11 limit)
    if (newSsid.length() == 0)
    {
        request->send(400, "text/plain", "SSID cannot be empty");
        return;
    }
    if (newSsid.length() > 32)
    {
        request->send(400, "text/plain", "SSID too long (max 32 characters)");
        return;
    }
    // Validate password: either empty (open network) or 8..63 chars (WPA2-PSK)
    if (newPassword.length() > 0 && newPassword.length() < 8)
    {
        request->send(400, "text/plain", "Password must be at least 8 characters or left empty for open networks");
        return;
    }
    if (newPassword.length() > 63)
    {
        request->send(400, "text/plain", "Password too long (max 63 characters)");
        return;
    }

    if (request->hasParam("timezone", true))
    {
        timezone = request->getParam("timezone", true)->value();
    }

    logger.info("New WiFi configuration received (SSID length: " +
                String(newSsid.length()) + ")");
    configManager.setWiFiConfig(newSsid, newPassword, false);
    if (!configManager.save())
    {
        request->send(500, "text/plain", "Failed to save configuration");
        return;
    }

    // Build confirmation page
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta http-equiv='refresh' content='10;url=/'>";
    html += "<title>Configuration Saved</title></head>";
    html += "<body><h1>Configuration Saved</h1>";
    html += "<p>New WiFi settings have been saved. The device will restart in a few seconds.</p>";
    html += "</body></html>";

    request->send(200, "text/html", html);

    // Defer the restart so the async server can finish flushing the response.
    // handleClient() (called from the main loop) will trigger ESP.restart()
    // once restartAt has been reached.
    restartAt = millis() + 1500;
    restartRequested = true;
}

// Sensors list page
void WebPortal::handleSensors(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /sensors");
    if (!authorizeRequest(request, false)) return;
    if (guardAPOnly(request)) return;

    // Get list of sensors
    std::vector<ActiveSensorEntry> sensorsList = sensorManager.getActiveSensorEntries();

    // Generate HTML
    String html = HTMLGenerator::generateSensorsPage(sensorsList);

    // Send response
    request->send(200, "text/html", html);
}

// Sensor add page
void WebPortal::handleSensorAdd(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /sensors/add");
    if (!authorizeRequest(request, false)) return;
    if (guardAPOnly(request)) return;

    // Generate HTML
    String html = HTMLGenerator::generateSensorAddPage();

    // Send response
    request->send(200, "text/html", html);
}

// Process sensor add form
void WebPortal::handleSensorAddPost(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /sensors/add");
    if (!authorizeRequest(request, true)) return;
    if (guardAPOnly(request)) return;

    // Check parameters
    if (request->hasParam("name", true) &&
        request->hasParam("deviceType", true) &&
        request->hasParam("serialNumber", true) &&
        request->hasParam("deviceKey", true))
    {

        // Get form data
        String name = request->getParam("name", true)->value();
        uint8_t deviceTypeValue = 0;
        const String deviceTypeText =
            request->getParam("deviceType", true)->value();
        String serialNumberHex = request->getParam("serialNumber", true)->value();
        String deviceKeyHex = request->getParam("deviceKey", true)->value();
        String customUrl = request->hasParam("customUrl", true) ? request->getParam("customUrl", true)->value() : "";
        int altitude = 0;
        if (request->hasParam("altitude", true))
        {
            altitude = request->getParam("altitude", true)->value().toInt();
        }

        if (name.length() > MAX_SENSOR_NAME_LENGTH)
        {
            request->send(400, "text/plain", "Sensor name too long");
            return;
        }
        if (customUrl.length() > MAX_SENSOR_CUSTOM_URL_LENGTH)
        {
            request->send(400, "text/plain", "Custom URL too long");
            return;
        }
        if (!SecurityPolicy::isHexString(serialNumberHex.c_str(), 6))
        {
            request->send(400, "text/plain", "Serial number must be exactly 6 hexadecimal characters");
            return;
        }
        if (!parseSupportedSensorType(deviceTypeText, deviceTypeValue))
        {
            request->send(400, "text/plain", "Unsupported device type");
            return;
        }

        if (!SecurityPolicy::isHexString(deviceKeyHex.c_str(), 8))
        {
            request->send(400, "text/plain", "Device key must be exactly 8 hexadecimal characters");
            return;
        }

        // Convert values
        SensorType deviceType = static_cast<SensorType>(deviceTypeValue);
        uint32_t serialNumber = strtoul(serialNumberHex.c_str(), NULL, 16);
        uint32_t deviceKey = strtoul(deviceKeyHex.c_str(), NULL, 16);

        // Get correction values with defaults if not present
        float tempCorr = 0.0f;
        float humCorr = 0.0f;
        float pressCorr = 0.0f;
        float ppmCorr = 0.0f;
        float luxCorr = 0.0f;
        float windSpeedCorr = 1.0f;
        int windDirCorr = 0;
        float rainAmountCorr = 1.0f;
        float rainRateCorr = 1.0f;

        if (request->hasParam("tempCorr", true))
        {
            tempCorr = request->getParam("tempCorr", true)->value().toFloat();
        }
        if (request->hasParam("humCorr", true))
        {
            humCorr = request->getParam("humCorr", true)->value().toFloat();
        }
        if (request->hasParam("pressCorr", true))
        {
            pressCorr = request->getParam("pressCorr", true)->value().toFloat();
        }
        if (request->hasParam("ppmCorr", true))
        {
            ppmCorr = request->getParam("ppmCorr", true)->value().toFloat();
        }
        if (request->hasParam("luxCorr", true))
        {
            luxCorr = request->getParam("luxCorr", true)->value().toFloat();
        }
        if (request->hasParam("windSpeedCorr", true))
        {
            windSpeedCorr = request->getParam("windSpeedCorr", true)->value().toFloat();
        }
        if (request->hasParam("windDirCorr", true))
        {
            windDirCorr = request->getParam("windDirCorr", true)->value().toInt();
        }
        if (request->hasParam("rainAmountCorr", true))
        {
            rainAmountCorr = request->getParam("rainAmountCorr", true)->value().toFloat();
        }
        if (request->hasParam("rainRateCorr", true))
        {
            rainRateCorr = request->getParam("rainRateCorr", true)->value().toFloat();
        }

        // Add sensor
        int sensorIndex = sensorManager.addSensor(deviceType, serialNumber, deviceKey, name);

        if (sensorIndex >= 0)
        {
            // Apply the remaining fields through the manager so the update,
            // serialization and rollback are covered by the same mutex.
            const bool configured = sensorManager.updateSensorConfig(
                sensorIndex, name, deviceType, serialNumber, deviceKey,
                customUrl, altitude, tempCorr, humCorr, pressCorr, ppmCorr,
                luxCorr, windSpeedCorr, windDirCorr, rainAmountCorr,
                rainRateCorr);
            if (configured)
            {
                logger.info("Added new sensor: " + name + " (SN: " + serialNumberHex + ")");

                // If MQTT is enabled, publish discovery message
                if (mqttManager && mqttManager->isConnected())
                {
                    mqttManager->publishDiscoveryForSensor(sensorIndex);
                }

                // Redirect to sensors list
                request->redirect("/sensors");
                return;
            }

            logger.error("Failed to persist complete configuration for newly added sensor");
        }

        // Error adding sensor
        request->send(500, "text/plain", "Failed to add sensor");
    }
    else
    {
        // Missing parameters
        request->send(400, "text/plain", "Missing parameters");
    }
}

// Sensor edit page
void WebPortal::handleSensorEdit(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /sensors/edit");
    if (!authorizeRequest(request, false)) return;
    if (guardAPOnly(request)) return;

    // Check index parameter
    if (request->hasParam("index"))
    {
        int index = request->getParam("index")->value().toInt();

        SensorData sensor;
        if (sensorManager.getSensorSnapshot(index, sensor))
        {
            // Generate HTML
            String html = HTMLGenerator::generateSensorEditPage(sensor, index);

            // Send response
            request->send(200, "text/html", html);
            return;
        }
    }

    // Sensor not found, redirect to sensors list
    request->redirect("/sensors");
}

// Process sensor edit form
void WebPortal::handleSensorEditPost(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /sensors/update");
    if (!authorizeRequest(request, true)) return;
    if (guardAPOnly(request)) return;

    // Check parameters
    if (request->hasParam("index", true) &&
        request->hasParam("name", true) &&
        request->hasParam("deviceType", true) &&
        request->hasParam("serialNumber", true) &&
        request->hasParam("deviceKey", true))
    {
        // Get form data for corrections with defaults if not present
        float tempCorr = 0.0f;
        float humCorr = 0.0f;
        float pressCorr = 0.0f;
        float ppmCorr = 0.0f;
        float luxCorr = 0.0f;
        float windSpeedCorr = 1.0f;
        int windDirCorr = 0;
        float rainAmountCorr = 1.0f;
        float rainRateCorr = 1.0f;

        if (request->hasParam("tempCorr", true))
        {
            tempCorr = request->getParam("tempCorr", true)->value().toFloat();
        }
        if (request->hasParam("humCorr", true))
        {
            humCorr = request->getParam("humCorr", true)->value().toFloat();
        }
        if (request->hasParam("pressCorr", true))
        {
            pressCorr = request->getParam("pressCorr", true)->value().toFloat();
        }
        if (request->hasParam("ppmCorr", true))
        {
            ppmCorr = request->getParam("ppmCorr", true)->value().toFloat();
        }
        if (request->hasParam("luxCorr", true))
        {
            luxCorr = request->getParam("luxCorr", true)->value().toFloat();
        }
        if (request->hasParam("windSpeedCorr", true))
        {
            windSpeedCorr = request->getParam("windSpeedCorr", true)->value().toFloat();
        }
        if (request->hasParam("windDirCorr", true))
        {
            windDirCorr = request->getParam("windDirCorr", true)->value().toInt();
        }
        if (request->hasParam("rainAmountCorr", true))
        {
            rainAmountCorr = request->getParam("rainAmountCorr", true)->value().toFloat();
        }
        if (request->hasParam("rainRateCorr", true))
        {
            rainRateCorr = request->getParam("rainRateCorr", true)->value().toFloat();
        }

        // Get form data
        int index = request->getParam("index", true)->value().toInt();
        String name = request->getParam("name", true)->value();
        uint8_t deviceTypeValue = 0;
        const String deviceTypeText =
            request->getParam("deviceType", true)->value();
        String serialNumberHex = request->getParam("serialNumber", true)->value();
        String deviceKeyHex = request->getParam("deviceKey", true)->value();
        String customUrl = request->hasParam("customUrl", true) ? request->getParam("customUrl", true)->value() : "";
        int altitude = 0;
        if (request->hasParam("altitude", true))
        {
            altitude = request->getParam("altitude", true)->value().toInt();
        }

        if (name.length() > MAX_SENSOR_NAME_LENGTH)
        {
            request->send(400, "text/plain", "Sensor name too long");
            return;
        }
        if (customUrl.length() > MAX_SENSOR_CUSTOM_URL_LENGTH)
        {
            request->send(400, "text/plain", "Custom URL too long");
            return;
        }
        if (!SecurityPolicy::isHexString(serialNumberHex.c_str(), 6))
        {
            request->send(400, "text/plain", "Serial number must be exactly 6 hexadecimal characters");
            return;
        }
        if (!parseSupportedSensorType(deviceTypeText, deviceTypeValue))
        {
            request->send(400, "text/plain", "Unsupported device type");
            return;
        }

        SensorData existingSensor;
        if (!sensorManager.getSensorSnapshot(index, existingSensor))
        {
            request->send(404, "text/plain", "Sensor not found");
            return;
        }

        uint32_t deviceKey = existingSensor.deviceKey;
        if (deviceKeyHex.length() > 0)
        {
            if (!SecurityPolicy::isHexString(deviceKeyHex.c_str(), 8))
            {
                request->send(400, "text/plain", "Replacement device key must be exactly 8 hexadecimal characters");
                return;
            }
            deviceKey = strtoul(deviceKeyHex.c_str(), NULL, 16);
        }

        // Convert values
        SensorType deviceType = static_cast<SensorType>(deviceTypeValue);
        uint32_t serialNumber = strtoul(serialNumberHex.c_str(), NULL, 16);

        // Update sensor - Update this function parameter list too (see next step)
        bool success = sensorManager.updateSensorConfig(
            index, name, deviceType, serialNumber, deviceKey, customUrl, altitude,
            tempCorr, humCorr, pressCorr, ppmCorr, luxCorr,
            windSpeedCorr, windDirCorr, rainAmountCorr, rainRateCorr);

        if (success)
        {
            logger.info("Updated sensor: " + name + " (SN: " + serialNumberHex + ")");

            // If MQTT is enabled, publish discovery message
            if (mqttManager && mqttManager->isConnected())
            {
                logger.info("Updating MQTT discovery for edited sensor");
                // Remove the old capability set first. This also cleans up
                // topics when the serial number or device type changed.
                mqttManager->removeDiscoveryForSensor(
                    existingSensor.serialNumber);
                mqttManager->publishDiscoveryForSensor(index);
            }

            // Redirect to sensors list
            request->redirect("/sensors");
        }
        else
        {
            // Error updating sensor
            request->send(500, "text/plain", "Failed to update sensor");
        }
    }
    else
    {
        // Missing parameters
        request->send(400, "text/plain", "Missing parameters");
    }
}

// Delete sensor
void WebPortal::handleSensorDelete(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /sensors/delete");
    if (!authorizeRequest(request, true)) return;
    if (guardAPOnly(request)) return;

    // Check index parameter
    if (request->hasParam("index", true))
    {
        int index = request->getParam("index", true)->value().toInt();

        SensorData sensor;
        if (sensorManager.getSensorSnapshot(index, sensor))
        {
            String name = sensor.name;
            uint32_t serialNumber = sensor.serialNumber;

            // Delete sensor
            bool success = sensorManager.deleteSensor(index);

            if (success)
            {
                logger.info("Deleted sensor: " + name + " (SN: " + formatSN(serialNumber) + ")");

                // If MQTT is enabled, remove discovery message
                if (mqttManager && mqttManager->isConnected())
                {
                    logger.info("Removing MQTT discovery for deleted sensor");
                    mqttManager->removeDiscoveryForSensor(serialNumber);
                }
            }
            else
            {
                logger.warning("Failed to delete sensor with index " + String(index));
            }
        }
        else
        {
            logger.warning("Attempt to delete non-existent sensor with index " + String(index));
        }
    }

    // Redirect to sensors list
    request->redirect("/sensors");
}

// Logs page
void WebPortal::handleLogs(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /logs");
    if (!authorizeRequest(request, false)) return;
    if (guardAPOnly(request)) return;

    // Snapshot under Logger's mutex; render only caller-owned String objects.
    const std::vector<LogEntry> logs = logger.getLogsSnapshot();

    // Generate HTML
    String html = HTMLGenerator::generateLogsPage(logs, logger.getLogLevel());

    // Send response
    request->send(200, "text/html", html);
}

// Clear logs
void WebPortal::handleLogsClear(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /logs/clear");
    if (!authorizeRequest(request, true)) return;
    if (guardAPOnly(request)) return;

    // Clear logs
    logger.clearLogs();

    logger.info("Memory after log clear - Free heap: " + String(ESP.getFreeHeap()) +
                " bytes, Largest block: " + String(ESP.getMaxAllocHeap()) + " bytes");

    // Redirect back to logs page
    request->redirect("/logs");
}

// Set logging level
void WebPortal::handleLogLevel(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /logs/level");
    if (!authorizeRequest(request, true)) return;
    if (guardAPOnly(request)) return;

    // Check level parameter
    if (request->hasParam("level", true))
    {
        String levelStr = request->getParam("level", true)->value();
        LogLevel level = logger.levelFromString(levelStr);

        // Set new level
        configManager.setLogLevel(level);
    }

    // Redirect back to logs page
    request->redirect("/logs");
}

// MQTT Configuration Page
void WebPortal::handleMqtt(AsyncWebServerRequest *request)
{
    if (!authorizeRequest(request, false)) return;
    // logger.debug("HTTP request: GET /mqtt");
    if (guardAPOnly(request)) return;

    // Get MQTT configuration from ConfigManager
    // ConfigManager* configManager = (ConfigManager*)request->getParam("configManager")->value().toInt();
    logger.debug("HTTP request: GET /mqtt");

    // Generate HTML
    String html = HTMLGenerator::generateMqttPage(
        configManager.mqttHost,
        configManager.mqttPort,
        configManager.mqttUser,
        configManager.mqttPassword.length() > 0,
        configManager.mqttEnabled,
        configManager.mqttTls,
        configManager.mqttPrefix,
        configManager.mqttHAEnabled,
        configManager.mqttHAPrefix
    );

    // Send response
    request->send(200, "text/html", html);
}

// MQTT Configuration Post Handler
void WebPortal::handleMqttPost(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /mqtt");
    if (!authorizeRequest(request, true)) return;
    if (guardAPOnly(request)) return;

    // Check parameters
    if (request->hasParam("host", true) &&
        request->hasParam("port", true) &&
        request->hasParam("user", true) &&
        request->hasParam("password", true) &&
        request->hasParam("prefix", true) &&
        request->hasParam("haPrefix", true))
    {

        // Get form data
        String host = request->getParam("host", true)->value();
        int port = request->getParam("port", true)->value().toInt();
        String user = request->getParam("user", true)->value();
        const String submittedPassword = request->getParam("password", true)->value();
        const bool clearPassword = request->hasParam("clearPassword", true);
        String password(SecurityPolicy::resolveSecret(
                            configManager.mqttPassword.c_str(),
                            submittedPassword.c_str(),
                            clearPassword)
                            .c_str());
        bool enabled = request->hasParam("enabled", true);
        bool tls = request->hasParam("tls", true);
        String prefix = request->getParam("prefix", true)->value();
        bool haEnabled = request->hasParam("haEnabled", true);
        String haPrefix = request->getParam("haPrefix", true)->value();

        // Update configuration
        if (!configManager.setMqttConfig(
                host, port, user, password, enabled, tls, prefix, haPrefix, haEnabled))
        {
            request->send(400, "text/plain", "Invalid MQTT configuration");
            return;
        }

        logger.info("MQTT configuration updated");
        logger.info("  Enabled: " + String(enabled));
        logger.info("  TLS: " + String(tls));
        logger.info("  HA Enabled: " + String(haEnabled));

        // If MQTT is enabled, reinitialize the MQTT manager
        if (mqttManager)
        {
            logger.info("Reinitializing MQTT with new configuration...");
            mqttManager->disconnect();
            mqttManager->init();
        }

        // Redirect to MQTT page
        request->redirect("/mqtt");
    }
    else
    {
        request->send(400, "text/plain", "Missing parameters");
    }
}

// API for retrieving sensor data
void WebPortal::handleAPI(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /api");
    if (!authorizeRequest(request, false)) return;
    if (guardAPOnly(request)) return;

    // Check format parameter
    String format = request->hasParam("format") ? request->getParam("format")->value() : "html";

    // Check sensor parameter
    String sensorParam = request->hasParam("sensor") ? request->getParam("sensor")->value() : "";

    // Get list of sensors
    std::vector<SensorData> sensorsList;

    if (sensorParam.length() > 0)
    {
        // Filter by specific sensor
        uint32_t serialNumber = strtoul(sensorParam.c_str(), NULL, 16);
        int sensorIndex = sensorManager.findSensorBySN(serialNumber);

        if (sensorIndex >= 0)
        {
            SensorData sensor;
            if (sensorManager.getSensorSnapshot(sensorIndex, sensor))
            {
                sensorsList.push_back(sensor);
            }
        }
    }
    else
    {
        // All sensors
        sensorsList = sensorManager.getActiveSensors();
    }

    if (format.equalsIgnoreCase("json"))
    {
        // JSON format
        String jsonOutput = HTMLGenerator::generateAPIJson(sensorsList);

        // Send JSON response
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        response->print(jsonOutput);
        request->send(response);
    }
    else if (format.equalsIgnoreCase("csv"))
    {
        // CSV format
        AsyncResponseStream *response = request->beginResponseStream("text/csv");
        response->print("name,type,serialNumber,lastSeen,temperature,humidity,pressure,ppm,lux,batteryVoltage,rssi\r\n");

        for (const auto &sensor : sensorsList)
        {
            response->print(
                SecurityPolicy::csvEscape(sensor.name.c_str()).c_str());
            response->print(",");
            response->print(static_cast<uint8_t>(sensor.deviceType));
            response->print(",");
            response->print(formatSN(sensor.serialNumber));
            response->print(",");
            response->print(sensor.lastSeen > 0 ? String((millis() - sensor.lastSeen) / 1000) : "-1");
            response->print(",");

            response->print(sensor.hasTemperature() ? String(sensor.temperature, 2) : "");
            response->print(",");
            response->print(sensor.hasHumidity() ? String(sensor.humidity, 2) : "");
            response->print(",");
            response->print(sensor.hasPressure() ? String(sensor.pressure, 2) : "");
            response->print(",");
            response->print(sensor.hasPPM() ? String(sensor.ppm, 0) : "");
            response->print(",");
            response->print(sensor.hasLux() ? String(sensor.lux, 1) : "");
            response->print(",");
            response->print(String(sensor.batteryVoltage, 2));
            response->print(",");
            response->print(String(sensor.rssi));
            response->print("\r\n");
        }

        request->send(response);
    }
    else if (format.equalsIgnoreCase("html"))
    {
        // HTML format (API documentation)
        String html = HTMLGenerator::generateAPIPage(sensorsList);
        request->send(200, "text/html", html);
    }
    else
    {
        // Unknown format
        request->send(400, "text/plain", "Invalid format parameter. Supported formats: json, csv, html");
    }
}

// Restart device
void WebPortal::handleReboot(AsyncWebServerRequest *request)
{
    if (!authorizeRequest(request, true)) return;
    logger.info("HTTP request: POST /reboot - Rebooting device");

    request->send(200, "text/html",
                  "<html><head><meta http-equiv='refresh' content='10;url=/'></head>"
                  "<body><h1>Rebooting</h1>"
                  "<p>The device is rebooting. You will be redirected in 10 seconds...</p></body></html>");

    // Defer the restart so the async server can flush the response cleanly
    // (same pattern as handleConfigPost). The handleClient() main-loop hook
    // fires ESP.restart() once restartAt has been reached. The previous
    // inline `delay(500); ESP.restart()` raced with the async-server task
    // and intermittently lost the restart entirely when the LoRa loop was
    // busy at the same moment.
    restartAt = millis() + 1500;
    restartRequested = true;
}

// Handle unknown pages
// In WebServer.cpp, modify the handleNotFound method
// Update WebPortal::handleNotFound in WebServer.cpp
void WebPortal::handleNotFound(AsyncWebServerRequest *request)
{
    String requestUrl = request->url();
    logger.debug("HTTP 404: " + requestUrl);

    if (isAPMode)
    {
        // In AP mode - captive portal - redirect all requests to config page

        // Skip redirecting for certain files that might be requested by browsers
        if (requestUrl.endsWith(".ico") || requestUrl.endsWith(".jpg") ||
            requestUrl.endsWith(".png") || requestUrl.endsWith(".gif") ||
            requestUrl.endsWith(".css") || requestUrl.endsWith(".js"))
        {
            request->send(404, "text/plain", "Not found");
            return;
        }

        // For Apple devices
        if (requestUrl == "/hotspot-detect.html" || requestUrl.indexOf("captive.apple.com") >= 0)
        {
            request->redirect("http://" + WiFi.softAPIP().toString() + "/config");
            return;
        }

        // For Android and Windows devices
        if (requestUrl == "/generate_204" || requestUrl == "/ncsi.txt" ||
            requestUrl.indexOf("detectportal.firefox.com") >= 0)
        {
            request->redirect("http://" + WiFi.softAPIP().toString() + "/config");
            return;
        }

        // General case - redirect to config
        request->redirect("/config");
    }
    else
    {
        // In normal mode - 404
        request->send(404, "text/plain", "404: Not Found");
    }
}

// Diagnostics endpoint: heap and system info 
void WebPortal::handleDiagHeap(AsyncWebServerRequest *request)
{
    if (!authorizeRequest(request, false)) return;
    size_t freeHeap = ESP.getFreeHeap();
    size_t maxAlloc = ESP.getMaxAllocHeap();
#ifdef BOARD_HAS_PSRAM
    size_t psramSize = ESP.getPsramSize();
    size_t psramFree = ESP.getFreePsram();
#else
    size_t psramSize = 0;
    size_t psramFree = 0;
#endif
    size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    float frag = (free8 > 0) ? (1.0f - (float)largest8 / (float)free8) : 0.0f;
    const esp_partition_t *runningPartition = esp_ota_get_running_partition();
    esp_ota_img_states_t otaImageState = ESP_OTA_IMG_UNDEFINED;
    if (runningPartition != nullptr)
    {
        esp_ota_get_state_partition(runningPartition, &otaImageState);
    }

    AsyncResponseStream *res = request->beginResponseStream("application/json");
    res->print("{");
    res->print("\"freeHeap\":"); res->print((unsigned long)freeHeap); res->print(",");
    res->print("\"maxAllocHeap\":"); res->print((unsigned long)maxAlloc); res->print(",");
    res->print("\"free8bit\":"); res->print((unsigned long)free8); res->print(",");
    res->print("\"largest8bit\":"); res->print((unsigned long)largest8); res->print(",");
    res->print("\"fragmentation\":"); res->print(frag, 4); res->print(",");
    res->print("\"psramTotal\":"); res->print((unsigned long)psramSize); res->print(",");
    res->print("\"psramFree\":"); res->print((unsigned long)psramFree); res->print(",");
    res->print("\"uptimeMs\":"); res->print((unsigned long)millis()); res->print(",");
    res->print("\"wifiStatus\":"); res->print((int)WiFi.status()); res->print(",");
    res->print("\"apMode\":"); res->print(isAPMode ? "true" : "false"); res->print(",");
    res->print("\"otaImageState\":"); res->print((int)otaImageState); res->print(",");
    res->print("\"runningPartition\":\"");
    res->print(runningPartition != nullptr ? runningPartition->label : "unknown");
    res->print("\"");
    res->print("}");
    request->send(res);
}

void WebPortal::handleSecurity(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /security");
    if (!authorizeRequest(request, false)) return;
    request->send(200, "text/html", HTMLGenerator::generateSecurityPage(apName));
}

void WebPortal::handleSecurityRotate(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /security/rotate");
    if (!authorizeRequest(request, true)) return;
    if (!request->hasParam("kind", true))
    {
        request->send(400, "application/json", "{\"error\":\"Missing credential kind\"}");
        return;
    }

    const String kind = request->getParam("kind", true)->value();
    String credential;
    bool ok = false;
    if (kind == "admin")
    {
        ok = configManager.rotateAdminCredential(credential);
    }
    else if (kind == "ap")
    {
        ok = configManager.rotateApCredential(credential);
    }
    else
    {
        request->send(400, "application/json", "{\"error\":\"Unknown credential kind\"}");
        return;
    }

    if (!ok)
    {
        request->send(500, "application/json", "{\"error\":\"Credential rotation could not be persisted\"}");
        return;
    }

    String response = "{\"kind\":\"" + kind + "\",\"credential\":\"" +
                      credential + "\",\"oneTimeDisplay\":true";
    if (kind == "ap")
    {
        response += ",\"rebooting\":true";
        restartAt = millis() + 3000;
        restartRequested = true;
    }
    response += "}";
    request->send(200, "application/json", response);
}

bool WebPortal::tryBeginOta(void *owner)
{
    if (otaStateMutex == nullptr ||
        xSemaphoreTake(otaStateMutex, pdMS_TO_TICKS(250)) != pdTRUE)
    {
        return false;
    }
    const bool available = !otaInProgress;
    if (available)
    {
        otaInProgress = true;
        otaOwner = owner;
        otaStatus = "starting";
        otaStatusMessage = "Firmware update is starting";
        otaProgress = 0;
    }
    xSemaphoreGive(otaStateMutex);
    return available;
}

void WebPortal::setOtaStatus(const String &status, const String &message,
                             int progress)
{
    if (otaStateMutex == nullptr ||
        xSemaphoreTake(otaStateMutex, pdMS_TO_TICKS(250)) != pdTRUE)
    {
        return;
    }
    otaStatus = status;
    otaStatusMessage = message;
    otaProgress = constrain(progress, 0, 100);
    xSemaphoreGive(otaStateMutex);
}

void WebPortal::finishOta(void *owner, const String &status,
                          const String &message, int progress)
{
    if (otaStateMutex == nullptr ||
        xSemaphoreTake(otaStateMutex, pdMS_TO_TICKS(250)) != pdTRUE)
    {
        return;
    }
    if (otaOwner == owner)
    {
        otaInProgress = false;
        otaOwner = nullptr;
        otaStatus = status;
        otaStatusMessage = message;
        otaProgress = constrain(progress, 0, 100);
    }
    xSemaphoreGive(otaStateMutex);
}

void WebPortal::handleFirmwareStatus(AsyncWebServerRequest *request)
{
    if (!authorizeRequest(request, false)) return;

    bool inProgress = false;
    String status;
    String message;
    int progress = 0;
    if (otaStateMutex == nullptr ||
        xSemaphoreTake(otaStateMutex, pdMS_TO_TICKS(250)) != pdTRUE)
    {
        request->send(503, "application/json",
                      "{\"error\":\"Firmware status is temporarily unavailable\"}");
        return;
    }
    inProgress = otaInProgress;
    status = otaStatus;
    message = otaStatusMessage;
    progress = otaProgress;
    xSemaphoreGive(otaStateMutex);

    DynamicJsonDocument document(384);
    document["status"] = status;
    document["message"] = message;
    document["progress"] = progress;
    document["inProgress"] = inProgress;
    String response;
    serializeJson(document, response);
    request->send(200, "application/json", response);
}

// Handle firmware version request
void WebPortal::handleFirmwareVersion(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /firmware/version");
    if (!authorizeRequest(request, false)) return;

    String json = "{\"version\":\"" + String(FIRMWARE_VERSION) + "\"}";
    request->send(200, "application/json", json);
}
