/**
 * expLORA Gateway Lite
 *
 * Pure discovery-entity plan shared by MQTT code and native tests.
 */

#pragma once

namespace MQTTDiscoveryPlan
{

struct Capabilities
{
    bool temperature = false;
    bool humidity = false;
    bool pressure = false;
    bool co2 = false;
    bool illuminance = false;
    bool windSpeed = false;
    bool windDirection = false;
    bool rainAmount = false;
    bool rainRate = false;
};

template <typename Callback>
inline void forEachValue(const Capabilities &capabilities, Callback callback)
{
    if (capabilities.temperature) callback("temperature");
    if (capabilities.humidity) callback("humidity");
    if (capabilities.pressure) callback("pressure");
    if (capabilities.co2) callback("co2");
    if (capabilities.illuminance) callback("illuminance");
    if (capabilities.windSpeed) callback("wind_speed");
    if (capabilities.windDirection) callback("wind_direction");
    if (capabilities.rainAmount)
    {
        callback("rain_amount");
        callback("daily_rain");
    }
    if (capabilities.rainRate) callback("rain_rate");

    // Every sensor exposes radio diagnostics.
    callback("battery");
    callback("rssi");
}

} // namespace MQTTDiscoveryPlan
