#include <ArduinoJson.h>
#include <unity.h>

#include <cstdio>
#include <string>

#include "../../src/Data/SensorStoragePolicy.h"

namespace
{

constexpr size_t SENSOR_COUNT = 20;

void addSensor(JsonArray array, size_t index, const std::string &name,
               const std::string &customUrl)
{
    JsonObject sensor = array.createNestedObject();
    sensor["deviceType"] = 3;
    sensor["serialNumber"] = static_cast<uint32_t>(0x100000 + index);
    sensor["deviceKey"] = static_cast<uint32_t>(0xA0000000 + index);
    sensor["name"] = name;
    sensor["customUrl"] = customUrl;
    sensor["altitude"] = 250;
    sensor["dailyRainTotal"] = 12.5;
    sensor["lastRainReset"] = 123456;
    sensor["temperatureCorrection"] = 0.1;
    sensor["humidityCorrection"] = 0.2;
    sensor["pressureCorrection"] = 0.3;
    sensor["ppmCorrection"] = 0.4;
    sensor["luxCorrection"] = 0.5;
    sensor["windSpeedCorrection"] = 1.1;
    sensor["windDirectionCorrection"] = -10;
    sensor["rainAmountCorrection"] = 1.2;
    sensor["rainRateCorrection"] = 1.3;
}

void test_capacity_round_trips_twenty_large_sensors()
{
    const std::string name(96, 'N');
    const std::string customUrl =
        "https://collector.example.test/measurements/" + std::string(240, 'x') +
        "?temperature=*TEMP*&humidity=*HUM*&serial=*SN*";
    const size_t copiedStrings =
        SENSOR_COUNT * (name.size() + 1 + customUrl.size() + 1);

    DynamicJsonDocument saveDoc(
        SensorStoragePolicy::documentCapacity(SENSOR_COUNT, copiedStrings));
    JsonArray sensors = saveDoc.createNestedArray("sensors");
    for (size_t i = 0; i < SENSOR_COUNT; ++i)
    {
        addSensor(sensors, i, name, customUrl);
    }

    TEST_ASSERT_FALSE(saveDoc.overflowed());
    TEST_ASSERT_EQUAL_UINT32(SENSOR_COUNT, sensors.size());

    std::string serialized;
    const size_t written = serializeJson(saveDoc, serialized);
    TEST_ASSERT_GREATER_THAN_UINT32(4096, written);

    DynamicJsonDocument loadDoc(
        SensorStoragePolicy::loadDocumentCapacity(serialized.size(), SENSOR_COUNT));
    const DeserializationError error = deserializeJson(loadDoc, serialized);

    TEST_ASSERT_TRUE(error == DeserializationError::Ok);
    TEST_ASSERT_FALSE(loadDoc.overflowed());
    TEST_ASSERT_EQUAL_UINT32(
        SENSOR_COUNT, loadDoc["sensors"].as<JsonArrayConst>().size());
    TEST_ASSERT_EQUAL_STRING(
        customUrl.c_str(),
        loadDoc["sensors"][SENSOR_COUNT - 1]["customUrl"].as<const char *>());
}

void test_legacy_4096_capacity_overflows_the_same_shape()
{
    const std::string name(96, 'N');
    const std::string customUrl(320, 'U');
    DynamicJsonDocument legacyDoc(4096);
    JsonArray sensors = legacyDoc.createNestedArray("sensors");

    for (size_t i = 0; i < SENSOR_COUNT; ++i)
    {
        addSensor(sensors, i, name, customUrl);
    }

    TEST_ASSERT_TRUE(legacyDoc.overflowed());
    TEST_ASSERT_LESS_THAN_UINT32(SENSOR_COUNT, sensors.size());
}

} // namespace

void setUp() {}
void tearDown() {}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_capacity_round_trips_twenty_large_sensors);
    RUN_TEST(test_legacy_4096_capacity_overflows_the_same_shape);
    return UNITY_END();
}
