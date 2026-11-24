#include <doctest/doctest.h>

#include <string>

#include "modem_operator.hpp"

using modem_uplink::clean_operator_name;

TEST_CASE("modem: operator name for the check-in (#149)")
{
    SUBCASE("a plain name is kept")
    {
        CHECK(clean_operator_name("Telekom.de") == "Telekom.de");
        CHECK(clean_operator_name("  o2 - de  ") == "o2 - de");
        CHECK(clean_operator_name("26201") == "26201");
    }

    SUBCASE("the quoted name of a COPS answer")
    {
        CHECK(clean_operator_name("+COPS: 0,0,\"Vodafone.de\",7") ==
              "Vodafone.de");
        CHECK(clean_operator_name("\"1&1\"") == "1&1");
        // An unclosed quote takes the rest.
        CHECK(clean_operator_name("0,0,\"Telekom.de") == "Telekom.de");
    }

    SUBCASE("nothing usable gives an empty name")
    {
        CHECK(clean_operator_name("").empty());
        CHECK(clean_operator_name("   ").empty());
        CHECK(clean_operator_name("\"\"").empty());
        CHECK(clean_operator_name("Unknown").empty());
        CHECK(clean_operator_name("No Service").empty());
        CHECK(clean_operator_name("\x01\x02").empty());
    }

    SUBCASE("control and non-ASCII bytes are dropped, the length is capped")
    {
        CHECK(clean_operator_name("Tele\r\nkom\xc3\xa4") == "Telekom");
        const std::string name = clean_operator_name(std::string(100, 'a'));
        CHECK(name.size() == modem_uplink::kOperatorNameMax);
        // No trailing blank after the cut.
        const std::string cut =
            clean_operator_name(std::string(31, 'a') + " bbb");
        CHECK(cut == std::string(31, 'a'));
    }
}

TEST_CASE("a position is read from an AT+CGNSSINFO answer")
{
    double lat = 0;
    double lon = 0;
    REQUIRE(modem_uplink::parse_gnss_info(
        "\r\n+CGNSSINFO: 2,05,00,00,5230.123456,N,01324.123456,E,241124,"
        "074500.0,35.2,0.0,0.0,1.2,0.9,0.8\r\n\r\nOK\r\n",
        lat, lon));
    CHECK(lat == doctest::Approx(52.5020576).epsilon(0.00001));
    CHECK(lon == doctest::Approx(13.4020576).epsilon(0.00001));

    // South and west are negative.
    REQUIRE(modem_uplink::parse_gnss_info(
        "+CGNSSINFO: 3,08,02,00,3352.000000,S,15112.000000,W,241124,"
        "074500.0,10.0,0.0,0.0,1.0,0.8,0.6\r\nOK",
        lat, lon));
    CHECK(lat == doctest::Approx(-33.8667).epsilon(0.0001));
    CHECK(lon == doctest::Approx(-151.2).epsilon(0.0001));
}

TEST_CASE("an AT+CGNSSINFO answer without a fix gives no position")
{
    double lat = 1;
    double lon = 2;
    CHECK_FALSE(modem_uplink::parse_gnss_info(
        "\r\n+CGNSSINFO: ,,,,,,,,,,,,,,,\r\n\r\nOK\r\n", lat, lon));
    CHECK(lat == 1);
    CHECK(lon == 2);
    CHECK_FALSE(modem_uplink::parse_gnss_info("OK", lat, lon));
    CHECK_FALSE(modem_uplink::parse_gnss_info("+CGNSSINFO: 2,05", lat, lon));
    // Minutes of 60 or more, degrees out of range, a wrong hemisphere.
    CHECK_FALSE(modem_uplink::parse_gnss_info(
        "+CGNSSINFO: 2,05,00,00,5260.0,N,01324.0,E,a", lat, lon));
    CHECK_FALSE(modem_uplink::parse_gnss_info(
        "+CGNSSINFO: 2,05,00,00,9530.0,N,01324.0,E,a", lat, lon));
    CHECK_FALSE(modem_uplink::parse_gnss_info(
        "+CGNSSINFO: 2,05,00,00,5230.0,X,01324.0,E,a", lat, lon));
    CHECK_FALSE(modem_uplink::parse_gnss_info(
        "+CGNSSINFO: 2,05,00,00,52a0.0,N,01324.0,E,a", lat, lon));
}
