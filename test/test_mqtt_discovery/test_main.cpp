#include <unity.h>

#include <string>
#include <vector>

#include "../../src/Protocol/MQTTDiscoveryPlan.h"

namespace
{

std::vector<std::string> valuesFor(
    const MQTTDiscoveryPlan::Capabilities &capabilities)
{
    std::vector<std::string> values;
    MQTTDiscoveryPlan::forEachValue(
        capabilities,
        [&](const char *value) { values.emplace_back(value); });
    return values;
}

void assertValues(const std::vector<std::string> &actual,
                  const std::vector<std::string> &expected)
{
    TEST_ASSERT_EQUAL_UINT32(expected.size(), actual.size());
    for (size_t index = 0; index < expected.size(); ++index)
    {
        TEST_ASSERT_EQUAL_STRING(expected[index].c_str(), actual[index].c_str());
    }
}

void test_clima_discovery_plan()
{
    MQTTDiscoveryPlan::Capabilities capabilities;
    capabilities.temperature = true;
    capabilities.humidity = true;
    capabilities.pressure = true;
    assertValues(
        valuesFor(capabilities),
        {"temperature", "humidity", "pressure", "battery", "rssi"});
}

void test_veml7700_discovery_plan()
{
    MQTTDiscoveryPlan::Capabilities capabilities;
    capabilities.illuminance = true;
    assertValues(
        valuesFor(capabilities),
        {"illuminance", "battery", "rssi"});
}

void test_meteo_discovery_plan()
{
    MQTTDiscoveryPlan::Capabilities capabilities;
    capabilities.temperature = true;
    capabilities.humidity = true;
    capabilities.pressure = true;
    capabilities.windSpeed = true;
    capabilities.windDirection = true;
    capabilities.rainAmount = true;
    capabilities.rainRate = true;
    assertValues(
        valuesFor(capabilities),
        {"temperature", "humidity", "pressure", "wind_speed",
         "wind_direction", "rain_amount", "daily_rain", "rain_rate",
         "battery", "rssi"});
}

} // namespace

void setUp() {}
void tearDown() {}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_clima_discovery_plan);
    RUN_TEST(test_veml7700_discovery_plan);
    RUN_TEST(test_meteo_discovery_plan);
    return UNITY_END();
}
