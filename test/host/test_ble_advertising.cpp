#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>

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

TEST_CASE("camera names are recognised by their prefix")
{
    const char *const prefixes[] = {"TC100_"};
    const std::string a = "TC100_ED3B";
    const std::string b = "TC100_A1F0";
    const std::string bare = "TC100_";
    const std::string other = "TC200_ED3B";
    const auto check = [&](const std::string &name) {
        return camera::ble::name_has_prefix(
            reinterpret_cast<const std::uint8_t *>(name.data()), name.size(),
            prefixes, 1);
    };
    CHECK(check(a));
    CHECK(check(b));
    CHECK_FALSE(check(bare));
    CHECK_FALSE(check(other));
    CHECK_FALSE(camera::ble::name_has_prefix(nullptr, 0, prefixes, 1));
}

TEST_CASE("the cameras done in a scan are remembered by address and type")
{
    camera::ble::AddressSet done;
    const std::uint8_t first[6] = {0x3B, 0xED, 0x06, 0x56, 0xC1, 0xF4};
    const std::uint8_t second[6] = {0xF0, 0xA1, 0x06, 0x56, 0xC1, 0xF4};
    CHECK_FALSE(done.contains(0, first));
    CHECK(done.add(0, first));
    CHECK(done.add(0, first));  // once only
    CHECK(done.size() == 1);
    CHECK(done.contains(0, first));
    CHECK_FALSE(done.contains(1, first));
    CHECK_FALSE(done.contains(0, second));

    std::uint8_t address[6] = {};
    for (std::size_t i = 1; i < camera::ble::AddressSet::kMax; i++) {
        address[0] = static_cast<std::uint8_t>(i);
        CHECK(done.add(1, address));
    }
    CHECK_FALSE(done.add(0, second));  // full
    CHECK_FALSE(done.contains(0, second));

    done.clear();
    CHECK(done.size() == 0);
    CHECK_FALSE(done.contains(0, first));
}

TEST_CASE("format_address prints the address most significant byte first")
{
    const std::uint8_t address[6] = {0x3b, 0xed, 0x06, 0x56, 0xc1, 0xf4};
    char text[18];
    camera::ble::format_address(address, text);
    CHECK(std::string(text) == "F4:C1:56:06:ED:3B");
}

TEST_CASE("SeenList keeps one entry per address with the latest values")
{
    camera::ble::SeenList seen;
    const std::uint8_t a[6] = {1, 0, 0, 0, 0, 0xaa};
    const std::uint8_t b[6] = {2, 0, 0, 0, 0, 0xaa};
    const std::uint8_t name[] = {'T', 'C', '1', '0', '0',
                                 '_', '0', '0', '0', '1'};
    CHECK(seen.note(a, name, sizeof(name), -80, 100));
    CHECK(seen.note(a, nullptr, 0, -70, 105));  // no name in this report
    CHECK(seen.note(b, nullptr, 0, -60, 106));
    REQUIRE(seen.size() == 2);
    CHECK(std::string(seen.at(0).address) == "AA:00:00:00:00:01");
    CHECK(std::string(seen.at(0).name) == "TC100_0001");
    CHECK(seen.at(0).rssi == -70);
    CHECK(seen.at(0).seen_at == 105);
    CHECK(std::string(seen.at(1).name).empty());

    std::uint8_t other[6] = {0, 0, 0, 0, 0, 0xbb};
    for (std::uint8_t i = 0; i < camera::ble::SeenList::kMax; ++i) {
        other[0] = i;
        seen.note(other, nullptr, 0, -50, 107);
    }
    CHECK(seen.size() == camera::ble::SeenList::kMax);
    other[0] = 0xff;
    CHECK_FALSE(seen.note(other, nullptr, 0, -50, 108));
    seen.clear();
    CHECK(seen.size() == 0);
}
