#include <doctest/doctest.h>

#include "found_cameras.hpp"

using camera::found::Entry;

namespace
{
constexpr std::int64_t kNow = 1800000000;
}

TEST_CASE("found cameras: one entry per address, newest time, name kept")
{
    const std::vector<Entry> stored = {
        {"AA:BB:CC:00:00:01", "TC100_0001", -80, kNow - 600},
        {"F4:C1:56:06:ED:3B", "", -60, kNow - 300},
    };
    const std::vector<Entry> heard = {
        {"AA:BB:CC:00:00:01", "", -70, kNow},
        {"F4:C1:56:06:ED:3B", "TC100_ED3B", -50, kNow - 10},
        {"not an address", "x", -1, kNow},
    };
    const auto merged = camera::found::merge(stored, heard, kNow);
    REQUIRE(merged.size() == 2);
    CHECK(merged[0].address == "AA:BB:CC:00:00:01");
    CHECK(merged[0].name == "TC100_0001");
    CHECK(merged[0].rssi == -70);
    CHECK(merged[0].seen_at == kNow);
    CHECK(merged[1].name == "TC100_ED3B");
    CHECK(merged[1].seen_at == kNow - 10);
}

TEST_CASE("found cameras: old entries go, the newest ones stay")
{
    std::vector<Entry> heard;
    for (int i = 0; i < 40; ++i) {
        char address[18];
        snprintf(address, sizeof(address), "AA:BB:CC:00:00:%02X", i);
        heard.push_back({address, "", -60, kNow - i});
    }
    heard.push_back(
        {"AA:BB:CC:00:01:00", "", -60, kNow - camera::found::kKeepS - 1});
    const auto merged = camera::found::merge({}, heard, kNow);
    CHECK(merged.size() == camera::found::kMaxEntries);
    CHECK(merged.front().seen_at == kNow);
    CHECK(merged.back().seen_at ==
          kNow - static_cast<std::int64_t>(camera::found::kMaxEntries) + 1);
    // Without a set clock nothing is dropped for its age.
    const auto unset =
        camera::found::merge({}, {{"AA:BB:CC:00:01:00", "", -60, 5}}, 0);
    CHECK(unset.size() == 1);
}

TEST_CASE("found cameras: the file round-trips and skips broken lines")
{
    const std::vector<Entry> entries = {
        {"AA:BB:CC:00:00:01", "TC100_0001", -70, kNow},
        {"F4:C1:56:06:ED:3B", "a,b\nc", -55, 0},
    };
    const std::string text = camera::found::serialize(entries);
    CHECK(text == "AA:BB:CC:00:00:01,-70,1800000000,TC100_0001\n"
                  "F4:C1:56:06:ED:3B,-55,0,abc\n");
    const auto parsed = camera::found::parse(text + "garbage\nzz,1,2,3\n");
    REQUIRE(parsed.size() == 2);
    CHECK(parsed[1].address == "F4:C1:56:06:ED:3B");
    CHECK(parsed[1].rssi == -55);
    CHECK(parsed[1].name == "abc");
}
