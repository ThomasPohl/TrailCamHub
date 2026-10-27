#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "hex_payload.hpp"

using camera::parse_hex_payload;

TEST_CASE("parses the default BT_KEY_ON payload")
{
    std::uint8_t out[20];
    std::size_t len = 99;
    REQUIRE(parse_hex_payload("42545F4B45595F4F4E", out, sizeof(out), &len));
    REQUIRE(len == 9);
    CHECK(std::string(reinterpret_cast<char *>(out), len) == "BT_KEY_ON");
}

TEST_CASE("accepts separators and lower case")
{
    std::uint8_t out[4];
    std::size_t len = 0;
    REQUIRE(parse_hex_payload(" de:ad-be ef ", out, sizeof(out), &len));
    REQUIRE(len == 4);
    CHECK(out[0] == 0xde);
    CHECK(out[1] == 0xad);
    CHECK(out[2] == 0xbe);
    CHECK(out[3] == 0xef);
}

TEST_CASE("empty payload yields zero bytes")
{
    std::uint8_t out[4];
    std::size_t len = 99;
    REQUIRE(parse_hex_payload("", out, sizeof(out), &len));
    CHECK(len == 0);
}

TEST_CASE("rejects invalid payloads")
{
    std::uint8_t out[2];
    std::size_t len = 0;
    CHECK_FALSE(parse_hex_payload("123", out, sizeof(out), &len));
    CHECK_FALSE(parse_hex_payload("1 2", out, sizeof(out), &len));
    CHECK_FALSE(parse_hex_payload("zz", out, sizeof(out), &len));
    CHECK_FALSE(parse_hex_payload("010203", out, sizeof(out), &len));
}
