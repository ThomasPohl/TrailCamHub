#include <doctest/doctest.h>

#include <string>

#include "artemis_json.hpp"
#include "device_info.hpp"

using namespace camera::artemis;

TEST_CASE("parse_device_info reads identity and state from a 512 response")
{
    const std::string json =
        "{\"devId\":\"TC100-ED3B\",\"fwVerName\":\"1.4.2\","
        "\"modelName\":\"TC100\",\"subModelName\":\"A\",\"productId\":7,"
        "\"batPercent\":83,\"sdTotalMB\":30436,\"sdFreeMB\":28110,"
        "\"capability\":{\"video\":1},\"result\":0,\"cmdId\":512}";
    DeviceInfo info;
    REQUIRE(parse_device_info(json, info));
    CHECK(info.dev_id == "TC100-ED3B");
    CHECK(info.model_name == "TC100");
    CHECK(info.fw_version == "1.4.2");
    CHECK(info.bat_percent == 83);
    CHECK(info.sd_total_mb == 30436);
    CHECK(info.sd_free_mb == 28110);
}

TEST_CASE(
    "parse_device_info accepts a numeric devId and defaults missing fields")
{
    const std::string json = "{\"devId\":12345,\"result\":0,\"cmdId\":512}";
    DeviceInfo info;
    REQUIRE(parse_device_info(json, info));
    CHECK(info.dev_id == "12345");
    CHECK(info.bat_percent == -1);  // unknown
    CHECK(info.model_name.empty());
}

TEST_CASE("parse_device_info fails without a device identity")
{
    DeviceInfo info;
    CHECK_FALSE(parse_device_info("{\"batPercent\":50}", info));
}

TEST_CASE("json_find_string handles quotes, escapes and scoping")
{
    std::string s;
    REQUIRE(json_find_string("{\"a\":\"hello\",\"b\":\"x\"}", "a", s));
    CHECK(s == "hello");

    REQUIRE(
        json_find_string("{\"path\":\"C:\\\\dir\\/f\\\"q\\\"\"}", "path", s));
    CHECK(s == "C:\\dir/f\"q\"");

    // A later key is found when scoping past the first object.
    std::string json = "{\"name\":\"first\"}{\"name\":\"second\"}";
    std::size_t from = json.find('}');
    REQUIRE(json_find_string(json, "name", s, from));
    CHECK(s == "second");

    CHECK_FALSE(json_find_string("{\"n\":123}", "n", s));      // not a string
    CHECK_FALSE(json_find_string("{\"n\":\"abc\"}", "x", s));  // absent
}

TEST_CASE("serialize_device_info round-trips through parse_device_info")
{
    camera::artemis::DeviceInfo info;
    info.dev_id = "F4:C1:56:06:ED:3B";
    info.model_name = "Wi-Fi \"Trail\" Cam";
    info.fw_version = "2.3.0.3";
    info.bat_percent = 78;
    info.sd_free_mb = 28412;

    camera::artemis::DeviceInfo parsed;
    REQUIRE(camera::artemis::parse_device_info(
        camera::artemis::serialize_device_info(info), parsed));
    CHECK(parsed.dev_id == info.dev_id);
    CHECK(parsed.model_name == info.model_name);
    CHECK(parsed.fw_version == "2.3.0.3");
    CHECK(parsed.bat_percent == 78);
    CHECK(parsed.sd_total_mb == -1);
    CHECK(parsed.sd_free_mb == 28412);
}

TEST_CASE("the stored device info keeps the state of the SD card")
{
    DeviceInfo info;
    info.dev_id = "F4C15606ED3B";
    info.sd_free_mb = 0;
    info.sd_card = SdCard::missing;
    info.sd_error = "Memory card \"not\" exist";
    const std::string json = serialize_device_info(info);
    CHECK(json == "{\"devId\":\"F4C15606ED3B\",\"modelName\":\"\","
                  "\"fwVerName\":\"\",\"sdFreeMB\":0,\"sdCard\":\"missing\","
                  "\"sdError\":\"Memory card \\\"not\\\" exist\"}");
    DeviceInfo back;
    REQUIRE(parse_device_info(json, back));
    CHECK(back.sd_card == SdCard::missing);
    CHECK(back.sd_error == "Memory card \"not\" exist");

    // A camera's own response has no SD card state.
    REQUIRE(parse_device_info("{\"devId\":\"X\",\"sdFreeMB\":0}", back));
    CHECK(back.sd_card == SdCard::unknown);
    CHECK(serialize_device_info(back).find("sdCard") == std::string::npos);
}

TEST_CASE("sd_card_name and parse_sd_card are inverse")
{
    for (SdCard card : {SdCard::unknown, SdCard::present, SdCard::missing}) {
        CHECK(parse_sd_card(sd_card_name(card)) == card);
    }
    CHECK(parse_sd_card("maybe") == SdCard::unknown);
}

TEST_CASE("json_set_string replaces string and number values or appends")
{
    const std::string json = "{\"a\":\"x\\\"y\",\"n\":12,\"b\":1}";
    CHECK(json_set_string(json, "a", "new") ==
          "{\"a\":\"new\",\"n\":12,\"b\":1}");
    CHECK(json_set_string(json, "n", "s") ==
          "{\"a\":\"x\\\"y\",\"n\":\"s\",\"b\":1}");
    CHECK(json_set_string(json, "c", "a\\b") ==
          "{\"a\":\"x\\\"y\",\"n\":12,\"b\":1,\"c\":\"a\\\\b\"}");
    CHECK(json_set_string("{}", "c", "") == "{\"c\":\"\"}");
    // json_set_int now also replaces a string value.
    CHECK(json_set_int("{\"a\":\"x\",\"b\":1}", "a", 5) == "{\"a\":5,\"b\":1}");
}

TEST_CASE("parse_device_info reads the allowlisted status fields only")
{
    // Shape of a real 512 answer (PH970S firmware 2.3.6.5), shortened.
    const std::string json =
        "{\"devId\":\"C6:1E:0D:00:00:01\",\"fwVerName\":\"2.3.6.5\","
        "\"bleVerName\":\"3.9.4\",\"bleVerCode\":394,\"modelName\":\"PH970S\","
        "\"cameraNameOnOff\":0,\"cameraName\":\"0000\",\"batPercent\":100,"
        "\"emergencyBatPercent\":200,\"batType\":2,\"curBatType\":2,"
        "\"isCharging\":false,\"isSolarCharging\":true,\"temperature\":-4,"
        "\"secureCodeOnOff\":1,\"secureCode0\":1,\"secureCode1\":2,"
        "\"sdTotalMB\":29998,\"sdFreeMB\":20617,"
        "\"capability\":{\"platform\":\"T23ZN\"},\"result\":0,\"cmdId\":512}";
    DeviceInfo info;
    REQUIRE(parse_device_info(json, info));
    CHECK(info.ble_version == "3.9.4");
    CHECK(info.camera_name == "0000");
    CHECK(info.bat_type == 2);
    CHECK(info.charging == false);
    CHECK(info.solar_charging == true);
    CHECK(info.temperature_c == -4);

    // Only allowlisted fields are written back: no secrets, nothing unknown.
    const std::string stored = serialize_device_info(info);
    CHECK(stored.find("secureCode") == std::string::npos);
    CHECK(stored.find("curBatType") == std::string::npos);
    CHECK(stored.find("capability") == std::string::npos);
    CHECK(stored.find("emergencyBatPercent") == std::string::npos);

    DeviceInfo parsed;
    REQUIRE(parse_device_info(stored, parsed));
    CHECK(parsed.ble_version == "3.9.4");
    CHECK(parsed.camera_name == "0000");
    CHECK(parsed.bat_type == 2);
    CHECK(parsed.charging == false);
    CHECK(parsed.solar_charging == true);
    CHECK(parsed.temperature_c == -4);
}

TEST_CASE("status fields stay unknown when the camera does not report them")
{
    DeviceInfo info;
    REQUIRE(parse_device_info("{\"devId\":\"x\",\"isCharging\":1}", info));
    CHECK_FALSE(info.charging.has_value());  // not a JSON boolean
    CHECK_FALSE(info.temperature_c.has_value());
    CHECK_FALSE(info.bat_type.has_value());
    const std::string stored = serialize_device_info(info);
    CHECK(stored.find("isCharging") == std::string::npos);
    CHECK(stored.find("temperature") == std::string::npos);
}

TEST_CASE("device_state_summary lists the known state")
{
    DeviceInfo info;
    CHECK(device_state_summary(info).empty());
    info.bat_percent = 78;
    CHECK(device_state_summary(info) == "battery 78 %");
    info.charging = true;
    info.temperature_c = 22;
    CHECK(device_state_summary(info) == "battery 78 %, charging, 22 °C");
    info.charging = false;
    info.solar_charging = true;
    CHECK(device_state_summary(info) ==
          "battery 78 %, charging (solar), 22 °C");
}

TEST_CASE("json_find_bool reads true and false only")
{
    bool b = true;
    REQUIRE(json_find_bool("{\"a\":false,\"b\": true}", "a", b));
    CHECK_FALSE(b);
    REQUIRE(json_find_bool("{\"a\":false,\"b\": true}", "b", b));
    CHECK(b);
    CHECK_FALSE(json_find_bool("{\"a\":\"true\"}", "a", b));
    CHECK_FALSE(json_find_bool("{\"a\":0}", "a", b));
    CHECK_FALSE(json_find_bool("{\"x\":true}", "a", b));
}
