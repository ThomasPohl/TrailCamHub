#include <doctest/doctest.h>

#include "coprocessor_version.hpp"

using coprocessor::parse_version;
using coprocessor::Version;

TEST_CASE("co-processor versions are compared as numbers")
{
    Version v;
    REQUIRE(parse_version("2.6.1", v));
    CHECK(v == Version{2, 6, 1});
    REQUIRE(parse_version("v2.6.1", v));
    CHECK(v == Version{2, 6, 1});
    REQUIRE(parse_version("2.6.1-3-gabc1234-dirty", v));
    CHECK(v == Version{2, 6, 1});
    REQUIRE(parse_version("ESP-Hosted 10.20.300", v));
    CHECK(v == Version{10, 20, 300});
    REQUIRE(parse_version("2.06.01", v));
    CHECK(v == Version{2, 6, 1});
    CHECK(coprocessor::to_string(Version{2, 6, 1}) == "2.6.1");
    CHECK(Version{2, 6, 1} != Version{2, 6, 2});

    CHECK_FALSE(parse_version("", v));
    CHECK_FALSE(parse_version("2.6", v));
    CHECK_FALSE(parse_version("v2.x.1", v));
    CHECK_FALSE(parse_version("1.2.99999999999", v));
    // A later triple is found when an earlier part is not one.
    REQUIRE(parse_version("build 7 of 1.2.3", v));
    CHECK(v == Version{1, 2, 3});
}
