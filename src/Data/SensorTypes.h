/**
 * expLORA Gateway Lite
 *
 * Sensor types definitions header file
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
#include "../config.h"

/**
 * Definition of sensor types and their properties
 *
 * This file contains definitions for all supported sensor types including their
 * specific properties and capabilities. When adding a new sensor type, simply
 * define its properties here and add implementation for its processing.
 */

// Enumeration for sensor types - matching the definitions in config.h
enum class SensorType : uint8_t
{
    UNKNOWN = SENSOR_TYPE_UNKNOWN,
    BME280 = SENSOR_TYPE_BME280,       // Temperature, humidity, pressure
    SCD40 = SENSOR_TYPE_SCD40,         // Temperature, humidity, CO2
    VEML7700 = SENSOR_TYPE_VEML7700,   // Light sensor (LUX)
    METEO = SENSOR_TYPE_METEO,         // Meteorological station
    DIY_TEMP = SENSOR_TYPE_DIY_TEMP,   // Temperature sensor (DS18B20)
    // Add more sensor types here...
};

// Structure for sensor type information
struct SensorTypeInfo
{
    SensorType type;            // Sensor type (enum)
    const char *name;           // Sensor type name
    uint8_t expectedDataLength; // Expected data length in packet (excluding header and checksum)
    uint8_t packetDataOffset;   // Offset where sensor data begins in the packet
    bool hasTemperature;        // Whether sensor provides temperature
    bool hasHumidity;           // Whether sensor provides humidity
    bool hasPressure;           // Whether sensor provides pressure
    bool hasPPM;                // Whether sensor provides PPM (CO2)
    bool hasLux;                // Whether sensor provides illumination (lux)
    bool hasWindSpeed;          // Whether sensor provides wind speed
    bool hasWindDirection;      // Whether sensor provides wind direction
    bool hasRainAmount;         // Whether sensor provides rain amount
    bool hasRainRate;           // Whether sensor provides rain intensity
    // Possibility to add more parameters as needed
};

// Table of sensor type definitions - for easy lookup and adding new types
const SensorTypeInfo SENSOR_TYPE_DEFINITIONS[] = {
    // type,                 name,       dataLen, offset, temp,  hum,   press, ppm,   lux,   windS, windD, rain,  rainR
    {SensorType::UNKNOWN, "Unknown",  0, 7, false, false, false, false, false, false, false, false, false},
    {SensorType::BME280,    "CLIMA",  6, 7, true,  true,  true,  false, false, false, false, false, false},
    {SensorType::SCD40,    "CARBON",  6, 7, true,  true,  false,  true, false, false, false, false, false},
    {SensorType::VEML7700, "VEML7700", 4, 7, false, false, false, false, true, false, false, false, false},
    {SensorType::METEO,     "METEO", 14, 7, true,  true,  true,  false, false,  true,  true,  true,  true},
    {SensorType::DIY_TEMP, "DIY_TEMP",     2,     7,    true,  false, false, false, false, false, false, false, false},

    // Add more sensor types here in the future...
};

// Helper function to get information about sensor type by enum value
inline const SensorTypeInfo &getSensorTypeInfo(SensorType type)
{
    for (const auto &info : SENSOR_TYPE_DEFINITIONS)
    {
        if (info.type == type)
        {
            return info;
        }
    }
    return SENSOR_TYPE_DEFINITIONS[0]; // Returns UNKNOWN if type is not found
}

// Helper function to get information about sensor type by uint8_t value
inline const SensorTypeInfo &getSensorTypeInfo(uint8_t typeValue)
{
    return getSensorTypeInfo(static_cast<SensorType>(typeValue));
}

// Helper function to get enum SensorType from uint8_t value
inline SensorType sensorTypeFromValue(uint8_t value)
{
    return static_cast<SensorType>(value);
}

inline bool isSupportedSensorType(uint8_t value)
{
    const SensorTypeInfo &info = getSensorTypeInfo(value);
    return info.type != SensorType::UNKNOWN &&
           static_cast<uint8_t>(info.type) == value;
}

// Convert SensorType to string for debugging and logging
inline String sensorTypeToString(SensorType type)
{
    const SensorTypeInfo &info = getSensorTypeInfo(type);
    return String(info.name);
}
