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
