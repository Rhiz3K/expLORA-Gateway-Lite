/**
 * expLORA Gateway Lite
 *
 * MQTT communication manager implementation file
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

#include "MQTTManager.h"
#include <ArduinoJson.h>
#include "config.h"
#include "../Security/OtaTrust.h"
#include "MQTTDiscoveryPlan.h"

// Constructor
MQTTManager::MQTTManager(SensorManager &sensors, ConfigManager &config, Logger &log)
    : mqttClient(), sensorManager(sensors), configManager(config), logger(log),
      lastReconnectAttempt(0), lastDiscoveryUpdate(0)
{

    // Generate unique client ID from MAC address
    clientId = "explora-gw-";
    uint8_t mac[6];
    WiFi.macAddress(mac);
    for (int i = 0; i < 6; i++)
    {
        if (mac[i] < 0x10)
            clientId += '0';
        clientId += String(mac[i], HEX);
    }
}

// Initialization
bool MQTTManager::init()
{
    // Only initialize if MQTT is enabled in configuration
    if (!configManager.mqttEnabled)
    {
        logger.info("MQTT integration disabled in configuration");
        return false;
    }

    if (configManager.mqttTls) {
        configurePublicCaTls(wifiClientSecure);
        mqttClient.setClient(wifiClientSecure);
    }
    else {
        mqttClient.setClient(wifiClient);
    }
    mqttClientReady = true;

    mqttClient.setBufferSize(1024); // Increase to accommodate larger messages

    // Configure MQTT client
    mqttClient.setServer(configManager.mqttHost.c_str(), configManager.mqttPort);
    mqttClient.setSocketTimeout(5);

    // Log initialization
    logger.info("MQTT client initialized with configured broker");

    return true;
}

// Connect to MQTT broker
bool MQTTManager::connect()
{
    if (!mqttClientReady) {
        logger.error("MQTT client not initialized. Wait for init() first.");
        return false;
    }
    
    logger.debug("Attempting to connect to MQTT broker...");

    bool connected = false;

    // Connect with credentials if provided
    if (configManager.mqttUser.length() > 0)
    {
        connected = mqttClient.connect(
            clientId.c_str(),
            configManager.mqttUser.c_str(),
            configManager.mqttPassword.c_str());
    }
    else
    {
        connected = mqttClient.connect(clientId.c_str());
    }

    if (connected)
    {
        logger.info("Connected to MQTT broker");

        // Publish discovery information after successful connection
        publishDiscovery();

        // TODO: Some better idea than magic number delay?
        // Give HA time to subscribe this topic
        delay(500);
        mqttClient.publish(String(configManager.mqttPrefix + "/status").c_str(), "online", true);
    }
    else
    {
        logger.warning("Failed to connect to MQTT broker, error code: " + String(mqttClient.state()));
    }

    return connected;
}

// Process MQTT communication (call in main loop)
void MQTTManager::process()
{
    // Skip if MQTT is disabled in configuration
    if (!configManager.mqttEnabled || !mqttClientReady)
    {
        return;
    }

    // Check connection to WiFi and MQTT broker
    if (WiFi.status() == WL_CONNECTED)
    {
        if (!mqttClient.connected())
        {
            // Try to reconnect on interval
            unsigned long now = millis();
            if (now - lastReconnectAttempt > MQTT_RECONNECT_INTERVAL)
            {
                lastReconnectAttempt = now;
                if (connect())
                {
                    lastReconnectAttempt = 0;
                }
            }
        }
        else
        {
            // Process MQTT loop
            mqttClient.loop();

            // Republish discovery information periodically (every hour)
            unsigned long now = millis();
            if (now - lastDiscoveryUpdate > 3600000)
            { // 1 hour
                lastDiscoveryUpdate = now;
                publishDiscovery();
            }
        }
    }
}

// Publish discovery configuration for sensors
void MQTTManager::publishDiscovery()
{
    if (!configManager.mqttHAEnabled) {
        return; // Skip discovery if HA is disabled
    }

    // Skip if not connected
    if (!mqttClient.connected())
    {
        return;
    }

    logger.info("Publishing Home Assistant discovery information...");

    // Get all active sensors
    std::vector<SensorData> sensors = sensorManager.getActiveSensors();

    // Publish discovery information for each sensor
    for (const auto &sensor : sensors)
    {
        publishDiscoverySnapshot(sensor);
    }

    logger.info("Home Assistant discovery completed for " + String(sensors.size()) + " sensors");
    lastDiscoveryUpdate = millis();
}

// Create discovery topic for sensor
String MQTTManager::buildDiscoveryTopic(const SensorData &sensor, const String &valueType)
{
    return String(configManager.mqttHAPrefix) + "/sensor/" +
           String(configManager.mqttPrefix) + "_" + formatSN(sensor.serialNumber) + "_" + valueType + "/config";
}

// Helper function to capitalize first letter
String MQTTManager::capitalizeFirst(const String &input)
{
    if (input.length() == 0)
        return input;
    String result = input;
    result.setCharAt(0, toupper(input.charAt(0)));
    return result;
}

// Create configuration JSON for Home Assistant discovery
String MQTTManager::buildDiscoveryJson(const SensorData &sensor, const String &valueType, const String &stateTopic)
{
    // Create JSON document for discovery payload
    DynamicJsonDocument doc(1024);

    // Entity name
    // doc["name"] = sensor.name + " " + capitalizeFirst(valueType);

    // Use the name from the sensor if it ends with the value type
    if (sensor.name.endsWith(capitalizeFirst(valueType)))
    {
        doc["name"] = sensor.name;
    }
    else
    {
        doc["name"] = capitalizeFirst(valueType); // Just the value type
    }

    // State topic
    doc["state_topic"] = stateTopic;

    // Value template - just use raw value
    doc["value_template"] = "{{ value }}";

    // Unique ID
    doc["unique_id"] = String(configManager.mqttPrefix) + "_" + formatSN(sensor.serialNumber) + "_" + valueType;

    // Availability topic - use LWT (Last Will and Testament)
    doc["availability_topic"] = String(configManager.mqttPrefix) + "/status";
    doc["payload_available"] = "online";
    doc["payload_not_available"] = "offline";

    // Set measurement units based on value type
    if (valueType == "temperature")
    {
        doc["device_class"] = "temperature";
        doc["unit_of_measurement"] = "°C";
        doc["suggested_display_precision"] = 1;
    }
    else if (valueType == "humidity")
    {
        doc["device_class"] = "humidity";
        doc["unit_of_measurement"] = "%";
        doc["suggested_display_precision"] = 1;
    }
    else if (valueType == "pressure")
    {
        doc["device_class"] = "pressure";
        doc["unit_of_measurement"] = "hPa";
        doc["suggested_display_precision"] = 1;
    }
    else if (valueType == "co2")
    {
        doc["device_class"] = "carbon_dioxide";
        doc["unit_of_measurement"] = "ppm";
    }
    else if (valueType == "illuminance")
    {
        doc["device_class"] = "illuminance";
        doc["unit_of_measurement"] = "lx";
        doc["suggested_display_precision"] = 1;
    }
    else if (valueType == "wind_speed")
    {
        doc["device_class"] = "wind_speed";
        doc["unit_of_measurement"] = "m/s";
        doc["suggested_display_precision"] = 1;
    }
    else if (valueType == "wind_direction")
    {
        doc["device_class"] = "wind_direction";
        doc["unit_of_measurement"] = "°";
    }
    else if (valueType == "rain_amount")
    {
        doc["device_class"] = "precipitation";
        doc["unit_of_measurement"] = "mm";
        doc["suggested_display_precision"] = 1;
    }
    else if (valueType == "daily_rain")
    {
        doc["device_class"] = "precipitation";
        doc["unit_of_measurement"] = "mm";
        doc["suggested_display_precision"] = 1;
        doc["name"] = sensor.name + " Daily Rain Total";
    }
    else if (valueType == "rain_rate")
    {
        doc["device_class"] = "precipitation_intensity";
        doc["unit_of_measurement"] = "mm/h";
        doc["suggested_display_precision"] = 1;
    }
    else if (valueType == "battery")
    {
        doc["device_class"] = "voltage";
        doc["unit_of_measurement"] = "V";
        doc["suggested_display_precision"] = 2;
    }
    else if (valueType == "rssi")
    {
        doc["device_class"] = "signal_strength";
        doc["unit_of_measurement"] = "dBm";
    }

    // Device information
    JsonObject device = doc.createNestedObject("device");
    device["identifiers"] = formatSN(sensor.serialNumber);
    device["name"] = sensor.name;
    device["model"] = sensor.getTypeInfo().name;
    device["manufacturer"] = "expLORA";

    // Serialize JSON to string
    String payload;
    serializeJson(doc, payload);
    return payload;
}

// Publish sensor data
void MQTTManager::publishSensorData(int sensorIndex)
{
    // Skip if not connected
    if (!mqttClient.connected() || !configManager.mqttEnabled)
    {
        return;
    }

    SensorData sensor;
    if (!sensorManager.getSensorSnapshot(sensorIndex, sensor))
    {
        return;
    }

    // Base topic for this sensor
    String baseTopic = String(configManager.mqttPrefix) + "/" + formatSN(sensor.serialNumber);

    // Publish each value based on sensor type
    if (sensor.hasTemperature())
    {
        mqttClient.publish((baseTopic + "/temperature").c_str(),
                           String(sensor.temperature, 2).c_str());
    }

    if (sensor.hasHumidity())
    {
        mqttClient.publish((baseTopic + "/humidity").c_str(),
                           String(sensor.humidity, 2).c_str());
    }

    if (sensor.hasPressure())
    {
        mqttClient.publish((baseTopic + "/pressure").c_str(),
                           String(sensor.pressure, 2).c_str());
    }

    if (sensor.hasPPM())
    {
        mqttClient.publish((baseTopic + "/co2").c_str(),
                           String(int(sensor.ppm)).c_str());
    }

    if (sensor.hasLux())
    {
        mqttClient.publish((baseTopic + "/illuminance").c_str(),
                           String(sensor.lux, 1).c_str());
    }

    if (sensor.hasWindSpeed())
    {
        mqttClient.publish((baseTopic + "/wind_speed").c_str(),
                           String(sensor.windSpeed, 1).c_str());
    }

    if (sensor.hasWindDirection())
    {
        mqttClient.publish((baseTopic + "/wind_direction").c_str(),
                           String(sensor.windDirection).c_str());
    }

    if (sensor.hasRainAmount())
    {
        mqttClient.publish((baseTopic + "/rain_amount").c_str(),
                           String(sensor.rainAmount, 1).c_str());
        mqttClient.publish((baseTopic + "/daily_rain").c_str(),
                           String(sensor.dailyRainTotal, 1).c_str());
    }

    if (sensor.hasRainRate())
    {
        mqttClient.publish((baseTopic + "/rain_rate").c_str(),
                           String(sensor.rainRate, 1).c_str());
    }

    // Battery voltage - available for all sensors
    mqttClient.publish((baseTopic + "/battery").c_str(),
                       String(sensor.batteryVoltage, 2).c_str());

    // RSSI - available for all sensors
    mqttClient.publish((baseTopic + "/rssi").c_str(),
                       String(sensor.rssi).c_str());

    logger.debug("Published MQTT data for sensor: " + sensor.name);
}

void MQTTManager::publishDiscoveryValue(const SensorData &sensor,
                                        const String &valueType,
                                        const String &stateTopic)
{
    const String topic = buildDiscoveryTopic(sensor, valueType);
    const String payload = buildDiscoveryJson(sensor, valueType, stateTopic);
    if (!mqttClient.publish(topic.c_str(), payload.c_str(), true))
    {
        logger.warning("Failed to publish " + valueType +
                       " discovery for " + sensor.name);
        return;
    }
    logger.debug("Published " + valueType + " discovery for " + sensor.name);
}

void MQTTManager::publishDiscoverySnapshot(const SensorData &sensor)
{
    const String baseTopic =
        String(configManager.mqttPrefix) + "/" + formatSN(sensor.serialNumber);
    MQTTDiscoveryPlan::Capabilities capabilities;
    capabilities.temperature = sensor.hasTemperature();
    capabilities.humidity = sensor.hasHumidity();
    capabilities.pressure = sensor.hasPressure();
    capabilities.co2 = sensor.hasPPM();
    capabilities.illuminance = sensor.hasLux();
    capabilities.windSpeed = sensor.hasWindSpeed();
    capabilities.windDirection = sensor.hasWindDirection();
    capabilities.rainAmount = sensor.hasRainAmount();
    capabilities.rainRate = sensor.hasRainRate();

    MQTTDiscoveryPlan::forEachValue(
        capabilities,
        [&](const char *valueType)
        {
            const String type(valueType);
            publishDiscoveryValue(sensor, type, baseTopic + "/" + type);
        });
}

void MQTTManager::publishDiscoveryForSensor(int sensorIndex)
{
    if (!configManager.mqttHAEnabled || !mqttClient.connected())
    {
        return;
    }

    SensorData sensor;
    if (!sensorManager.getSensorSnapshot(sensorIndex, sensor))
    {
        return;
    }
    logger.info("Publishing MQTT discovery for sensor: " + sensor.name);
    publishDiscoverySnapshot(sensor);
}

// Check connection
bool MQTTManager::isConnected()
{
    return mqttClient.connected() && configManager.mqttEnabled;
}

// Disconnect from MQTT broker
void MQTTManager::disconnect()
{
    if (mqttClient.connected())
    {
        logger.info("Disconnecting from MQTT broker");
        mqttClient.disconnect();
    }
}

// Remove discovery for deleted sensor
void MQTTManager::removeDiscoveryForSensor(uint32_t serialNumber)
{
    if (!configManager.mqttHAEnabled) {
        return; // Do not send discovery if HA is disabled
    }

    if (!mqttClient.connected())
    {
        return;
    }

    logger.info("Removing MQTT discovery for sensor with SN: " + formatSN(serialNumber));

    // Create discovery topics for all possible value types
    String baseDiscoveryTopic = String(configManager.mqttHAPrefix) + "/sensor/" + String(configManager.mqttPrefix) + "_" +
                                formatSN(serialNumber) + "_";

    // Remove discovery for all possible types
    String types[] = {"temperature", "humidity", "pressure", "co2", "illuminance",
                      "battery", "rssi", "wind_speed", "wind_direction",
                      "rain_amount", "daily_rain", "rain_rate"};

    for (const auto &type : types)
    {
        String topic = baseDiscoveryTopic + type + "/config";
        mqttClient.publish(topic.c_str(), "", true); // Empty payload deletes discovery
    }
}
