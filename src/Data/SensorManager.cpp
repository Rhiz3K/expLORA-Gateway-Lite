/**
 * expLORA Gateway
 *
 * Sensor manager implementation file
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

#include "SensorManager.h"
#include "SensorCalibration.h"
#include "SensorStoragePolicy.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include "../Security/OtaTrust.h"
#include <WiFiClientSecure.h>
#include <memory>
#include "../Network/Uplink.h"
#include "../Hardware/WatchdogPause.h"

// Constructor
SensorManager::SensorManager(Logger &log, const char *file)
    : sensorCount(0), logger(log), storageLoadedSuccessfully(true), sensorsFile(file)
{
}

// Destructor
SensorManager::~SensorManager()
{
    // Nothing special needs to be released, all objects will be released automatically
}

// Sensor manager initialization
bool SensorManager::init()
{
    logger.info("Initializing sensor manager");
    return loadSensors();
}

// Constants for pressure conversion
#define G 9.80665   // gravitational acceleration [m/s^2]
#define M 0.0289644 // molar mass of air [kg/mol]
#define R 8.3144598 // universal gas constant [J/(mol·K)]
#define L 0.0065    // temperature gradient [K/m]

// Convert relative pressure to absolute pressure
double SensorManager::relativeToAbsolutePressure(double p_rel_hpa, int altitude_m, double temp_c)
{
    if (altitude_m == 0)
    {
        return p_rel_hpa;
    }
    double T = temp_c + 273.15;
    double exponent = (G * M) / (R * L);
    return p_rel_hpa / pow(1 - (L * altitude_m) / T, exponent);
}

// Add a new sensor
int SensorManager::addSensor(SensorType deviceType, uint32_t serialNumber, uint32_t deviceKey, const String &name)
{
    std::lock_guard<std::mutex> lock(sensorMutex);

    if (name.length() > MAX_SENSOR_NAME_LENGTH)
    {
        logger.error("Failed to add sensor: name exceeds maximum length");
        return -1;
    }

    // A second sensor with the same serial number would never receive data
    // (and silently replacing the key of the first one is worse)
    if (findSensorBySN(serialNumber) >= 0)
    {
        logger.warning("Failed to add sensor: SN " + formatSN(serialNumber) + " already exists");
        return -2;
    }

    // First free slot (deleted sensors leave free slots below sensorCount)
    int newIndex = -1;
    for (size_t i = 0; i < MAX_SENSORS; i++)
    {
        if (!sensors[i].configured)
        {
            newIndex = i;
            break;
        }
    }
    if (newIndex < 0)
    {
        logger.error("Failed to add sensor: maximum number of sensors reached");
        return -1;
    }
    if ((size_t)newIndex >= sensorCount)
    {
        sensorCount = newIndex + 1;
    }

    // Fresh state - a reused slot must not inherit the deleted sensor's
    // health counters, rain totals or corrections
    sensors[newIndex] = SensorData();
    sensors[newIndex].deviceType = deviceType;
    sensors[newIndex].serialNumber = serialNumber;
    sensors[newIndex].deviceKey = deviceKey;
    sensors[newIndex].name = name;
    sensors[newIndex].configured = true;
    pendingAdds[newIndex] = true;

    // Not saved yet: the web form's updateSensorConfig() saves the complete
    // sensor once (no half-configured sensor on flash if that fails)
    logger.info("Added new sensor: " + name + " (SN: " + formatSN(serialNumber) + ")");
    return newIndex;
}

bool SensorManager::discardUnsavedSensor(int index)
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    if (index < 0 || static_cast<size_t>(index) >= sensorCount || !pendingAdds[index])
    {
        return false;
    }
    sensors[index] = SensorData();
    pendingAdds[index] = false;
    while (sensorCount > 0 && !sensors[sensorCount - 1].configured)
    {
        --sensorCount;
    }
    return true;
}

// Find sensor by serial number
int SensorManager::findSensorBySN(uint32_t serialNumber)
{
    for (size_t i = 0; i < sensorCount; i++)
    {
        if (sensors[i].configured && sensors[i].serialNumber == serialNumber)
        {
            return i;
        }
    }
    return -1;
}

// Update sensor data
bool SensorManager::updateSensor(int index, const SensorData &data)
{
    std::lock_guard<std::mutex> lock(sensorMutex);

    if (index < 0 || index >= sensorCount || !sensors[index].configured)
    {
        logger.warning("Attempt to update non-existent sensor at index " + String(index));
        return false;
    }

    // Update data while preserving configuration
    sensors[index].deviceType = data.deviceType;
    sensors[index].temperature = data.temperature;
    sensors[index].humidity = data.humidity;
    sensors[index].pressure = data.pressure;
    sensors[index].ppm = data.ppm;
    sensors[index].lux = data.lux;
    sensors[index].batteryVoltage = data.batteryVoltage;
    sensors[index].rssi = data.rssi;
    sensors[index].lastSeen = millis();

    return true;
}

// Update sensor data by type
bool SensorManager::updateSensorData(int index, float temperature, float humidity, float pressure,
                                     float ppm, float lux, float batteryVoltage, int rssi,
                                     float windSpeed, uint16_t windDirection,
                                     float rainAmount, float rainRate)
{
    std::unique_lock<std::mutex> lock(sensorMutex);

    if (index < 0 || index >= sensorCount || !sensors[index].configured)
    {
        logger.warning("Attempt to update non-existent sensor at index " + String(index));
        return false;
    }

    // Store original values for logging
    float originalTemp = temperature;
    float originalHum = humidity;
    float originalPress = pressure;
    float originalPPM = ppm;
    float originalLux = lux;
    float originalWindSpeed = windSpeed;
    uint16_t originalWindDir = windDirection;
    float originalRainAmount = rainAmount;
    float originalRainRate = rainRate;

    // Apply corrections to input values before updating.
    // Delegated to SensorCalibration (covered by native unit tests).
    // Note: applyWindDirectionCorrection fixes a latent C++ integer-promotion
    // bug in the previous inline (uint16_t + int) % 360 path that wrapped
    // negative offsets to ~65k° instead of e.g. 350°.
    temperature = SensorCalibration::applyOffset(temperature, sensors[index].temperatureCorrection);
    humidity    = SensorCalibration::applyOffset(humidity,    sensors[index].humidityCorrection);
    pressure    = SensorCalibration::applyOffset(pressure,    sensors[index].pressureCorrection);
    ppm         = SensorCalibration::applyOffset(ppm,         sensors[index].ppmCorrection);
    lux         = SensorCalibration::applyOffset(lux,         sensors[index].luxCorrection);
    windSpeed   = SensorCalibration::applyMultiplier(windSpeed, sensors[index].windSpeedCorrection);
    windDirection = SensorCalibration::applyWindDirectionCorrection(
        windDirection, sensors[index].windDirectionCorrection);
    rainAmount  = SensorCalibration::applyMultiplier(rainAmount, sensors[index].rainAmountCorrection);
    rainRate    = SensorCalibration::applyMultiplier(rainRate,   sensors[index].rainRateCorrection);

    // Log if corrections were applied (only build string if debug enabled)
    if (logger.getLogLevel() >= LogLevel::DEBUG)
    {
        bool correctionsApplied = false;
        String correctionLog;
        correctionLog.reserve(128);
        correctionLog += "Corrections applied to ";
        correctionLog += sensors[index].name;
        correctionLog += ": ";

        if (sensors[index].hasTemperature() && sensors[index].temperatureCorrection != 0)
        {
            correctionLog += "Temp ";
            correctionLog += String(originalTemp, 2);
            correctionLog += "→";
            correctionLog += String(temperature, 2);
            correctionLog += "°C, ";
            correctionsApplied = true;
        }

        if (sensors[index].hasHumidity() && sensors[index].humidityCorrection != 0)
        {
            correctionLog += "Hum ";
            correctionLog += String(originalHum, 2);
            correctionLog += "→";
            correctionLog += String(humidity, 2);
            correctionLog += "%, ";
            correctionsApplied = true;
        }

        if (sensors[index].hasPressure() && sensors[index].pressureCorrection != 0)
        {
            correctionLog += "Press ";
            correctionLog += String(originalPress, 2);
            correctionLog += "→";
            correctionLog += String(pressure, 2);
            correctionLog += "hPa, ";
            correctionsApplied = true;
        }

        if (sensors[index].hasPPM() && sensors[index].ppmCorrection != 0)
        {
            correctionLog += "CO2 ";
            correctionLog += String(originalPPM, 0);
            correctionLog += "→";
            correctionLog += String(ppm, 0);
            correctionLog += "ppm, ";
            correctionsApplied = true;
        }

        if (sensors[index].hasLux() && sensors[index].luxCorrection != 0)
        {
            correctionLog += "Lux ";
            correctionLog += String(originalLux, 1);
            correctionLog += "→";
            correctionLog += String(lux, 1);
            correctionLog += "lx, ";
            correctionsApplied = true;
        }

        if (sensors[index].hasWindSpeed() && sensors[index].windSpeedCorrection != 1.0f)
        {
            correctionLog += "Wind ";
            correctionLog += String(originalWindSpeed, 1);
            correctionLog += "→";
            correctionLog += String(windSpeed, 1);
            correctionLog += "m/s, ";
            correctionsApplied = true;
        }

        if (sensors[index].hasWindDirection() && sensors[index].windDirectionCorrection != 0)
        {
            correctionLog += "Dir ";
            correctionLog += String(originalWindDir);
            correctionLog += "→";
            correctionLog += String(windDirection);
            correctionLog += "°, ";
            correctionsApplied = true;
        }

        if (sensors[index].hasRainAmount() && sensors[index].rainAmountCorrection != 1.0f)
        {
            correctionLog += "Rain ";
            correctionLog += String(originalRainAmount, 1);
            correctionLog += "→";
            correctionLog += String(rainAmount, 1);
            correctionLog += "mm, ";
            correctionsApplied = true;
        }

        if (sensors[index].hasRainRate() && sensors[index].rainRateCorrection != 1.0f)
        {
            correctionLog += "Rate ";
            correctionLog += String(originalRainRate, 1);
            correctionLog += "→";
            correctionLog += String(rainRate, 1);
            correctionLog += "mm/h, ";
            correctionsApplied = true;
        }

        if (correctionsApplied)
        {
            // Remove trailing ", " if present
            if (correctionLog.endsWith(", "))
            {
                correctionLog.remove(correctionLog.length() - 2);
            }
            logger.debug(correctionLog);
        }
    }

    // Adjust pressure for altitude
    // Only adjust if the sensor has pressure capability and altitude is set
    if (sensors[index].hasPressure() && sensors[index].altitude > 0)
    {
        float adjustedPressure = relativeToAbsolutePressure(pressure, sensors[index].altitude, temperature);
        logger.debug("Adjusted pressure from " + String(pressure, 2) + " hPa to " +
                     String(adjustedPressure, 2) + " hPa at altitude " +
                     String(sensors[index].altitude) + " m");
        pressure = adjustedPressure;
    }

    // Update only relevant values according to sensor type
    if (sensors[index].hasTemperature())
    {
        sensors[index].temperature = temperature;
    }

    if (sensors[index].hasHumidity())
    {
        sensors[index].humidity = humidity;
    }

    if (sensors[index].hasPressure())
    {
        sensors[index].pressure = pressure;
    }

    if (sensors[index].hasPPM())
    {
        sensors[index].ppm = ppm;
    }

    if (sensors[index].hasLux())
    {
        sensors[index].lux = lux;
    }

    // Meteorological data
    if (sensors[index].hasWindSpeed())
    {
        sensors[index].windSpeed = windSpeed;
    }

    if (sensors[index].hasWindDirection())
    {
        sensors[index].windDirection = windDirection;
    }

    if (sensors[index].hasRainAmount())
    {
        sensors[index].rainAmount = rainAmount;

        // Check if we need to reset daily total (new day)
        if (Logger::isTimeInitialized())
        { // Using method from Logger
            // Get current time
            struct tm timeinfo;
            time_t now;
            time(&now);

            if (getLocalTime(&timeinfo, 0))
            {
                // Create timestamps for comparing dates
                // Date of last reset
                struct tm lastResetTime;
                const time_t lastRainReset =
                    static_cast<time_t>(sensors[index].lastRainReset);
                localtime_r(&lastRainReset, &lastResetTime);

                // If last reset was on a different day than today, reset the counter
                if (sensors[index].lastRainReset == 0 ||
                    lastResetTime.tm_mday != timeinfo.tm_mday ||
                    lastResetTime.tm_mon != timeinfo.tm_mon ||
                    lastResetTime.tm_year != timeinfo.tm_year)
                {

                    logger.info("Resetting daily rain total for sensor: " + sensors[index].name);
                    sensors[index].dailyRainTotal = 0.0f;
                    sensors[index].lastRainReset = now;
                }
            }
        }

        // Add current rain amount to daily total
        sensors[index].dailyRainTotal += rainAmount;

        // Log the updated daily total if it has changed
        if (rainAmount > 0)
        {
            rainDirty = true; // saved from process(), not on every packet
        }
    }

    if (sensors[index].hasRainRate())
    {
        sensors[index].rainRate = rainRate;
    }

    // Always update general data
    sensors[index].batteryVoltage = batteryVoltage;
    sensors[index].rssi = rssi;
    sensors[index].lastSeen = millis();

    if (sensors[index].customUrl.length() == 0)
    {
        return true;
    }
    if (!uplink->isOnline())
    {
        logger.debug("Not forwarding data - offline");
        return true;
    }
    // The HTTP request blocks for up to seconds; web handlers wait on the lock
    SensorData copy = sensors[index];
    lock.unlock();
    forwardSensorData(copy);
    return true;
}

bool SensorManager::forwardSensorData(const SensorData &sensor)
{
    if (!sensor.configured)
    {
        return false;
    }

    // Check if the sensor has a custom endpoint configured
    if (sensor.customUrl.length() == 0)
    {
        // No endpoint configured, nothing to do
        return true;
    }

    // DNS has no timeout of its own and a TLS handshake can be slow; the
    // loop task stays off the watchdog for this request
    WatchdogPause pause;

    // Clients first: ~HTTPClient() may still touch its client (stop()), so
    // they must outlive it
    WiFiClient plainClient;
    std::unique_ptr<WiFiClientSecure> secureClient; // only for HTTPS
    HTTPClient http;
    http.setTimeout(3000);
    http.setConnectTimeout(3000);
    http.setReuse(false);

    // Format the custom URL by replacing placeholders
    String url = sensor.customUrl;

    // Skip if no URL is configured
    if (url.length() == 0)
    {
        return true;
    }

    // Check if HTTP or HTTPS
    bool isHttps = url.startsWith("https://");

    // Process the URL - replace placeholders with actual values
    if (sensor.hasTemperature())
    {
        url.replace("*TEMP*", String(sensor.temperature, 2));
    }
    if (sensor.hasHumidity())
    {
        url.replace("*HUM*", String(sensor.humidity, 2));
    }
    if (sensor.hasPressure())
    {
        url.replace("*PRESS*", String(sensor.pressure, 2));
    }
    if (sensor.hasPPM())
    {
        url.replace("*PPM*", String(sensor.ppm, 0));
    }
    if (sensor.hasLux())
    {
        url.replace("*LUX*", String(sensor.lux, 1));
    }

    // Added placeholders for METEO data
    if (sensor.hasWindSpeed())
    {
        url.replace("*WIND_SPEED*", String(sensor.windSpeed, 1));
    }
    if (sensor.hasWindDirection())
    {
        url.replace("*WIND_DIR*", String(sensor.windDirection));
    }
    if (sensor.hasRainAmount())
    {
        url.replace("*RAIN*", String(sensor.rainAmount, 1));
        url.replace("*DAILY_RAIN*", String(sensor.dailyRainTotal, 1)); // New placeholder
    }
    if (sensor.hasRainRate())
    {
        url.replace("*RAIN_RATE*", String(sensor.rainRate, 1));
    }

    // Replace other common placeholders
    url.replace("*BAT*", String(sensor.batteryVoltage, 2));
    url.replace("*RSSI*", String(sensor.rssi));
    url.replace("*SN*", formatSN(sensor.serialNumber));
    url.replace("*TYPE*", String(static_cast<uint8_t>(sensor.deviceType)));

    logger.debug("Forwarding data for sensor " + sensor.name +
                 " to its configured endpoint");

    // For HTTPS, use a secure client, but ensure we delete it after the request
    if (isHttps)
    {
        // A TLS session needs ~40 kB (Pro: next to an MQTT TLS session)
        if (ESP.getFreeHeap() < 48000 || ESP.getMaxAllocHeap() < 24000)
        {
            logger.warning("Not forwarding sensor data - not enough memory for HTTPS");
            return false;
        }
        secureClient.reset(new WiFiClientSecure());
        configurePublicCaTls(*secureClient);
        secureClient->setHandshakeTimeout(5);
        http.begin(*secureClient, url);
    }
    else
    {
        http.begin(plainClient, url);
    }

    // Send the request
    int httpCode = http.GET();

    // Check result
    if (httpCode > 0)
    {
        logger.debug("HTTP request sent, response code: " + String(httpCode));
        // The body is not read: only the status matters, and an endpoint
        // could send an unbounded (chunked) reply
    }
    else
    {
        logger.warning("HTTP request failed: " + http.errorToString(httpCode));
    }

    http.end();

    return (httpCode == HTTP_CODE_OK);
}

// Update sensor configuration
bool SensorManager::updateSensorConfig(int index, const String &name, SensorType deviceType,
                                       uint32_t serialNumber, uint32_t deviceKey,
                                       const String &customUrl, int altitude,
                                       float tempCorr, float humCorr, float pressCorr,
                                       float ppmCorr, float luxCorr,
                                       float windSpeedCorr, int windDirCorr,
                                       float rainAmountCorr, float rainRateCorr)
{
    std::lock_guard<std::mutex> lock(sensorMutex);

    if (name.length() > MAX_SENSOR_NAME_LENGTH ||
        customUrl.length() > MAX_SENSOR_CUSTOM_URL_LENGTH)
    {
        logger.warning("Rejected sensor configuration with oversized text fields");
        return false;
    }

    if (index < 0 || index >= sensorCount || !sensors[index].configured)
    {
        logger.warning("Attempt to update non-existent sensor at index " + String(index));
        return false;
    }

    // Check if serial number is already used by another sensor
    int existingIndex = findSensorBySN(serialNumber);
    if (existingIndex >= 0 && existingIndex != index)
    {
        logger.warning("Cannot update sensor config: Serial number " +
                       formatSN(serialNumber) + " already used by sensor " +
                       sensors[existingIndex].name);
        return false;
    }

    const SensorData previous = sensors[index];
    const bool wasPendingAdd = pendingAdds[index];
    // Another sensor now (type or serial number changed): its readings, rain
    // totals and health counters do not belong to it
    if (sensors[index].deviceType != deviceType || sensors[index].serialNumber != serialNumber)
    {
        SensorData fresh;
        fresh.configured = true;
        sensors[index] = fresh;
    }

    // Update basic configuration
    sensors[index].name = name;
    sensors[index].deviceType = deviceType;
    sensors[index].serialNumber = serialNumber;
    sensors[index].deviceKey = deviceKey;
    sensors[index].customUrl = customUrl;
    sensors[index].altitude = altitude;

    // Update correction values
    sensors[index].temperatureCorrection = tempCorr;
    sensors[index].humidityCorrection = humCorr;
    sensors[index].pressureCorrection = pressCorr;
    sensors[index].ppmCorrection = ppmCorr;
    sensors[index].luxCorrection = luxCorr;
    sensors[index].windSpeedCorrection = windSpeedCorr;
    sensors[index].windDirectionCorrection = windDirCorr;
    sensors[index].rainAmountCorrection = rainAmountCorr;
    sensors[index].rainRateCorrection = rainRateCorr;

    pendingAdds[index] = false; // This save includes the complete new configuration.
    logger.info("Updated configuration for sensor: " + name + " (SN: " + formatSN(serialNumber) + ")");
    if (!saveSensors(false))
    {
        sensors[index] = previous;
        pendingAdds[index] = wasPendingAdd;
        logger.error("Sensor configuration update rolled back because persistence failed");
        return false;
    }
    return true;
}

// Delete sensor
bool SensorManager::deleteSensor(int index)
{
    std::lock_guard<std::mutex> lock(sensorMutex);

    if (index < 0 || index >= sensorCount || !sensors[index].configured)
    {
        logger.warning("Attempt to delete non-existent sensor at index " + String(index));
        return false;
    }

    String name = sensors[index].name;
    uint32_t serialNumber = sensors[index].serialNumber;
    const bool wasConfigured = sensors[index].configured;

    // Mark as unconfigured instead of physically removing
    sensors[index].configured = false;

    logger.info("Deleted sensor: " + name + " (SN: " + formatSN(serialNumber) + ")");
    if (!saveSensors(false))
    {
        sensors[index].configured = wasConfigured;
        logger.error("Sensor delete rolled back because persistence failed");
        return false;
    }
    return true;
}

// Get number of sensors
size_t SensorManager::getSensorCount() const
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    return sensorCount;
}

bool SensorManager::getSensorSnapshot(int index, SensorData &snapshot) const
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    if (index < 0 || static_cast<size_t>(index) >= sensorCount ||
        !sensors[index].configured)
    {
        return false;
    }
    snapshot = sensors[index];
    return true;
}

// Get list of all sensors
std::vector<SensorData> SensorManager::getAllSensors() const
{
    std::lock_guard<std::mutex> lock(sensorMutex);

    std::vector<SensorData> result;
    result.reserve(sensorCount);
    for (size_t i = 0; i < sensorCount; i++)
    {
        result.push_back(sensors[i]);
    }
    return result;
}

std::vector<ActiveSensorEntry> SensorManager::getActiveSensorEntries() const
{
    std::lock_guard<std::mutex> lock(sensorMutex);

    std::vector<ActiveSensorEntry> result;
    result.reserve(sensorCount);
    for (size_t i = 0; i < sensorCount; ++i)
    {
        if (sensors[i].configured)
        {
            result.push_back({ i, sensors[i] });
        }
    }
    return result;
}

// Get list of all active sensors (configured)
std::vector<SensorData> SensorManager::getActiveSensors() const
{
    std::lock_guard<std::mutex> lock(sensorMutex);

    std::vector<SensorData> result;
    result.reserve(sensorCount);
    for (size_t i = 0; i < sensorCount; i++)
    {
        if (sensors[i].configured)
        {
            result.push_back(sensors[i]);
        }
    }
    return result;
}

bool SensorManager::getSensorCopy(int index, SensorData &out) const
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    if (index < 0 || index >= (int)sensorCount || !sensors[index].configured)
    {
        return false;
    }
    out = sensors[index];
    return true;
}

// Rain totals: at most one flash write per interval
static constexpr unsigned long RAIN_SAVE_INTERVAL = 600000;

void SensorManager::process(unsigned long now)
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    if (rainDirty && now - lastRainSave > RAIN_SAVE_INTERVAL)
    {
        // Next attempt after a full interval even when this one fails
        lastRainSave = now;
        saveSensorsLocked();
    }
}

// Save sensor configuration to file
bool SensorManager::saveSensors(bool lockMutex)
{
    std::unique_lock<std::mutex> lock(sensorMutex, std::defer_lock);
    if (lockMutex)
    {
        lock.lock();
    }
    return saveSensorsLocked();
}

// Caller holds sensorMutex; the previous file survives every failed save.
bool SensorManager::saveSensorsLocked()
{
    // A parse/open failure must not turn a recoverable sensors.json into an
    // empty or partial file on the next web request.
    const String temporaryPath = String(sensorsFile) + ".tmp";
    if (!storageLoadedSuccessfully &&
        (LittleFS.exists(sensorsFile) || LittleFS.exists(temporaryPath.c_str())))
    {
        logger.error("Refusing to overwrite sensor configuration after a failed load");
        return false;
    }

    size_t configuredCount = 0;
    size_t copiedStringBytes = 0;
    for (size_t i = 0; i < sensorCount; ++i)
    {
        if (!sensors[i].configured || pendingAdds[i])
        {
            continue;
        }
        ++configuredCount;
        copiedStringBytes += sensors[i].name.length() + 1;
        copiedStringBytes += sensors[i].customUrl.length() + 1;
    }

    DynamicJsonDocument doc(
        SensorStoragePolicy::documentCapacity(configuredCount, copiedStringBytes));
    if (doc.capacity() == 0)
    {
        logger.error("Not enough memory to save the sensor configuration");
        return false;
    }
    JsonArray sensorArray = doc.createNestedArray("sensors");

    // Add sensors to JSON
    for (size_t i = 0; i < sensorCount; i++)
    {
        if (sensors[i].configured && !pendingAdds[i])
        {
            JsonObject sensor = sensorArray.createNestedObject();
            sensor["deviceType"] = static_cast<uint8_t>(sensors[i].deviceType);
            sensor["serialNumber"] = sensors[i].serialNumber;
            sensor["deviceKey"] = sensors[i].deviceKey;
            sensor["name"] = sensors[i].name;
            sensor["customUrl"] = sensors[i].customUrl;
            sensor["altitude"] = sensors[i].altitude;

            // Also save daily rain total and time of last reset
            if (sensors[i].hasRainAmount())
            {
                sensor["dailyRainTotal"] = sensors[i].dailyRainTotal;
                sensor["lastRainReset"] = sensors[i].lastRainReset;
            }

            sensor["temperatureCorrection"] = sensors[i].temperatureCorrection;
            sensor["humidityCorrection"] = sensors[i].humidityCorrection;
            sensor["pressureCorrection"] = sensors[i].pressureCorrection;
            sensor["ppmCorrection"] = sensors[i].ppmCorrection;
            sensor["luxCorrection"] = sensors[i].luxCorrection;
            sensor["windSpeedCorrection"] = sensors[i].windSpeedCorrection;
            sensor["windDirectionCorrection"] = sensors[i].windDirectionCorrection;
            sensor["rainAmountCorrection"] = sensors[i].rainAmountCorrection;
            sensor["rainRateCorrection"] = sensors[i].rainRateCorrection;
        }
    }

    if (doc.overflowed() || sensorArray.size() != configuredCount)
    {
        logger.error("Sensor JSON capacity exhausted; existing configuration left unchanged");
        return false;
    }

    const size_t expectedBytes = measureJson(doc);
    if (expectedBytes == 0)
    {
        logger.error("Failed to measure sensor JSON");
        return false;
    }

    // Write a complete temporary file first. LittleFS rename uses the
    // filesystem's atomic replacement operation, so the previous file remains
    // valid until the new file has been fully flushed and closed.
    File file = LittleFS.open(temporaryPath.c_str(), "w");
    if (!file)
    {
        logger.error("Failed to open temporary sensor file for writing");
        return false;
    }

    const size_t bytesWritten = serializeJson(doc, file);
    file.flush();
    file.close();

    if (bytesWritten != expectedBytes)
    {
        logger.error("Failed to write complete sensor JSON; existing file preserved");
        LittleFS.remove(temporaryPath.c_str());
        return false;
    }

    if (!LittleFS.rename(temporaryPath.c_str(), sensorsFile))
    {
        logger.error("Failed to atomically replace sensor configuration");
        LittleFS.remove(temporaryPath.c_str());
        return false;
    }

    storageLoadedSuccessfully = true;
    rainDirty = false;
    lastRainSave = millis();
    logger.info("Saved " + String(sensorArray.size()) + " sensors to " + String(sensorsFile));
    return true;
}

// Load sensor configuration from file
bool SensorManager::loadSensors()
{
    std::lock_guard<std::mutex> lock(sensorMutex);

    // Power lost between removing the old file and renaming the new one
    String tmpFile = String(sensorsFile) + ".tmp";
    if (!LittleFS.exists(sensorsFile) && LittleFS.exists(tmpFile))
    {
        logger.warning("Recovering sensor configuration from " + tmpFile);
        if (!LittleFS.rename(tmpFile, sensorsFile))
        {
            logger.error("Failed to recover temporary sensor configuration; preserving it");
            storageLoadedSuccessfully = false;
            return false;
        }
    }

    // Check if file exists
    if (!LittleFS.exists(sensorsFile))
    {
        sensorCount = 0;
        for (size_t i = 0; i < MAX_SENSORS; ++i)
        {
            sensors[i] = SensorData();
            pendingAdds[i] = false;
        }
        storageLoadedSuccessfully = true;
        logger.info("Sensors file not found: " + String(sensorsFile) + ", starting with empty configuration");
        return true; // Not an error, we just start with an empty configuration
    }

    // Open file for reading
    File file = LittleFS.open(sensorsFile, "r");
    if (!file)
    {
        logger.error("Failed to open sensors file for reading: " + String(sensorsFile));
        storageLoadedSuccessfully = false;
        return false;
    }

    const size_t fileSize = file.size();
    if (fileSize == 0)
    {
        logger.error("Sensors file is empty; preserving it for recovery");
        file.close();
        storageLoadedSuccessfully = false;
        return false;
    }

    DynamicJsonDocument doc(
        SensorStoragePolicy::loadDocumentCapacity(fileSize, MAX_SENSORS));
    const DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error)
    {
        logger.error("Failed to parse sensors file: " + String(error.c_str()));
        storageLoadedSuccessfully = false;
        return false;
    }

    const JsonArrayConst sensorArray = doc["sensors"].as<JsonArrayConst>();
    if (sensorArray.isNull())
    {
        logger.error("Sensors file does not contain a sensors array");
        storageLoadedSuccessfully = false;
        return false;
    }
    if (sensorArray.size() > MAX_SENSORS)
    {
        logger.error("Sensors file exceeds MAX_SENSORS; existing configuration preserved");
        storageLoadedSuccessfully = false;
        return false;
    }

    std::vector<SensorData> loadedSensors;
    loadedSensors.reserve(sensorArray.size());
    for (JsonObjectConst sensorObj : sensorArray)
    {
        SensorData sensor;
        sensor.deviceType =
            static_cast<SensorType>(sensorObj["deviceType"].as<uint8_t>());
        sensor.serialNumber = sensorObj["serialNumber"].as<uint32_t>();
        sensor.deviceKey = sensorObj["deviceKey"].as<uint32_t>();
        sensor.name = sensorObj["name"].as<String>();
        sensor.customUrl = sensorObj["customUrl"].as<String>();
        sensor.configured = true;

        if (sensorObj.containsKey("dailyRainTotal"))
        {
            sensor.dailyRainTotal = sensorObj["dailyRainTotal"].as<float>();
        }
        if (sensorObj.containsKey("lastRainReset"))
        {
            sensor.lastRainReset = sensorObj["lastRainReset"].as<unsigned long>();
        }
        if (sensorObj.containsKey("altitude"))
        {
            sensor.altitude = sensorObj["altitude"].as<int>();
        }
        if (sensorObj.containsKey("temperatureCorrection"))
        {
            sensor.temperatureCorrection = sensorObj["temperatureCorrection"].as<float>();
        }
        if (sensorObj.containsKey("humidityCorrection"))
        {
            sensor.humidityCorrection = sensorObj["humidityCorrection"].as<float>();
        }
        if (sensorObj.containsKey("pressureCorrection"))
        {
            sensor.pressureCorrection = sensorObj["pressureCorrection"].as<float>();
        }
        if (sensorObj.containsKey("ppmCorrection"))
        {
            sensor.ppmCorrection = sensorObj["ppmCorrection"].as<float>();
        }
        if (sensorObj.containsKey("luxCorrection"))
        {
            sensor.luxCorrection = sensorObj["luxCorrection"].as<float>();
        }
        if (sensorObj.containsKey("windSpeedCorrection"))
        {
            sensor.windSpeedCorrection = sensorObj["windSpeedCorrection"].as<float>();
        }
        if (sensorObj.containsKey("windDirectionCorrection"))
        {
            sensor.windDirectionCorrection = sensorObj["windDirectionCorrection"].as<int>();
        }
        if (sensorObj.containsKey("rainAmountCorrection"))
        {
            sensor.rainAmountCorrection = sensorObj["rainAmountCorrection"].as<float>();
        }
        if (sensorObj.containsKey("rainRateCorrection"))
        {
            sensor.rainRateCorrection = sensorObj["rainRateCorrection"].as<float>();
        }
        loadedSensors.push_back(sensor);
    }

    for (size_t i = 0; i < MAX_SENSORS; ++i)
    {
        sensors[i] = SensorData();
        pendingAdds[i] = false;
    }
    sensorCount = loadedSensors.size();
    for (size_t i = 0; i < sensorCount; ++i)
    {
        sensors[i] = loadedSensors[i];
    }

    storageLoadedSuccessfully = true;
    logger.info("Loaded " + String(sensorCount) + " sensors from configuration");
    return true;
}

// ---------------------------------------------------------------------------
// Per-sensor health bookkeeping. Out-of-range indices and unconfigured slots
// are silently ignored — caller doesn't need to know whether a sensor was
// recently deleted between packet receipt and bookkeeping.
// ---------------------------------------------------------------------------

void SensorManager::recordSensorSuccess(int index, unsigned long nowMillis)
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    if (index < 0 || static_cast<size_t>(index) >= sensorCount || !sensors[index].configured)
    {
        return;
    }
    SensorHealth::recordSuccess(sensors[index].health, nowMillis);
}

void SensorManager::recordSensorRejection(int index, unsigned long nowMillis, const char *reason)
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    if (index < 0 || static_cast<size_t>(index) >= sensorCount || !sensors[index].configured)
    {
        return;
    }
    SensorHealth::recordRejection(sensors[index].health, nowMillis, reason);
}

void SensorManager::tickSensorHealth(unsigned long nowMillis)
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    for (size_t i = 0; i < sensorCount; i++)
    {
        if (sensors[i].configured)
        {
            SensorHealth::advanceBucketsIfNeeded(sensors[i].health, nowMillis);
            SensorHealth::latchStale(sensors[i].health, nowMillis);
        }
    }
}
