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

TEST_CASE("the check-in names the board")
{
    uplink::CheckinRequest request;
    request.hub_firmware = "v1.2.9";
    request.hub_board = "esp32s3";
    request.hub_variant = "release";
    CHECK(uplink::build_checkin_json(request) ==
          "{\"hubFirmware\":\"v1.2.9\",\"hubBoard\":\"esp32s3\","
          "\"hubVariant\":\"release\",\"cameras\":[]}");
}

namespace
{

const std::string kSha(64, 'a');

std::string manifest_with(const std::string &image)
{
    return "{\"version\": \"v1.3.0\",\n \"images\": {\n"
           "  \"esp32p4-release\": {\"url\": \"https://x/p4.bin\", \"sha256\": "
           "\"" +
           kSha + "\", \"size\": 1},\n  \"esp32s3-release\": " + image +
           "\n }\n}\n";
}

Conditions good_conditions()
{
    Conditions conditions;
    conditions.battery_percent = 80;
    return conditions;
}

}  // namespace

TEST_CASE("the release manifest gives the image of this board")
{
    // As written by the CI (jq output, with spaces and line breaks).
    const std::string url =
        "https://github.com/ThomasPohl/TrailCamHub/releases/download/v1.3.0/"
        "trailcamhub-esp32s3-release.app.bin";
    Offer offer;
    REQUIRE(parse_manifest(
        manifest_with("{\"url\": \"" + url + "\", \"sha256\": \"" +
                      std::string(64, 'B') + "\", \"size\": 1572864}"),
        "esp32s3-release", offer));
    CHECK(offer.version == "v1.3.0");
    CHECK(offer.url == url);
    CHECK(offer.sha256 == std::string(64, 'b'));
    CHECK(offer.size == 1572864);

    REQUIRE(parse_manifest(manifest_with("{}"), "esp32p4-release", offer));
    CHECK(offer.url == "https://x/p4.bin");

    // Escaped slashes are read.
    REQUIRE(parse_manifest("{\"version\":\"v2.0.0\",\"images\":{\"a\":{"
                           "\"url\":\"https:\\/\\/x\\/a.bin\",\"sha256\":\"" +
                               kSha + "\",\"size\":5}}}",
                           "a", offer));
    CHECK(offer.url == "https://x/a.bin");
}

TEST_CASE("incomplete or unsafe images are not offered")
{
    Offer offer;
    offer.version = "untouched";
    for (const std::string &image :
         {std::string("{\"url\": \"http://x/\", \"sha256\": \"") + kSha +
              "\", \"size\": 1}",
          std::string("{\"url\": \"https://x/\", \"sha256\": \"abc\", "
                      "\"size\": 1}"),
          std::string("{\"url\": \"https://x/\", \"sha256\": \"") + kSha +
              "\"}",
          std::string("{\"url\": \"https://x/\", \"sha256\": \"") + kSha +
              "\", \"size\": 0}",
          std::string("{\"url\": \"https://x/\", \"sha256\": \"") + kSha +
              "\", \"size\": -5}",
          std::string("\"not an object\"")}) {
        CAPTURE(image);
        CHECK_FALSE(
            parse_manifest(manifest_with(image), "esp32s3-release", offer));
    }
    // No image for this board, or no manifest at all.
    CHECK_FALSE(
        parse_manifest(manifest_with("{}"), "tdongle-c5-release", offer));
    // A key of the wrong object does not count.
    CHECK_FALSE(parse_manifest(manifest_with("{}"), "url", offer));
    CHECK_FALSE(
        parse_manifest("<html>Not Found</html>", "esp32s3-release", offer));
    CHECK_FALSE(parse_manifest("{\"version\":\"v1.0.0\",\"images\":{",
                               "esp32s3-release", offer));
    CHECK(offer.version == "untouched");
}

TEST_CASE("the battery decides whether an update may run")
{
    const Policy policy;
    Conditions c = good_conditions();
    CHECK(ready(true, "v1.0.0", c, policy) == Decision::install);
    CHECK(ready(false, "v1.0.0", c, policy) == Decision::disabled);
    CHECK(ready(true, "358b4b9", c, policy) == Decision::dev_build);

    c.battery_percent = 39;
    CHECK(ready(true, "v1.0.0", c, policy) == Decision::battery_low);
    c.battery_percent = 40;
    CHECK(ready(true, "v1.0.0", c, policy) == Decision::install);
    // Below 40 % while charging: allowed.
    c.battery_percent = 36;
    c.charging = true;
    CHECK(ready(true, "v1.0.0", c, policy) == Decision::install);
    // Never in the low or critical level, also not while charging.
    c.power_level = 1;
    CHECK(ready(true, "v1.0.0", c, policy) == Decision::battery_low);
    c.power_level = 2;
    c.battery_percent = 50;
    CHECK(ready(true, "v1.0.0", c, policy) == Decision::battery_critical);
    // Unknown battery (no gauge): allowed.
    c = Conditions{};
    CHECK(ready(true, "v1.0.0", c, policy) == Decision::install);
}

TEST_CASE("over the modem the signal has to be usable")
{
    const Policy policy;
    Conditions c = good_conditions();
    c.over_modem = true;
    c.signal_dbm = -107;
    CHECK(ready(true, "v1.0.0", c, policy) == Decision::weak_signal);
    c.signal_dbm = -105;
    CHECK(ready(true, "v1.0.0", c, policy) == Decision::install);
    // Unknown signal: allowed.
    c.signal_dbm = 0;
    CHECK(ready(true, "v1.0.0", c, policy) == Decision::install);
    // Over Wi-Fi the modem's signal does not count.
    c.over_modem = false;
    c.signal_dbm = -113;
    CHECK(ready(true, "v1.0.0", c, policy) == Decision::install);
}

TEST_CASE("the manifest is read at most once a day")
{
    const Policy policy;
    const std::int64_t day = 24 * 3600;
    CHECK(manifest_due(0, 1000, policy));
    CHECK_FALSE(manifest_due(1000, 1000 + day - 1, policy));
    CHECK(manifest_due(1000, 1000 + day, policy));
    // Clock not set or gone back.
    CHECK(manifest_due(1000, 0, policy));
    CHECK(manifest_due(5000, 1000, policy));
}

TEST_CASE("a version that failed too often waits a day")
{
    const Policy policy;
    const Conditions c = good_conditions();
    const std::int64_t t = 1'800'000'000;
    Attempts attempts;
    auto decide_at = [&](const char *offered, std::int64_t now) {
        return decide(offered, "v1.2.9", "", true, c, policy, attempts, now);
    };

    CHECK(decide_at("v1.3.0", t) == Decision::install);
    record_failure(attempts, "v1.3.0", t);
    record_failure(attempts, "v1.3.0", t + 100);
    CHECK(decide_at("v1.3.0", t + 200) == Decision::install);
    record_failure(attempts, "v1.3.0", t + 300);
    CHECK(attempts.failures == 3);
    CHECK(decide_at("v1.3.0", t + 400) == Decision::gave_up);
    CHECK(decide_at("v1.3.0", t + 300 + 24 * 3600 - 1) == Decision::gave_up);
    // Without a clock it keeps waiting.
    CHECK(decide_at("v1.3.0", 0) == Decision::gave_up);
    // A day later one more attempt; a failure waits another day.
    CHECK(decide_at("v1.3.0", t + 300 + 24 * 3600) == Decision::install);
    record_failure(attempts, "v1.3.0", t + 300 + 24 * 3600);
    CHECK(decide_at("v1.3.0", t + 300 + 24 * 3600 + 10) == Decision::gave_up);

    // A newer release starts afresh.
    CHECK(decide_at("v1.3.1", t + 400) == Decision::install);
    record_failure(attempts, "v1.3.1", t + 500);
    CHECK(attempts.version == "v1.3.1");
    CHECK(attempts.failures == 1);

    // The version checks still apply, and the battery comes first.
    CHECK(decide_at("v1.2.9", t) == Decision::not_newer);
    Conditions low = c;
    low.power_level = 1;
    CHECK(decide("v1.4.0", "v1.2.9", "", true, low, policy, attempts, t) ==
          Decision::battery_low);
    for (Decision d : {Decision::battery_critical, Decision::battery_low,
                       Decision::weak_signal, Decision::gave_up}) {
        CHECK(std::string(decision_text(d)).size() > 0);
    }
}
