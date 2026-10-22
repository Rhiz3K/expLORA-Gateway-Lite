/**
 * expLORA Gateway
 *
 * Configuration manager header file
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

#pragma once

#include <Arduino.h>
#include <mutex>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "../Data/Logging.h"
#include "../config.h"

/**
 * Class for device configuration management
 *
 * Configuration is kept in two stores, both written by save():
 * 1. LittleFS (config.json) - WiFi, MQTT, log level, timezone
 * 2. Preferences (NVS) - a copy of the above plus settings kept only there
 *    (LTE, Ethernet, admin password); on load Preferences take precedence
 */
class ConfigManager
{
private:
    std::recursive_mutex configMutex;
    Logger &logger;              // Reference to logger
    Preferences preferences;     // For persistent configuration
    bool preferencesInitialized; // Preferences initialization flag
    String adminCredential;      // First-boot bootstrap password (RAM only)
    String apCredential;         // Separate WPA2 recovery AP secret
    bool adminCredentialGeneratedThisBoot = false;
    bool apCredentialGeneratedThisBoot = false;

    // Configuration files
    const char *configFile; // Configuration file

    // Loading and saving from/to LittleFS
    bool loadFromFS();
    bool saveToFS();

    // Loading and saving from/to Preferences
    bool loadFromPreferences();
    bool saveToPreferences();
    bool ensureDeviceCredentials();
    String generateCredential() const;
    bool persistCredential(const char *key, const String &credential);

public:
    // Device configuration
    String wifiSSID;               // WiFi network SSID
    String wifiPassword;           // WiFi network password
    bool configMode;               // Configuration mode (AP mode)
    unsigned long lastWifiAttempt; // Time of last WiFi connection attempt
    LogLevel logLevel;             // Logging level
    String timezone;               // Timezone in Posix format

    // MQTT Configuration
    String mqttHost;     // MQTT broker hostname
    int mqttPort;        // MQTT broker port
    String mqttUser;     // MQTT username
    String mqttPassword; // MQTT password
    bool mqttEnabled;    // MQTT enabled flag
    bool mqttTls;        // MQTTS flag
    String mqttPrefix;   // MQTT Topic prefix
    String mqttHAPrefix; // MQTT Homeassistant topic prefix
    bool mqttHAEnabled;  // MQTT Homeassistant discovery enable

    // LTE configuration (boards with HAS_LTE; stored in Preferences only)
    bool lteEnabled; // Modem enabled
    String lteApn;   // Access point name
    String ltePin;   // SIM PIN (empty = none)
    bool ltePinRejected; // The SIM refused ltePin - never sent again until it changes

    // Ethernet configuration (boards with HAS_ETHERNET; stored in Preferences only)
    bool ethStatic;     // Static address instead of DHCP
    String ethIp;       // Static address
    String ethMask;     // Subnet mask
    String ethGateway;  // Default gateway
    String ethDns1;     // Primary DNS (empty = gateway)
    String ethDns2;     // Secondary DNS (optional)

    // Web admin password, "pbkdf2$..." (see WebAuth); empty = not set yet
    String adminPasswordHash;

    // Held by save()/load() and the setters (web and loop task both write);
    // readers in another task take it while copying values (MQTTManager)
    std::recursive_mutex &mutex() { return configMutex; }

    // Constructor
    ConfigManager(Logger &log, const char *file = CONFIG_FILE);

    // Destructor
    ~ConfigManager();

    // Initialization
    bool init();

    // Load configuration from both sources
    bool load();

    // Save configuration to both sources
    bool save();

    // Reset configuration to default values
    void resetToDefaults();

    // Get firmware version
    String getFirmwareVersion() const;

    // Get device MAC address
    String getMacAddress() const;

    // Get device name for AP mode
    String getDeviceName() const;

    // Separate per-device credentials for HTTP administration and the WPA2 AP.
    // Values are intentionally available only to local firmware components.
    const String &getAdminCredential() const;
    const String &getApCredential() const;
    bool wasAdminCredentialGeneratedThisBoot() const;
    bool wasApCredentialGeneratedThisBoot() const;
    void clearBootstrapAdminCredential() { adminCredential = ""; }
    bool rotateAdminCredential(String &newCredential);
    bool rotateApCredential(String &newCredential);

    // Set WiFi configuration
    bool setWiFiConfig(const String &ssid, const String &password, bool saveConfig = true);

    // Switch to configuration mode (AP)
    void enableConfigMode(bool enable = true, bool saveConfig = true);

    // Set MQTT configuration
    bool setMqttConfig(const String &host, int port, const String &user,
                       const String &password, bool enabled, bool tls,
                       const String &rootPrefix, const String &haPrefix, bool haEnable,
                       bool saveConfig = true);

    // Set LTE configuration
    bool setLteConfig(bool enabled, const String &apn, const String &pin, bool saveConfig = true);

    // Set ethernet addressing; returns an error message, empty on success
    String setEthernetConfig(bool useStatic, const String &ip, const String &mask, const String &gateway,
                             const String &dns1, const String &dns2, bool saveConfig = true);

    // Remember that the SIM rejected the stored PIN (survives restarts, so a
    // wrong PIN is not retried into a PUK lock)
    void setLtePinRejected(bool rejected);

    // Store the admin password hash (Preferences only)
    bool setAdminPasswordHash(const String &hash);

    // Set logging level
    void setLogLevel(LogLevel level, bool saveConfig = true);


    // MQTT topic prefix check (no wildcards, no leading/trailing slash)
    static bool isValidMqttTopic(const String &topic);
};
