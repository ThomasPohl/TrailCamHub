#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>

#include "ble_advertising.hpp"

using namespace camera::ble;

namespace
{

// Advertisement of the TC100 camera, captured on hardware.
const std::uint8_t kTc100Advertisement[] = {
    0x02, 0x01, 0x06, 0x03, 0x03, 0x01, 0x88, 0x0b, 0xff, 0x87, 0x19,
    0xf4, 0xc1, 0x56, 0x06, 0xed, 0x3b, 0x01, 0x16, 0x0b, 0x09, 0x54,
    0x43, 0x31, 0x30, 0x30, 0x5f, 0x45, 0x44, 0x33, 0x42,
};

const char *const kAllowlist[] = {"TC100_ED3B"};

}  // namespace

TEST_CASE("finds the TC100 name and service UUID")
{
    const std::uint8_t *name = nullptr;
    std::size_t name_len = 0;
    REQUIRE(find_device_name(kTc100Advertisement, sizeof(kTc100Advertisement),
                             &name, &name_len));
    CHECK(name_len == 10);
    CHECK(std::memcmp(name, "TC100_ED3B", name_len) == 0);
    CHECK(name_in_allowlist(name, name_len, kAllowlist, 1));
    CHECK(contains_uuid16(kTc100Advertisement, sizeof(kTc100Advertisement),
                          0x8801));
    CHECK_FALSE(contains_uuid16(kTc100Advertisement,
                                sizeof(kTc100Advertisement), 0x8800));
}

TEST_CASE("finds the manufacturer data field")
{
    const std::uint8_t *value = nullptr;
    std::size_t value_len = 0;
    REQUIRE(find_ad_field(kTc100Advertisement, sizeof(kTc100Advertisement),
                          0xff, &value, &value_len));
    CHECK(value_len == 10);
    CHECK(value[0] == 0x87);
    CHECK(value[1] == 0x19);
}

TEST_CASE("falls back to the shortened name")
{
    const std::uint8_t data[] = {0x04, kAdTypeShortName, 'A', 'B', 'C'};
    const std::uint8_t *name = nullptr;
    std::size_t name_len = 0;
    REQUIRE(find_device_name(data, sizeof(data), &name, &name_len));
    CHECK(name_len == 3);
}

TEST_CASE("stops at truncated or empty fields")
{
    const std::uint8_t truncated[] = {0x05, kAdTypeCompleteName, 'A'};
    const std::uint8_t *value = nullptr;
    std::size_t value_len = 0;
    CHECK_FALSE(find_ad_field(truncated, sizeof(truncated), kAdTypeCompleteName,
                              &value, &value_len));
    const std::uint8_t padded[] = {0x00, 0x03, 0x03, 0x01, 0x88};
    CHECK_FALSE(contains_uuid16(padded, sizeof(padded), 0x8801));
}

TEST_CASE("allowlist requires an exact match")
{
    const std::uint8_t prefix[] = {'T', 'C', '1', '0', '0'};
    CHECK_FALSE(name_in_allowlist(prefix, sizeof(prefix), kAllowlist, 1));
    CHECK_FALSE(name_in_allowlist(nullptr, 0, kAllowlist, 1));
}
