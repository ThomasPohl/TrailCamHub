#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "ota_policy.hpp"
#include "uplink_protocol.hpp"

using namespace ota;

TEST_CASE("release versions")
{
    Version v;
    REQUIRE(parse_version("v1.2.3", v));
    CHECK(v.major == 1);
    CHECK(v.minor == 2);
    CHECK(v.patch == 3);
    CHECK(parse_version("10.0.12", v));
    CHECK(v.major == 10);
    CHECK(v.patch == 12);
    CHECK(parse_version("v0.0.0", v));

    // Development builds and anything else.
    for (const char *text :
         {"", "v", "v1", "v1.2", "v1.2.3.4", "1.2.3-rc1", "v1.2.3-4-g358b4b9",
          "v1.2.3-dirty", "358b4b9", "V1.2.3", "v01.2.3", "v1..3",
          "v4294967296.0.0", "v1.2.3 "}) {
        CAPTURE(text);
        CHECK_FALSE(parse_version(text, v));
    }
}

TEST_CASE("versions compare by major, minor, patch")
{
    Version a;
    Version b;
    REQUIRE(parse_version("v1.2.3", a));
    REQUIRE(parse_version("v1.10.0", b));
    CHECK(compare(a, b) < 0);
    CHECK(compare(b, a) > 0);
    CHECK(compare(a, a) == 0);
    REQUIRE(parse_version("v2.0.0", b));
    CHECK(compare(a, b) < 0);
    REQUIRE(parse_version("v1.2.4", b));
    CHECK(compare(a, b) < 0);
}

TEST_CASE("which offers are installed")
{
    CHECK(decide("v1.3.0", "v1.2.9", "", true) == Decision::install);
    CHECK(decide("v1.3.0", "v1.2.9", "", false) == Decision::disabled);
    CHECK(decide("v1.3.0", "358b4b9", "", true) == Decision::dev_build);
    CHECK(decide("v1.3.0", "v1.2.9-dirty", "", true) == Decision::dev_build);
    CHECK(decide("latest", "v1.2.9", "", true) == Decision::invalid_offer);
    CHECK(decide("v1.2.9", "v1.2.9", "", true) == Decision::not_newer);
    CHECK(decide("v1.2.0", "v1.2.9", "", true) == Decision::not_newer);
    // A version that was rolled back is not installed again, a later one is.
    CHECK(decide("v1.3.0", "v1.2.9", "v1.3.0", true) == Decision::rolled_back);
    CHECK(decide("v1.3.1", "v1.2.9", "v1.3.0", true) == Decision::install);
    CHECK(std::string(decision_text(Decision::dev_build)).find("development") !=
          std::string::npos);
}

TEST_CASE("SHA-256 digests as lowercase hex")
{
    const std::uint8_t digest[] = {0x00, 0x0f, 0xa5, 0xff};
    CHECK(to_hex(digest, sizeof(digest)) == "000fa5ff");
}

TEST_CASE("the check-in names the board and offers a firmware")
{
    uplink::CheckinRequest request;
    request.hub_firmware = "v1.2.9";
    request.hub_board = "esp32s3";
    request.hub_variant = "release";
    CHECK(uplink::build_checkin_json(request) ==
          "{\"hubFirmware\":\"v1.2.9\",\"hubBoard\":\"esp32s3\","
          "\"hubVariant\":\"release\",\"cameras\":[]}");

    const std::string sha(64, 'a');
    uplink::CheckinResponse response;
    REQUIRE(uplink::parse_checkin_response(
        "{\"serverTime\":1,\"cameras\":[],\"firmware\":{\"version\":\"v1.3.0\","
        "\"url\":\"https://github.com/ThomasPohl/TrailCamHub/releases/download/"
        "v1.3.0/trailcamhub-esp32s3-release.app.bin\",\"sha256\":\"" +
            sha + "\",\"size\":1572864}}",
        response));
    CHECK(response.firmware.present);
    CHECK(response.firmware.version == "v1.3.0");
    CHECK(response.firmware.sha256 == sha);
    CHECK(response.firmware.size == 1572864);

    // Incomplete or unsafe offers are dropped.
    for (const std::string &firmware :
         {std::string("{\"version\":\"v1.3.0\",\"url\":\"http://x/\","
                      "\"sha256\":\"") +
              sha + "\",\"size\":1}",
          std::string("{\"version\":\"v1.3.0\",\"url\":\"https://x/\","
                      "\"sha256\":\"ABC\",\"size\":1}"),
          std::string("{\"version\":\"v1.3.0\",\"url\":\"https://x/\","
                      "\"sha256\":\"") +
              sha + "\"}"}) {
        CAPTURE(firmware);
        REQUIRE(uplink::parse_checkin_response(
            "{\"serverTime\":1,\"cameras\":[],\"firmware\":" + firmware + "}",
            response));
        CHECK_FALSE(response.firmware.present);
        CHECK(response.firmware.version.empty());
    }
    REQUIRE(uplink::parse_checkin_response("{\"serverTime\":1}", response));
    CHECK_FALSE(response.firmware.present);
}
