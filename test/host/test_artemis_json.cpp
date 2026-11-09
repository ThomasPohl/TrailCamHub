#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "artemis_json.hpp"

using namespace camera::artemis;

TEST_CASE("json_is_secret_key recognises the screen code and passwords")
{
    CHECK(json_is_secret_key("secureCode0"));
    CHECK(json_is_secret_key("secureCode3"));
    CHECK(json_is_secret_key("password"));
    CHECK(json_is_secret_key("newPassword"));
    CHECK(json_is_secret_key("pwd"));
    CHECK(json_is_secret_key("oldPwd"));
    CHECK_FALSE(json_is_secret_key("secureCodeOnOff"));
    CHECK_FALSE(json_is_secret_key("batPercent"));
    CHECK_FALSE(json_is_secret_key("usrName"));
    CHECK_FALSE(json_is_secret_key("token"));
}

TEST_CASE("json_redact hides the screen code of a device info answer")
{
    // Shape of a 512 answer from a TC100 on firmware 2.2.5.9 (shortened).
    const std::string json =
        "{\"devId\":\"F4:C1:56:06:ED:3B\",\"batPercent\":87,"
        "\"secureCodeOnOff\":1,\"secureCode0\":4,\"secureCode1\":7,"
        "\"secureCode2\":1,\"secureCode3\":9,\"result\":0,\"cmdId\":512}";
    const std::string redacted = json_redact(json);
    CHECK(redacted == "{\"devId\":\"F4:C1:56:06:ED:3B\",\"batPercent\":87,"
                      "\"secureCodeOnOff\":1,\"secureCode0\":\"***\","
                      "\"secureCode1\":\"***\",\"secureCode2\":\"***\","
                      "\"secureCode3\":\"***\",\"result\":0,\"cmdId\":512}");
    std::int64_t value = 0;
    CHECK(json_find_int(redacted, "batPercent", value));
    CHECK(value == 87);
    CHECK_FALSE(json_find_int(redacted, "secureCode0", value));
}

TEST_CASE("json_redact replaces strings, objects and arrays at any depth")
{
    CHECK(json_redact("{\"a\":{\"pwd\":\"x\\\"y\",\"b\":2}}") ==
          "{\"a\":{\"pwd\":\"***\",\"b\":2}}");
    CHECK(json_redact("{\"password\" : {\"k\":[1,\"}\"]}, \"n\":1}") ==
          "{\"password\" : \"***\", \"n\":1}");
    CHECK(json_redact("{\"list\":[{\"secureCode1\":-5}]}") ==
          "{\"list\":[{\"secureCode1\":\"***\"}]}");
}

TEST_CASE("json_redact leaves values that only look like secret keys")
{
    const std::string json = "{\"name\":\"password\",\"note\":\"pwd: 1\"}";
    CHECK(json_redact(json) == json);
    CHECK(json_redact("") == "");
    CHECK(json_redact("not json") == "not json");
}

TEST_CASE("json_redact does not leak a truncated secret")
{
    CHECK(json_redact("{\"pwd\":\"abc") == "{\"pwd\":\"***\"");
    CHECK(json_redact("{\"pwd\":") == "{\"pwd\":");
}
