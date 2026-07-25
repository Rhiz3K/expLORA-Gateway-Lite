/**
 * expLORA Gateway Lite
 *
 * Capacity policy for the persisted sensor JSON document.
 */

#pragma once

#include <ArduinoJson.h>
#include <stddef.h>

namespace SensorStoragePolicy
{

// METEO entries use all 17 persisted fields. Other sensor types use 15,
// but storage must be sized for the largest supported object.
constexpr size_t MAX_MEMBERS_PER_SENSOR = 17;
constexpr size_t CAPACITY_HEADROOM = 512;

inline size_t documentCapacity(size_t sensorCount, size_t copiedStringBytes)
{
    return JSON_OBJECT_SIZE(1) +
           JSON_ARRAY_SIZE(sensorCount) +
           sensorCount * JSON_OBJECT_SIZE(MAX_MEMBERS_PER_SENSOR) +
           copiedStringBytes +
           CAPACITY_HEADROOM;
}

// Deserialization copies object keys and string values. The serialized file
// length is a conservative upper bound for all copied text.
inline size_t loadDocumentCapacity(size_t serializedBytes, size_t maxSensors)
{
    return documentCapacity(maxSensors, serializedBytes);
}

} // namespace SensorStoragePolicy
