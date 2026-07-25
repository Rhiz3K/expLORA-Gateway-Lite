/**
 * expLORA Gateway Lite
 *
 * Main program file for the expLORA Gateway
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

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <time.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <ArduinoJson.h>

// Configuration headers
#include "config.h"

// Data
#include "Data/SensorTypes.h"
#include "Data/SensorData.h"
#include "Data/Logging.h"
#include "Data/SensorManager.h"

// Hardware
#include "Hardware/LoRa_Module.h"
#include "Hardware/PSRAM_Manager.h"
#include "Hardware/SPI_Manager.h"

// Storage
#include "Storage/ConfigManager.h"

// Protocol
#include "Protocol/LoRaProtocol.h"
// MQTT
#include "Protocol/MQTTManager.h"

// Web interface
#include "Web/WebServer.h"
#include "Web/HTMLGenerator.h"
#include "Security/SecurityPolicy.h"

// Global object instances
Logger logger;                // Logging system
ConfigManager *configManager; // Configuration manager
SPIManager *spiManager;       // SPI communication manager
LoRaModule *loraModule;       // LoRa module
SensorManager *sensorManager; // Sensor manager
LoRaProtocol *loraProtocol;   // LoRa protocol
MQTTManager *mqttManager;     // MQTT Manager
WebPortal *webPortal;         // Web interface

// Timer for disabling AP mode
unsigned long apStartTime = 0;
bool temporaryAPMode = false;
bool systemReady = false;
wl_status_t lifecycleLastWifiStatus = WL_NO_SHIELD;
bool lifecycleWifiStatusInitialized = false;

// NTP synchronization helper - returns true if time was successfully synchronized
bool syncNTPTime(const String &timezone, unsigned long timeoutMs = 10000)
{
    configTime(0, 0, NTP_SERVER);
    setenv("TZ", timezone.c_str(), 1);
    tzset();

    // Wait for actual NTP sync with timeout
    struct tm timeinfo;
    unsigned long startWait = millis();
    while (!getLocalTime(&timeinfo, 100))
    {
        if (millis() - startWait > timeoutMs)
        {
            return false;
        }
        delay(100);
    }
    return true;
}

// File system initialization
bool initFileSystem()
{
    if (!LittleFS.begin(true))
    {
        Serial.println("ERROR: Failed to mount LittleFS");
        return false;
    }
    Serial.println("LittleFS mounted successfully");
    return true;
}

// Setup - device initialization
void setup()
{
    // Serial port initialization
    Serial.begin(115200);
    delay(1000); // Wait for stabilization

    Serial.println("\n\nexpLORA Gateway Lite");
    Serial.println("------------------------------------------------------------");

// Explicit PSRAM initialization
#ifdef BOARD_HAS_PSRAM
    if (psramInit())
    {
        Serial.println("PSRAM initialized manually!");
    }
#endif

    // Memory diagnostics
    Serial.printf("Total heap: %d bytes\n", ESP.getHeapSize());
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());

// PSRAM initialization
#ifdef BOARD_HAS_PSRAM
    bool psramInitialized = false;

    // Try different PSRAM detection methods
    if (esp_spiram_is_initialized())
    {
        psramInitialized = true;
        Serial.println("PSRAM initialized via esp_spiram_is_initialized()");
    }
    else if (ESP.getPsramSize() > 0)
    {
        psramInitialized = true;
        Serial.println("PSRAM detected via ESP.getPsramSize()");
    }

    if (psramInitialized)
    {
        Serial.printf("Total PSRAM: %d bytes\n", ESP.getPsramSize());
        Serial.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());
    }
    else
    {
        Serial.println("PSRAM initialization failed or not available");
    }
#endif

    // File system initialization
    if (!initFileSystem())
    {
        Serial.println("ERROR: File system initialization failed");
        return;
    }

    // Logging system initialization
    if (!logger.init(LOG_BUFFER_SIZE))
    {
        Serial.println("ERROR: Logger initialization failed");
        return;
    }

    // Basic log after initialization
    logger.info("expLORA Gateway Lite starting up - Firmware v" + String(FIRMWARE_VERSION));

    // HTML generator initialization
    if (!HTMLGenerator::init(true, WEB_BUFFER_SIZE))
    {
        logger.error("Failed to initialize HTML generator");
        return;
    }

    // Enable the ESP32 radio entropy source before a first-boot credential may
    // be generated. No STA association is started until WiFi.begin() below.
    if (!WiFi.mode(WIFI_STA))
    {
        logger.error("Failed to initialize WiFi entropy source");
        return;
    }

    // Configuration manager initialization
    configManager = new ConfigManager(logger);
    if (!configManager->init())
    {
        WiFi.mode(WIFI_OFF);
        Serial.println("ERROR: Secure NVS credential initialization failed. Repair or erase NVS over USB, then reboot.");
        logger.error("Failed to initialize configuration manager");
        return;
    }

    // Setting logging level according to configuration
    logger.setLogLevel(configManager->logLevel);

    // Physical ownership/bootstrap channel. Secrets are printed only when
    // first generated. Existing credentials can be rotated from the
    // authenticated Security page without erasing unrelated NVS settings.
    const String uniqueSSID = makeApName();
    Serial.println("\nSecure device access");
    Serial.println("AP SSID: " + uniqueSSID);
    Serial.println("Admin username: " + String(HTTP_ADMIN_USERNAME));
    if (configManager->wasApCredentialGeneratedThisBoot())
    {
        Serial.println("New AP credential: " + configManager->getApCredential());
    }
    if (configManager->wasAdminCredentialGeneratedThisBoot())
    {
        Serial.println("New admin credential: " +
                       configManager->getAdminCredential());
    }
    if (!configManager->wasApCredentialGeneratedThisBoot() &&
        !configManager->wasAdminCredentialGeneratedThisBoot())
    {
        Serial.println("Credentials already provisioned; they are not repeated on every boot.");
    }
    Serial.println("Keep both credentials private. Rotate them from the authenticated Security page.\n");

    // SPI manager initialization
    spiManager = new SPIManager(logger);
    if (!spiManager->init())
    {
        logger.error("Failed to initialize SPI manager");
        return;
    }

    // Sensor manager initialization
    sensorManager = new SensorManager(logger);
    if (!sensorManager->init())
    {
        logger.error("Failed to initialize sensor manager");
    }
    else
    {
        logger.info("Sensor manager initialized with " +
                    String(sensorManager->getSensorCount()) + " sensors");
    }

    webPortal = new WebPortal(*sensorManager, logger,
                              configManager->wifiSSID, configManager->wifiPassword,
                              configManager->configMode, *configManager, configManager->timezone);

    // WiFi initialization
    logger.info("Configuring WiFi. ConfigMode: " + String(configManager->configMode ? "true" : "false") +
                ", SSID length: " + String(configManager->wifiSSID.length()));

    // Setting timer for temporary AP mode
    apStartTime = millis();
    temporaryAPMode = true;
    wl_status_t setupWifiOutcome = WL_DISCONNECTED;

    if (configManager->configMode || configManager->wifiSSID.length() == 0)
    {
        // We're in configuration mode or don't have credentials - AP mode only
        logger.info("Starting in AP mode only");
        if (!webPortal->startAccessPoint(false))
        {
            WiFi.mode(WIFI_OFF);
            Serial.println("ERROR: Protected AP startup failed; refusing open fallback");
            logger.error("Protected AP startup failed; setup cannot continue");
            return;
        }
        logger.info("AP started with SSID: " + uniqueSSID + ", IP: " + WiFi.softAPIP().toString());

        configManager->enableConfigMode(true);
    }
    else
    {
        // We have credentials - start AP+STA mode
        logger.info("Starting in AP+STA mode (dual mode)");
        if (!webPortal->startAccessPoint(true))
        {
            WiFi.mode(WIFI_OFF);
            Serial.println("ERROR: Protected temporary AP startup failed; refusing open fallback");
            logger.error("Protected temporary AP startup failed; setup cannot continue");
            return;
        }
        logger.info("Temporary AP started with SSID: " + uniqueSSID +
                    " (will be active for 5 minutes). IP: " + WiFi.softAPIP().toString());

        // Configure STA part (client)
        logger.info("Attempting to connect to configured WiFi network");
        WiFi.begin(configManager->wifiSSID.c_str(), configManager->wifiPassword.c_str());

        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20)
        {
            delay(500);
            Serial.print(".");
            attempts++;
        }

        if (WiFi.status() == WL_CONNECTED)
        {
            setupWifiOutcome = WL_CONNECTED;
            logger.info("WiFi connected! IP: " + WiFi.localIP().toString());
            configManager->enableConfigMode(false);

            // Initialize NTP with sync verification
            if (syncNTPTime(configManager->timezone))
            {
                logger.info("NTP time synchronized successfully");
                logger.setTimeInitialized(true);
            }
            else
            {
                logger.warning("NTP sync failed - time not available");
            }
        }
        else
        {
            logger.warning("Failed to connect to configured WiFi after " + String(attempts) +
                           " attempts; keeping protected AP+STA active for automatic reconnect");
            // Keep STA enabled. ESP32 auto-reconnect can still succeed later,
            // at which point the normal connected transition shuts down AP.
        }
    }

    // Snapshot the result of the initial connection attempt. If STA connects
    // later while the remaining setup work runs, the first loop iteration
    // observes the transition and explicitly shuts down the recovery AP.
    lifecycleLastWifiStatus = setupWifiOutcome;
    lifecycleWifiStatusInitialized = true;

    // LoRa module initialization
    loraModule = new LoRaModule(logger, spiManager);
    if (!loraModule->init())
    {
        logger.error("Failed to initialize LoRa module");
    }
    else
    {
        logger.info("LoRa module initialized successfully");
    }

    // LoRa protocol initialization
    loraProtocol = new LoRaProtocol(*loraModule, *sensorManager, logger);

    // Initialize MQTT Manager if WiFi is connected
    mqttManager = new MQTTManager(*sensorManager, *configManager, logger);
    if (!mqttManager->init())
    {
        logger.debug("MQTT Manager initialization skipped (disabled in config)");
    }

    if (!webPortal->init())
    {
        logger.error("Failed to initialize web portal");
    }
    else
    {
        logger.info("Web portal initialized successfully");
    }

    webPortal->setMqttManager(mqttManager);

    // Initialize task watchdog
    esp_task_wdt_init(WDT_TIMEOUT, true); // Enable panic on timeout
    esp_task_wdt_add(NULL);               // Add current task to watchdog
    logger.info("Task watchdog initialized with timeout of " + String(WDT_TIMEOUT) + " seconds");

    // A freshly installed image remains rollback-eligible until all critical
    // storage, radio, protocol, and management components initialize. Only
    // now confirm it to the bootloader; a crash/reset before here restores the
    // previous known-good slot.
    const esp_partition_t *runningPartition = esp_ota_get_running_partition();
    esp_ota_img_states_t imageState;
    if (runningPartition != nullptr &&
        esp_ota_get_state_partition(runningPartition, &imageState) == ESP_OK &&
        imageState == ESP_OTA_IMG_PENDING_VERIFY)
    {
        if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK)
        {
            logger.error("Could not confirm the pending OTA image; rebooting for rollback");
            delay(250);
            ESP.restart();
            return;
        }
        logger.info("Pending OTA image passed initialization and was confirmed");
    }

    logger.info("System initialization complete");
    systemReady = true;
}

// Loop - main loop
void loop()
{
    // setup() can fail closed before global components exist (notably when
    // NVS cannot persist the per-device credential). Arduino still calls
    // loop() after setup() returns, so keep the radio/server inert and avoid
    // dereferencing partially initialized globals.
    if (!systemReady)
    {
        delay(1000);
        return;
    }

    esp_task_wdt_reset();

    // Detect WiFi status transitions and adapt web server routes
    wl_status_t currentWifiStatus = WiFi.status();
    if (!lifecycleWifiStatusInitialized)
    {
        lifecycleLastWifiStatus = currentWifiStatus;
        lifecycleWifiStatusInitialized = true;
    }
    if (lifecycleLastWifiStatus != currentWifiStatus)
    {
        // On transition to connected, update portal mode (routes are always registered)
        if (currentWifiStatus == WL_CONNECTED)
        {
            logger.info("WiFi connected (transition). Closing recovery AP.");
            // A connection observed after the initial setup snapshot is a
            // reconnect, not the intentional five-minute bootstrap window.
            // Clear the window flag so a failed radio-disable operation is
            // retried below until the AP mode bit is actually gone.
            temporaryAPMode = false;
            if (webPortal &&
                SecurityPolicy::decideApLifecycleAction(
                    SecurityPolicy::ApLifecycleEvent::StaReconnected, true) ==
                    SecurityPolicy::ApLifecycleAction::ShutDown)
            {
                webPortal->shutdownAccessPoint();
            }
        }
        // On transition away from connected, switch portal to AP mode so handlers reflect captive state
        else if (lifecycleLastWifiStatus == WL_CONNECTED && currentWifiStatus != WL_CONNECTED)
        {
            logger.warning("WiFi disconnected (transition). Switching web portal to AP mode.");
            if (webPortal &&
                SecurityPolicy::decideApLifecycleAction(
                    SecurityPolicy::ApLifecycleEvent::StaDisconnected, false) ==
                    SecurityPolicy::ApLifecycleAction::EnsureRunning)
            {
                webPortal->setAPMode(true);
                // No immediate restart to avoid flapping; routes will still serve config via handlers
            }
        }
        lifecycleLastWifiStatus = currentWifiStatus;
    }

    // Check timer for temporary AP mode
    if (temporaryAPMode && !configManager->configMode && configManager->wifiSSID.length() > 0)
    {
        if (millis() - apStartTime > AP_TIMEOUT)
        {
            const bool staConnected = WiFi.status() == WL_CONNECTED;
            const SecurityPolicy::ApLifecycleAction action =
                SecurityPolicy::decideApLifecycleAction(
                    SecurityPolicy::ApLifecycleEvent::TemporaryWindowExpired,
                    staConnected);
            if (action == SecurityPolicy::ApLifecycleAction::ShutDown)
            {
                logger.info("Temporary AP timeout reached. Shutting down AP radio.");
                if (webPortal)
                {
                    webPortal->shutdownAccessPoint();
                }
            }
            else
            {
                logger.info("Temporary AP timeout reached but WiFi client not connected. Keeping AP mode active.");
            }
            // If disconnected, the AP stays up, but a later STA transition
            // still runs the same shutdown operation above.
            temporaryAPMode = false;
        }
    }

    // A radio-mode change can fail transiently. Once the temporary bootstrap
    // window is over (or a reconnect closed it early), retry AP-only shutdown
    // without touching an established STA association.
    static unsigned long lastApShutdownRetry = 0;
    if (!temporaryAPMode && WiFi.status() == WL_CONNECTED && webPortal &&
        webPortal->isAccessPointActive() &&
        millis() - lastApShutdownRetry > 5000)
    {
        lastApShutdownRetry = millis();
        logger.warning("AP radio still active while STA is connected; retrying shutdown");
        webPortal->shutdownAccessPoint();
    }

    // Process LoRa packets
    // if (loraModule && loraProtocol) {
    //    if (LoRaModule::hasInterrupt()) {
    //        logger.debug("LoRa interrupt detected");
    //        loraProtocol->processReceivedPacket();
    //    }
    //}

    // If in AP mode, process DNS captive portal:
    if (webPortal && webPortal->isInAPMode())
    {
        webPortal->processDNS(); // This method internally calls dnsServer.processNextRequest();
    }

    // Handle web interface
    if (webPortal)
    {
        webPortal->handleClient();
    }

    // Process MQTT communication
    if (mqttManager && WiFi.status() == WL_CONNECTED)
    {
        mqttManager->process();
    }

    // Add a check for LoRa packet processing to publish to MQTT
    static unsigned int lastProcessedIndex = -1;
    if (loraModule && loraProtocol && LoRaModule::hasInterrupt())
    {
        if (loraProtocol->processReceivedPacket())
        {
            // If we have a mqttManager and it's connected, publish the latest sensor data
            if (mqttManager && WiFi.status() == WL_CONNECTED)
            {
                mqttManager->publishSensorData(loraProtocol->getLastProcessedSensorIndex());
            }
        }
    }

    // Check WiFi connection and reconnect if needed.
    // Non-blocking state machine: kick off WiFi.begin() once per
    // WIFI_RECONNECT_INTERVAL window, then poll status across loop iterations
    // up to RECONNECT_ATTEMPT_TIMEOUT_MS before giving up. Keeps web/DNS/MQTT/LoRa
    // responsive throughout the reconnect window.
    {
        enum class ReconnectState { Idle, InProgress };
        static ReconnectState reconnectState = ReconnectState::Idle;
        static unsigned long reconnectStartedAt = 0;
        constexpr unsigned long RECONNECT_ATTEMPT_TIMEOUT_MS = 5000; // matches old 10 * 500ms

        if (!configManager->configMode && WiFi.status() != WL_CONNECTED)
        {
            unsigned long now = millis();

            if (reconnectState == ReconnectState::Idle)
            {
                if (now - configManager->lastWifiAttempt > WIFI_RECONNECT_INTERVAL)
                {
                    logger.info("Attempting to reconnect to WiFi...");
                    configManager->lastWifiAttempt = now;
                    WiFi.begin(configManager->wifiSSID.c_str(), configManager->wifiPassword.c_str());
                    reconnectStartedAt = now;
                    reconnectState = ReconnectState::InProgress;
                }
            }
            else // ReconnectState::InProgress
            {
                if (WiFi.status() == WL_CONNECTED)
                {
                    logger.info("WiFi reconnected! IP: " + WiFi.localIP().toString());
                    // NTP sync verification (note: this call may still block briefly)
                    if (syncNTPTime(configManager->timezone))
                    {
                        logger.info("NTP time re-synchronized successfully");
                        logger.setTimeInitialized(true);
                    }
                    else
                    {
                        logger.warning("NTP re-sync failed after WiFi reconnect");
                    }
                    reconnectState = ReconnectState::Idle;
                }
                else if (now - reconnectStartedAt > RECONNECT_ATTEMPT_TIMEOUT_MS)
                {
                    logger.warning("Failed to reconnect to WiFi");
                    reconnectState = ReconnectState::Idle;
                }
                // else: keep waiting; loop continues to service web/MQTT/LoRa
            }
        }
        else if (reconnectState == ReconnectState::InProgress)
        {
            // Connected (or no longer trying) via some other path while we were waiting.
            reconnectState = ReconnectState::Idle;
        }
    }

    // Short delay for stability
    delay(5);

    // Periodically rotate per-sensor health buckets so a dead-silent sensor
    // (no packets at all) eventually drops out of the 24h "OK" tally instead
    // of clinging to whatever was in the last bucket at boot.
    static unsigned long lastHealthTick = 0;
    if (millis() - lastHealthTick > 60000) // once a minute is plenty
    {
        lastHealthTick = millis();
        if (sensorManager)
        {
            sensorManager->tickSensorHealth(millis());
        }
    }

    // Memory diagnostics and time check every 10 minutes
    static unsigned long lastMemCheck = 0;
    if (millis() - lastMemCheck > 600000)
    { // 10 minutes
        lastMemCheck = millis();

        logger.info("Memory status - Free heap: " + String(ESP.getFreeHeap()) +
                    " bytes, Largest block: " + String(ESP.getMaxAllocHeap()) + " bytes");

#ifdef BOARD_HAS_PSRAM
        if (esp_spiram_is_initialized())
        {
            logger.debug("PSRAM status - Free: " + String(ESP.getFreePsram()) +
                         " bytes, Largest block: " + String(ESP.getMaxAllocPsram()) + " bytes");
        }
#endif

        // Periodic time check - re-sync if time was lost
        struct tm timeinfo;
        if (!getLocalTime(&timeinfo, 100))
        {
            logger.warning("System time invalid - attempting NTP re-sync");
            if (WiFi.status() == WL_CONNECTED)
            {
                if (syncNTPTime(configManager->timezone))
                {
                    logger.info("NTP time recovered successfully");
                    logger.setTimeInitialized(true);
                }
                else
                {
                    logger.error("NTP re-sync failed - time remains unavailable");
                    logger.setTimeInitialized(false);
                }
            }
            else
            {
                logger.warning("Cannot sync time - WiFi not connected");
                logger.setTimeInitialized(false);
            }
        }
    }
}
