/**
 * expLORA Gateway
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
#include "../Network/Uplink.h"
#include "../Hardware/PowerSource.h"
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <atomic>
#include "../Security/SecurityPolicy.h"

namespace { std::atomic<bool> logsPageInFlight{false}; }
#include <HTTPClient.h>
#include <Update.h>
#include <esp_app_format.h>
#include <NetworkClientSecure.h>
#include "../config.h"
#include "../Hardware/WatchdogPause.h"

// Request attribute set on a request the patched parser must drop. The
// library keys attributes by pointer, so both sides use this one symbol
// (declared extern in scripts/patch_asyncwebserver.py)
extern const char EXPLORA_REJECT_ATTR[] = "explora_reject";

// Requests whose body would be parsed into RAM are limited (the body is read
// before the login middleware runs); firmware uploads are streamed instead
static constexpr size_t MAX_REQUEST_BODY = 4096; // largest real form ~1 kB

class OversizeRequestHandler : public AsyncWebHandler
{
public:
    explicit OversizeRequestHandler(WebAuth &webAuth) : auth(webAuth) {}

    // Only a logged-in multipart firmware upload may be large: any other
    // body (also a form-encoded POST to /firmware) would be buffered in RAM.
    // Headers, including the session cookie, are parsed at this point.
    bool canHandle(AsyncWebServerRequest *request) const override
    {
        if (request->contentLength() <= MAX_REQUEST_BODY)
        {
            return false;
        }
        bool upload = request->method() == HTTP_POST && request->url() == "/firmware" &&
                      request->contentType().startsWith("multipart/form-data");
        if (upload && auth.hasPassword() && auth.isAuthorized(request))
        {
            return false;
        }
        // The library would still receive the whole body before answering:
        // the patched parser drops a marked request at its next data
        request->setAttribute(EXPLORA_REJECT_ATTR, true);
        return true;
    }
    void handleRequest(AsyncWebServerRequest *request) override
    {
        request->send(413, "text/plain", "Request too large");
    }

private:
    WebAuth &auth;
};


// Constructor
WebPortal::WebPortal(SensorManager &sensors, Logger &log, String &ssid, String &password,
                     bool &config_mode, ConfigManager &config, String &tz)
    : mqttManager(nullptr), server(HTTP_PORT), sensorManager(sensors), logger(log), isAPMode(false),
      wifiSSID(ssid), wifiPassword(password), configMode(config_mode),
      timezone(tz), configManager(config), auth(config, log)
{
    otaStateMutex = xSemaphoreCreateMutex();
}

// Add this static task function to WebPortal in WebServer.cpp
// void WebPortal::webServerTask(void *parameter) {
//     WebPortal *server = (WebPortal*)parameter;

//     for (;;) {
//         if (server->isAPMode) {
//             // Process DNS requests (captive portal)
//             server->dnsServer.processNextRequest();
//         }

//         // Give other tasks time to run
//         vTaskDelay(1);
//     }
// }

// Modify the init function to create the task
bool WebPortal::init()
{
    logger.info("Initializing web portal");

    // Captive portal while the AP runs without local network access (see Uplink)
    if (uplink->isCaptive())
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


    // Create task on core 0 for DNS and other processing
    // xTaskCreatePinnedToCore(
    //     webServerTask,        // Task function
    //     "WebServerTask",      // Task name
    //     4096,                 // Stack size (bytes)
    //     this,                 // Parameter to pass
    //     1,                    // Task priority (1 is low)
    //     &webServerTaskHandle, // Task handle
    //     0                     // Run on core 0
    // );

    // logger.info("Web server task created on core 0");

    return true;
}

// Setup routes for web server
// Modify WebPortal::setupRoutes() in WebServer.cpp
void WebPortal::setupRoutes()
{
    if (routesRegistered)
    {
        return;
    }

    logger.info("Registering web server routes (once)");

    // First handler: claims oversized requests so their body is not parsed
    server.addHandler(new OversizeRequestHandler(auth));
    server.addMiddleware([this](AsyncWebServerRequest *request, ArMiddlewareNext next)
                         { requireLogin(request, next); });
    server.on("/login", HTTP_GET, std::bind(&WebPortal::handleLogin, this, std::placeholders::_1));
    server.on("/login", HTTP_POST, std::bind(&WebPortal::handleLoginPost, this, std::placeholders::_1));
    server.on("/logout", HTTP_GET, std::bind(&WebPortal::handleLogout, this, std::placeholders::_1));
    server.on("/password", HTTP_GET, std::bind(&WebPortal::handlePassword, this, std::placeholders::_1));
    server.on("/password", HTTP_POST, std::bind(&WebPortal::handlePasswordPost, this, std::placeholders::_1));

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

    // Network (WiFi settings post to /config, shared with the AP setup page)
    server.on("/network", HTTP_GET, std::bind(&WebPortal::handleNetwork, this, std::placeholders::_1));
#if defined(HAS_ETHERNET) || defined(HAS_LTE)
    server.on("/network", HTTP_POST, std::bind(&WebPortal::handleNetworkPost, this, std::placeholders::_1));
#endif

    // MQTT
    server.on("/mqtt", HTTP_GET, std::bind(&WebPortal::handleMqtt, this, std::placeholders::_1));
    server.on("/mqtt", HTTP_POST, std::bind(&WebPortal::handleMqttPost, this, std::placeholders::_1));

    server.on("/security", HTTP_GET, std::bind(&WebPortal::handleSecurity, this, std::placeholders::_1));
    server.on("/security/rotate", HTTP_POST, std::bind(&WebPortal::handleSecurityRotate, this, std::placeholders::_1));

    // API
    server.on("/api", HTTP_GET, std::bind(&WebPortal::handleAPI, this, std::placeholders::_1));

    // Diagnostics
    server.on("/diag/heap", HTTP_GET, std::bind(&WebPortal::handleDiagHeap, this, std::placeholders::_1));
    server.on("/diag/network", HTTP_GET, std::bind(&WebPortal::handleDiagNetwork, this, std::placeholders::_1));

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

// Paths reachable without a login: the login page itself, the read-only
// sensor data API and the version probe
// (GET only: POST /firmware/version would otherwise reach the /firmware
// upload handler by prefix match)
static bool isPublicPath(AsyncWebServerRequest *request)
{
    const String &url = request->url();
    if (url == "/login")
    {
        return request->method() == HTTP_GET || request->method() == HTTP_POST;
    }
    return request->method() == HTTP_GET && (url == "/api" || url == "/firmware/version");
}

void WebPortal::requireLogin(AsyncWebServerRequest *request, ArMiddlewareNext next)
{
    const String &url = request->url();
    if (request->method() != HTTP_GET && request->hasHeader("Origin"))
    {
        const AsyncWebHeader *origin = request->getHeader("Origin");
        if (!origin || !SecurityPolicy::isAllowedBrowserOrigin(origin->value().c_str(), request->host().c_str()))
        {
            request->send(403, "text/plain", "Cross-origin state-changing request rejected");
            return;
        }
    }
    if (isPublicPath(request))
    {
        next();
        return;
    }
    if (!auth.isAuthorized(request))
    {
        if (request->method() != HTTP_GET)
        {
            request->send(401, "text/plain", "Login required");
            return;
        }
        // Absolute in AP mode: captive portal probes come with foreign host names
        String target = isAPMode ? "http://" + uplink->getApIP().toString() + "/login" : String("/login");
        if (!isAPMode && url != "/" && url != "/logout")
        {
            // url() is percent-decoded: re-encode, a raw CR/LF would split the header
            target += "?next=";
            for (size_t i = 0; i < url.length(); i++)
            {
                char c = url[i];
                if (isAlphaNumeric(c) || c == '/' || c == '-' || c == '_' || c == '.')
                {
                    target += c;
                }
                else
                {
                    char hex[4];
                    snprintf(hex, sizeof(hex), "%%%02X", (uint8_t)c);
                    target += hex;
                }
            }
        }
        request->redirect(target);
        return;
    }
    // Fresh device or update from a version without login: set a password first
    if (!auth.hasPassword() && url != "/password" && url != "/logout")
    {
        request->redirect("/password");
        return;
    }
    next();
}

// Local path to return to after the login
// Page to return to after the login: one of the pages only - never an action
// such as /reboot or /logs/clear (a crafted link would run it on login), and
// never another host
static String safeNextPath(const String &next)
{
    static const char *const pages[] = {"/", "/network", "/sensors", "/sensors/add", "/mqtt", "/firmware",
                                        "/logs", "/api", "/password", "/config", "/security"};
    for (const char *page : pages)
    {
        if (next == page)
        {
            return next;
        }
    }
    return "/";
}

void WebPortal::handleLogin(AsyncWebServerRequest *request)
{
    String next = request->hasParam("next") ? request->getParam("next")->value() : String();
    request->send(200, "text/html", HTMLGenerator::generateLoginPage(auth.hasPassword(), "", safeNextPath(next)));
}

void WebPortal::handleLoginPost(AsyncWebServerRequest *request)
{
    String password = request->hasParam("password", true) ? request->getParam("password", true)->value() : String();
    String next = request->hasParam("next", true) ? request->getParam("next", true)->value() : String();
    unsigned long retryAfter = 0;
    String token = auth.login(request, password, retryAfter);
    if (token.length() == 0)
    {
        String error = retryAfter > 0 ? "Too many failed attempts - try again in " + String(retryAfter) + " s"
                                      : String("Wrong password");
        request->send(401, "text/html", HTMLGenerator::generateLoginPage(auth.hasPassword(), error, safeNextPath(next)));
        return;
    }
    AsyncWebServerResponse *response = request->beginResponse(302);
    response->addHeader("Location", auth.hasPassword() ? safeNextPath(next) : String("/password"));
    response->addHeader("Set-Cookie", WebAuth::cookieFor(token));
    request->send(response);
}

void WebPortal::handleLogout(AsyncWebServerRequest *request)
{
    auth.logout(request);
    AsyncWebServerResponse *response = request->beginResponse(302);
    response->addHeader("Location", "/login");
    response->addHeader("Set-Cookie", WebAuth::cookieFor(""));
    request->send(response);
}

void WebPortal::handlePassword(AsyncWebServerRequest *request)
{
    request->send(200, "text/html", HTMLGenerator::generatePasswordPage(auth.hasPassword(), "", isAPMode));
}

void WebPortal::handlePasswordPost(AsyncWebServerRequest *request)
{
    std::lock_guard<std::recursive_mutex> credentialsGuard(configManager.mutex());
    auto param = [request](const char *name)
    {
        return request->hasParam(name, true) ? request->getParam(name, true)->value() : String();
    };
    String error = auth.changePassword(request, param("current"), param("password"), param("confirm"));
    if (error.length() > 0)
    {
        request->send(400, "text/html", HTMLGenerator::generatePasswordPage(auth.hasPassword(), error, isAPMode));
        return;
    }
    // All sessions ended with the change - continue with a new one
    String token = auth.sessionAfterPasswordChange();
    AsyncWebServerResponse *response = request->beginResponse(302);
    response->addHeader("Location", isAPMode ? "/config" : "/");
    response->addHeader("Set-Cookie", WebAuth::cookieFor(token));
    request->send(response);
}

// Called from the main loop. Drives deferred restarts so handlers can safely
// schedule ESP.restart() without blocking the async response flush.
void WebPortal::handleClient()
{
    void *otaContext = nullptr;
    bool checkUpdate = false;
    {
        std::lock_guard<std::mutex> guard(otaWorkMutex);
        otaContext = pendingOtaContext;
        pendingOtaContext = nullptr;
        checkUpdate = updateCheckRequested;
        updateCheckRequested = false;
    }
    if (otaContext != nullptr)
    {
        runFirmwareUpdate(otaContext);
    }
    if (checkUpdate)
    {
        runUpdateCheck();
    }
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
    // Stop the task if it's running
    // if (webServerTaskHandle != NULL) {
    //    vTaskDelete(webServerTaskHandle);
    //}

    // Stop DNS server
    dnsServer.stop();
}

// Process DNS requests
void WebPortal::processDNS()
{
    dnsServer.processNextRequest();
    // called automatically in handleClient()
}

// Toggle the captive-portal UI/DNS layer. The AP radio itself is managed by Uplink.
void WebPortal::setAPMode(bool enable)
{
    if (enable == isAPMode)
    {
        return; // No change
    }

    if (enable)
    {
        IPAddress apIP = uplink->getApIP();
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
        // Stop captive portal DNS; the AP radio is handled by Uplink
        dnsServer.stop();
        isAPMode = false;
    }
}

// Get current mode
bool WebPortal::isInAPMode() const
{
    return isAPMode;
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

    // First setup over the access point: WiFi page
    if (guardAPOnly(request))
    {
        return;
    }

    // Get list of sensors
    std::vector<SensorData> sensorsList = sensorManager.getActiveSensors();

    // Generate HTML
    String html = HTMLGenerator::generateHomePage(sensorsList);

    // Send response
    request->send(200, "text/html", html);
}

// Only the very first setup (no WiFi configured and no other uplink) is
// limited to the WiFi page; an LTE-only gateway, or one whose network is
// down, is managed over its access point like any other
bool WebPortal::guardAPOnly(AsyncWebServerRequest *request)
{
    if (isAPMode && wifiSSID.length() == 0 && !uplink->isOnline())
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

    // Outside AP mode the WiFi settings live on the network page
    if (!isAPMode)
    {
        request->redirect("/network");
        return;
    }

    String currentIP = isAPMode ? uplink->getApIP().toString() : uplink->getLocalIP().toString();

    // Generate HTML (use isAPMode to drive UI state)
    String html = HTMLGenerator::generateConfigPage(wifiSSID, wifiPassword.length() > 0, isAPMode, currentIP, timezone);

    // Send response
    request->send(200, "text/html", html);
}

// Process WiFi configuration form
void WebPortal::handleConfigPost(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /config");

    if (!request->hasParam("ssid", true) || !request->hasParam("password", true))
    {
        request->send(400, "text/plain", "Missing parameters");
        return;
    }

    String newSsid = request->getParam("ssid", true)->value();
    newSsid.trim();
    String newPassword(SecurityPolicy::resolveSecret(
        wifiPassword.c_str(), request->getParam("password", true)->value().c_str(),
        request->hasParam("clearPassword", true)).c_str());

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
    // Validate password: empty (open network), an 8..63 character passphrase
    // or a 64 hex digit pre-shared key (WPA2-PSK)
    bool hexKey = newPassword.length() == 64;
    for (size_t i = 0; hexKey && i < newPassword.length(); i++)
    {
        hexKey = isHexadecimalDigit(newPassword[i]);
    }
    if (newPassword.length() > 0 && newPassword.length() < 8)
    {
        request->send(400, "text/plain", "Password must be at least 8 characters or left empty for open networks");
        return;
    }
    if (newPassword.length() > 63 && !hexKey)
    {
        request->send(400, "text/plain", "Password too long (max 63 characters, or a 64 hex digit key)");
        return;
    }

    String newTimezone = request->hasParam("timezone", true) ? request->getParam("timezone", true)->value() : timezone;
    static const char *const zones[][2] = TIMEZONE_OPTIONS;
    bool knownZone = false;
    for (const auto &zone : zones)
    {
        knownZone = knownZone || newTimezone == zone[0];
    }
    if (!knownZone)
    {
        request->send(400, "text/plain", "Unknown timezone");
        return;
    }

    // Through ConfigManager: both stores (file and Preferences) and atomic,
    // so nothing (the timezone) is overridden by a stale copy at boot
    logger.info("New WiFi configuration received (SSID length: " + String(newSsid.length()) + ")");
    {
        std::lock_guard<std::recursive_mutex> guard(configManager.mutex());
        timezone = newTimezone;
    }
    if (!configManager.setWiFiConfig(newSsid, newPassword))
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
    if (guardAPOnly(request)) return;

    // Generate HTML
    String html = HTMLGenerator::generateSensorAddPage();

    // Send response
    request->send(200, "text/html", html);
}

// Sensor form fields shared by add and edit
struct SensorForm
{
    String name;
    SensorType deviceType;
    uint32_t serialNumber;
    uint32_t deviceKey;
    String customUrl;
    int altitude;
    float tempCorr, humCorr, pressCorr, ppmCorr, luxCorr;
    float windSpeedCorr;
    int windDirCorr;
    float rainAmountCorr, rainRateCorr;
};

// Hex number of 1..maxDigits digits, non-zero
static bool parseHexField(const String &text, uint8_t maxDigits, uint32_t &out)
{
    if (text.length() == 0 || text.length() > maxDigits)
    {
        return false;
    }
    for (size_t i = 0; i < text.length(); i++)
    {
        if (!isHexadecimalDigit(text[i]))
        {
            return false;
        }
    }
    out = strtoul(text.c_str(), nullptr, 16);
    return out != 0;
}

// Parse and validate the sensor form; returns an error message, empty if valid
// keepKey: an empty key field keeps form.deviceKey (edit form)
static String parseSensorForm(AsyncWebServerRequest *request, SensorForm &form, bool keepKey = false)
{
    auto text = [request](const char *name)
    {
        String value = request->hasParam(name, true) ? request->getParam(name, true)->value() : String();
        value.trim();
        return value;
    };
    auto number = [&text](const char *name, float fallback, float minValue, float maxValue, bool &ok)
    {
        String value = text(name);
        if (value.length() == 0)
        {
            return fallback;
        }
        float f = value.toFloat();
        if (!isfinite(f) || f < minValue || f > maxValue)
        {
            ok = false;
        }
        return f;
    };

    form.name = text("name");
    if (form.name.length() == 0 || form.name.length() > MAX_SENSOR_NAME_LENGTH)
    {
        return "Sensor name is empty or too long";
    }
    const String typeText = text("deviceType");
    for (size_t i = 0; i < typeText.length(); ++i)
    {
        if (!isDigit(static_cast<unsigned char>(typeText[i]))) return "Unsupported device type";
    }
    long type = typeText.toInt();
    if (type <= 0 || type > 255 ||
        !isSupportedSensorType(static_cast<uint8_t>(type)))
    {
        return "Unknown device type";
    }
    form.deviceType = static_cast<SensorType>(type);
    if ((!SecurityPolicy::isHexString(text("serialNumber").c_str(), 6) || !parseHexField(text("serialNumber"), 6, form.serialNumber)))
    {
        return "Serial number must be exactly 6 hexadecimal characters";
    }
    if (!(keepKey && text("deviceKey").length() == 0) && (!SecurityPolicy::isHexString(text("deviceKey").c_str(), 8) || !parseHexField(text("deviceKey"), 8, form.deviceKey)))
    {
        return "Device key must be exactly 8 hexadecimal characters";
    }
    form.customUrl = text("customUrl");
    if (form.customUrl.length() > MAX_SENSOR_CUSTOM_URL_LENGTH ||
        (form.customUrl.length() > 0 && !form.customUrl.startsWith("http://") && !form.customUrl.startsWith("https://")))
    {
        return "Custom URL must start with http:// or https:// and fit the configured length limit";
    }

    bool ok = true;
    form.altitude = (int)number("altitude", 0, 0, 9000, ok);
    form.tempCorr = number("tempCorr", 0, -50, 50, ok);
    form.humCorr = number("humCorr", 0, -50, 50, ok);
    form.pressCorr = number("pressCorr", 0, -200, 200, ok);
    form.ppmCorr = number("ppmCorr", 0, -5000, 5000, ok);
    form.luxCorr = number("luxCorr", 0, -100000, 100000, ok);
    form.windSpeedCorr = number("windSpeedCorr", 1, 0, 10, ok);
    form.windDirCorr = (int)number("windDirCorr", 0, -360, 360, ok);
    form.rainAmountCorr = number("rainAmountCorr", 1, 0, 10, ok);
    form.rainRateCorr = number("rainRateCorr", 1, 0, 10, ok);
    if (!ok)
    {
        return "A correction value or the altitude is out of range";
    }
    return "";
}

static bool saveSensorForm(SensorManager &sensors, int index, const SensorForm &form)
{
    return sensors.updateSensorConfig(index, form.name, form.deviceType, form.serialNumber, form.deviceKey,
                                      form.customUrl, form.altitude, form.tempCorr, form.humCorr,
                                      form.pressCorr, form.ppmCorr, form.luxCorr, form.windSpeedCorr,
                                      form.windDirCorr, form.rainAmountCorr, form.rainRateCorr);
}

// Process sensor add form
void WebPortal::handleSensorAddPost(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /sensors/add");
    if (guardAPOnly(request)) return;

    SensorForm form;
    String error = parseSensorForm(request, form);
    if (error.length() > 0)
    {
        request->send(400, "text/plain", error);
        return;
    }

    int index = sensorManager.addSensor(form.deviceType, form.serialNumber, form.deviceKey, form.name);
    if (index == -2)
    {
        request->send(409, "text/plain", "A sensor with this serial number already exists - edit it instead");
        return;
    }
    if (index >= 0 && !saveSensorForm(sensorManager, index, form))
    {
        // Not saved: take it back out, so a retry does not hit "already exists"
        sensorManager.discardUnsavedSensor(index);
        index = -1;
    }
    if (index < 0)
    {
        request->send(500, "text/plain", "Failed to add sensor (sensor list full, or the flash write failed - see Logs)");
        return;
    }

    if (mqttManager)
    {
        mqttManager->requestSensorDiscovery(form.serialNumber);
    }
    request->redirect("/sensors");
}

// Sensor edit page
void WebPortal::handleSensorEdit(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /sensors/edit");
    if (guardAPOnly(request)) return;

    // Check index parameter
    if (request->hasParam("index"))
    {
        int index = request->getParam("index")->value().toInt();

        SensorData sensor;
        if (sensorManager.getSensorCopy(index, sensor))
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
    if (guardAPOnly(request)) return;

    int index = request->hasParam("index", true) ? request->getParam("index", true)->value().toInt() : -1;
    SensorData before;
    if (!sensorManager.getSensorCopy(index, before))
    {
        request->send(404, "text/plain", "Sensor not found");
        return;
    }
    if (!request->hasParam("sn", true) || request->getParam("sn", true)->value() != formatSN(before.serialNumber))
    {
        request->send(409, "text/plain", "The sensor list changed in the meantime - reload the page");
        return;
    }

    SensorForm form;
    form.deviceKey = before.deviceKey;
    String error = parseSensorForm(request, form, true);
    if (error.length() > 0)
    {
        request->send(400, "text/plain", error);
        return;
    }
    if (!saveSensorForm(sensorManager, index, form))
    {
        request->send(409, "text/plain",
                      "Failed to update sensor (serial number used by another sensor, or the flash write failed - see Logs)");
        return;
    }

    if (mqttManager)
    {
        // Entities of the old serial number / old value set would stay in HA
        if (before.serialNumber != form.serialNumber || before.deviceType != form.deviceType)
        {
            mqttManager->requestSensorRemoval(before.serialNumber);
        }
        mqttManager->requestSensorDiscovery(form.serialNumber);
    }
    request->redirect("/sensors");
}

// Delete sensor
void WebPortal::handleSensorDelete(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /sensors/delete");
    if (guardAPOnly(request)) return;

    // Check index parameter
    if (request->hasParam("index", true))
    {
        int index = request->getParam("index", true)->value().toInt();

        SensorData sensor;
        String sn = request->hasParam("sn", true) ? request->getParam("sn", true)->value() : String();
        if (sensorManager.getSensorCopy(index, sensor) && sn == formatSN(sensor.serialNumber))
        {
            String name = sensor.name;
            uint32_t serialNumber = sensor.serialNumber;

            if (!sensorManager.deleteSensor(index))
            {
                logger.warning("Sensor deletion could not be persisted: " + name);
                request->send(500, "text/plain", "Failed to save sensor deletion");
                return;
            }
            if (mqttManager)
            {
                mqttManager->requestSensorRemoval(serialNumber);
            }
        }
        else
        {
            logger.warning("Delete ignored: no sensor " + sn + " at index " + String(index));
        }
    }

    // Redirect to sensors list
    request->redirect("/sensors");
}

// Logs page
void WebPortal::handleLogs(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /logs");
    if (guardAPOnly(request)) return;

    bool expected = false;
    if (!logsPageInFlight.compare_exchange_strong(expected, true))
    {
        auto *busy = request->beginResponse(503, "text/plain", "Logs page busy; retry");
        busy->addHeader("Retry-After", "1");
        request->send(busy);
        return;
    }
    request->onDisconnect([]() { logsPageInFlight.store(false); });

    // Streamed piece by piece: the whole page (up to ~50 kB with 150 entries)
    // never sits in RAM at once, nor twice (send() would copy it)
    struct Stream
    {
        String pending;
        size_t offset = 0;
        size_t entry = 0;
        uint8_t phase = 0; // 0 head, 1 entries, 2 tail, 3 done
        LogLevel level;
    };
    auto state = std::make_shared<Stream>();
    state->level = logger.getLogLevel();
    request->sendChunked("text/html", [state](uint8_t *buffer, size_t maxLen, size_t) -> size_t
                         {
        while (state->offset >= state->pending.length())
        {
            state->offset = 0;
            if (state->phase == 0)
            {
                state->pending = HTMLGenerator::generateLogsPageHead(state->level);
                state->phase = 1;
            }
            else if (state->phase == 1)
            {
                LogEntry entry;
                if (Logger::copyNewest(state->entry, entry))
                {
                    state->pending = HTMLGenerator::generateLogEntry(entry);
                    state->entry++;
                }
                else
                {
                    state->pending = state->entry == 0 ? "<div class='log-entry'>No logs to display</div>" : "";
                    state->phase = 2;
                }
            }
            else if (state->phase == 2)
            {
                state->pending = HTMLGenerator::generateLogsPageTail();
                state->phase = 3;
            }
            else
            {
                return 0;
            }
        }
        size_t n = std::min(maxLen, state->pending.length() - state->offset);
        memcpy(buffer, state->pending.c_str() + state->offset, n);
        state->offset += n;
        return n; });
}

// Clear logs
void WebPortal::handleLogsClear(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /logs/clear");
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
    if (guardAPOnly(request)) return;

    auto param = [request](const char *name)
    {
        String value = request->hasParam(name, true) ? request->getParam(name, true)->value() : String();
        value.trim();
        return value;
    };
    MQTTManager::Settings settings;
    settings.host = param("host");
    settings.port = param("port").toInt();
    settings.user = param("user");
    settings.password = request->hasParam("password", true) ? request->getParam("password", true)->value() : String();
    // Empty password = keep the stored one (it is not sent to the page);
    // no user means no password
    if (settings.password.length() == 0 && settings.user.length() > 0 && !request->hasParam("clearPassword", true))
    {
        settings.password = configManager.mqttPassword;
    }
    settings.enabled = request->hasParam("enabled", true);
    settings.tls = request->hasParam("tls", true);
    settings.prefix = param("prefix");
    settings.haEnabled = request->hasParam("haEnabled", true);
    settings.haPrefix = param("haPrefix");

    if (settings.host.length() == 0 || settings.host.length() > 64 || settings.host.indexOf(' ') >= 0)
    {
        request->send(400, "text/plain", "Broker host must have 1 to 64 characters without spaces");
        return;
    }
    if (settings.port < 1 || settings.port > 65535)
    {
        request->send(400, "text/plain", "Port must be 1 to 65535");
        return;
    }
    if (settings.user.length() > 64 || settings.password.length() > 64)
    {
        request->send(400, "text/plain", "User name and password max 64 characters");
        return;
    }
    bool prefixOk = ConfigManager::isValidMqttTopic(settings.prefix) || settings.prefix == configManager.mqttPrefix;
    bool haPrefixOk = ConfigManager::isValidMqttTopic(settings.haPrefix) || settings.haPrefix == configManager.mqttHAPrefix;
    if (!prefixOk || !haPrefixOk)
    {
        request->send(400, "text/plain", "Invalid topic (1-32 characters, no #, + or /)");
        return;
    }

    if (!configManager.setMqttConfig(settings.host, settings.port, settings.user, settings.password,
                                     settings.enabled, settings.tls, settings.prefix, settings.haPrefix,
                                     settings.haEnabled))
    {
        request->send(500, "text/plain", "Saving the MQTT configuration failed");
        return;
    }
    logger.info("MQTT configuration updated - " + settings.host + ":" + String(settings.port) +
                (settings.enabled ? ", enabled" : ", disabled"));
    // The loop task reconnects with a fresh copy of the settings
    if (mqttManager)
    {
        mqttManager->requestReinit();
    }
    request->redirect("/mqtt");
}

// API for retrieving sensor data
void WebPortal::handleAPI(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /api");
    if (guardAPOnly(request)) return;
    // Public endpoint: keep its peak (~20 kB with 20 sensors: list copy, JSON
    // document, response buffer) from eating the heap others need; 40 kB
    // still lets it run next to an MQTT TLS session on the Pro
    if (ESP.getFreeHeap() < 40000 || ESP.getMaxAllocHeap() < 12000)
    {
        request->send(503, "text/plain", "Low memory, try again");
        return;
    }

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
        for (const SensorData &sensor : sensorManager.getActiveSensors())
        {
            if (sensor.serialNumber == serialNumber)
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
        // One reserve of the measured size: the stream grows byte by byte
        // otherwise (a realloc per character of a public response)
        DynamicJsonDocument doc = HTMLGenerator::buildAPIJson(sensorsList);
        AsyncResponseStream *response = request->beginResponseStream("application/json", measureJson(doc) + 16);
        serializeJson(doc, *response);
        response->addHeader("Access-Control-Allow-Origin", "*");
        request->send(response);
    }
    else if (format.equalsIgnoreCase("csv"))
    {
        // CSV format
        AsyncResponseStream *response = request->beginResponseStream("text/csv");
        response->print("name,type,serialNumber,lastSeen,temperature,humidity,pressure,ppm,lux,batteryVoltage,rssi\r\n");

        for (const auto &sensor : sensorsList)
        {
            response->print(SecurityPolicy::csvEscape(sensor.name.c_str()).c_str());
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

        response->addHeader("Access-Control-Allow-Origin", "*");
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
            request->redirect("http://" + uplink->getApIP().toString() + "/config");
            return;
        }

        // For Android and Windows devices
        if (requestUrl == "/generate_204" || requestUrl == "/ncsi.txt" ||
            requestUrl.indexOf("detectportal.firefox.com") >= 0)
        {
            request->redirect("http://" + uplink->getApIP().toString() + "/config");
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

// Network state as JSON
void WebPortal::handleDiagNetwork(AsyncWebServerRequest *request)
{
    DynamicJsonDocument doc(1536);
    doc["uplink"] = uplink->getActiveName();
    doc["ip"] = uplink->getLocalIP().toString();
    doc["ap"] = uplink->isApActive();
    doc["captive"] = uplink->isCaptive();
    JsonObject timeObj = doc.createNestedObject("time");
    timeObj["valid"] = uplink->isTimeValid();
    timeObj["lastSync"] = (long)uplink->getLastTimeSync();
    timeObj["stale"] = uplink->isTimeStale();
    timeObj["status"] = uplink->getTimeStatus();
#if defined(HAS_PWR_SRC)
    doc["power"] = powerSource->getName();
#endif
    JsonObject wifi = doc.createNestedObject("wifi");
    wifi["connected"] = WiFi.STA.hasIP();
    wifi["ip"] = WiFi.STA.localIP().toString();
#if defined(HAS_ETHERNET)
    JsonObject eth = doc.createNestedObject("ethernet");
    eth["link"] = uplink->ethernetLinkUp();
    eth["ip"] = uplink->getEthernetIP().toString();
    eth["mode"] = configManager.ethStatic ? "static" : "dhcp";
#endif
#if defined(HAS_LTE)
    const LteModem &lte = uplink->getLte();
    JsonObject l = doc.createNestedObject("lte");
    l["state"] = lte.getStateName();
    l["ip"] = lte.localIP().toString();
    l["csq"] = lte.getSignalQuality();
    l["rssi"] = lte.getRssiDbm();
    l["operator"] = lte.getOperator();
    l["apn"] = lte.getApn();
    l["module"] = lte.getModuleName();
    l["error"] = lte.getLastError();
#endif
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// Also served in AP mode, so ethernet / LTE settings stay reachable from the
// access point (e.g. LTE-only installs or a wrong static address)
void WebPortal::handleNetwork(AsyncWebServerRequest *request)
{
    request->send(200, "text/html", HTMLGenerator::generateNetworkPage(configManager, wifiSSID, wifiPassword.length() > 0, timezone, isAPMode));
}

#if defined(HAS_ETHERNET) || defined(HAS_LTE)
// One form per section (hidden field "section"), both apply after a restart
void WebPortal::handleNetworkPost(AsyncWebServerRequest *request)
{
    auto param = [request](const char *name)
    {
        String value = request->hasParam(name, true) ? request->getParam(name, true)->value() : String();
        value.trim();
        return value;
    };
    String section = param("section");

    if (false)
    {
    }
#if defined(HAS_ETHERNET)
    else if (section == "ethernet")
    {
        bool useStatic = param("ethMode") == "static";
        String error = configManager.setEthernetConfig(useStatic, param("ip"), param("mask"), param("gateway"),
                                                       param("dns1"), param("dns2"));
        if (error.length() > 0)
        {
            request->send(400, "text/plain", error);
            return;
        }
        logger.info("Ethernet configuration updated - " +
                    String(useStatic ? "static " + configManager.ethIp : String("DHCP")));
    }
#endif
#if defined(HAS_LTE)
    else if (section == "lte")
    {
        bool enabled = request->hasParam("lteEnabled", true);
        String apn = param("apn");
        String pin = param("pin");
        bool pinTyped = pin.length() > 0;
        // Empty PIN = keep the stored one, unless removal was asked for
        if (pin.length() == 0 && !request->hasParam("clearPin", true))
        {
            pin = configManager.ltePin;
        }
        if (!configManager.setLteConfig(enabled, apn, pin))
        {
            request->send(400, "text/plain", "Invalid LTE settings (PIN 4-8 digits, APN max 63 characters: letters, digits, . - _)");
            return;
        }
        if (pinTyped)
        {
            configManager.setLtePinRejected(false); // re-entered: try it once more
        }
        logger.info("LTE configuration updated - enabled: " + String(enabled ? "yes" : "no") +
                    ", APN: " + (apn.length() > 0 ? apn : String("automatic")));
    }
#endif
    else
    {
        request->send(400, "text/plain", "Unknown settings section");
        return;
    }

    String html = "<!DOCTYPE html><html><head>";
    html += "<meta http-equiv='refresh' content='15;url=/network'>";
    html += "<title>Settings Saved</title></head>";
    html += "<body><h1>Settings Saved</h1>";
    html += "<p>The device will restart in a few seconds.";
#if defined(HAS_ETHERNET)
    if (section == "ethernet" && configManager.ethStatic)
    {
        String ip = HTMLGenerator::htmlEscape(configManager.ethIp);
        html += " Ethernet address: <a href='http://" + ip + "/network'>" + ip + "</a>";
    }
#endif
    html += "</p></body></html>";
    request->send(200, "text/html", html);
    // Deferred, see handleClient()
    restartAt = millis() + 1500;
    restartRequested = true;
}
#endif

// Handle firmware version request
void WebPortal::handleFirmwareVersion(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /firmware/version");
    if (guardAPOnly(request)) return;

    String json = "{\"version\":\"" + String(FIRMWARE_VERSION) + "\"}";
    request->send(200, "application/json", json);
}


bool WebPortal::isRequestAuthorized(AsyncWebServerRequest *request, bool stateChanging)
{
    if (!auth.hasPassword() || !auth.isAuthorized(request)) return false;
    if (!stateChanging || !request->hasHeader("Origin")) return true;
    const AsyncWebHeader *origin = request->getHeader("Origin");
    return origin && SecurityPolicy::isAllowedBrowserOrigin(origin->value().c_str(), request->host().c_str());
}

bool WebPortal::authorizeRequest(AsyncWebServerRequest *request, bool stateChanging)
{
    if (!auth.hasPassword() || !auth.isAuthorized(request))
    {
        request->send(401, "text/plain", "Login required");
        return false;
    }
    if (stateChanging && !isRequestAuthorized(request, true))
    {
        request->send(403, "text/plain", "Cross-origin state-changing request rejected");
        return false;
    }
    return true;
}

void WebPortal::handleSecurity(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /security");
    if (!authorizeRequest(request, false)) return;
    request->send(200, "text/html", HTMLGenerator::generateSecurityPage(uplink->getApName()));
}

void WebPortal::handleSecurityRotate(AsyncWebServerRequest *request)
{
    std::lock_guard<std::recursive_mutex> credentialsGuard(configManager.mutex());
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
        if (ok) auth.invalidateSessions();
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

bool WebPortal::startAccessPoint(bool allowStation)
{
    (void)allowStation;
    return uplink->restartAccessPoint();
}

void WebPortal::shutdownAccessPoint()
{
    setAPMode(false);
    uplink->shutdownAccessPoint();
}

bool WebPortal::isAccessPointActive() const
{
    return uplink->isApActive();
}
