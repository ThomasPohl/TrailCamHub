#include <doctest/doctest.h>

#include <string>

#include "command_parser.hpp"
#include "hub_config_rules.hpp"

using namespace hub_console;

TEST_CASE("simple commands")
{
    CHECK(parse_command("scan\n").type == CommandType::scan);
    CHECK(parse_command("  upload \r\n").type == CommandType::upload);
    CHECK(parse_command("update").type == CommandType::update);
    CHECK(parse_command("update now").type == CommandType::invalid);
    CHECK(parse_command("status").type == CommandType::status);
    CHECK(parse_command("help").type == CommandType::help);
    CHECK(parse_command("tch scan").type == CommandType::scan);
    CHECK(parse_command("").type == CommandType::empty);
    CHECK(parse_command("   \r\n").type == CommandType::empty);
}

TEST_CASE("unknown commands and stray arguments are rejected")
{
    Command command = parse_command("format sdcard");
    CHECK(command.type == CommandType::invalid);
    CHECK(command.error.find("format") != std::string::npos);
    CHECK(parse_command("scan now").type == CommandType::invalid);
    CHECK(parse_command("config").type == CommandType::invalid);
    CHECK(parse_command("config set").type == CommandType::invalid);
}

TEST_CASE("config set keeps inner spaces of the value")
{
    const Command command = parse_command("config set wifi_ssid My Home Net");
    CHECK(command.type == CommandType::config_set);
    CHECK(command.key == "wifi_ssid");
    CHECK(command.value == "My Home Net");
}

TEST_CASE("config set-hex decodes any characters")
{
    // "p@ss word " including the trailing space
    const Command command =
        parse_command("config set-hex wifi_password 7040737320776f726420");
    CHECK(command.type == CommandType::config_set);
    CHECK(command.key == "wifi_password");
    CHECK(command.value == "p@ss word ");
    CHECK(parse_command("config set-hex wifi_password 7g").type ==
          CommandType::invalid);
    CHECK(parse_command("config set-hex wifi_password 123").type ==
          CommandType::invalid);
}

TEST_CASE("config unset, show and clear")
{
    const Command unset = parse_command("config unset api_url");
    CHECK(unset.type == CommandType::config_set);
    CHECK(unset.key == "api_url");
    CHECK(unset.value.empty());
    CHECK(parse_command("config show").type == CommandType::config_show);
    CHECK(parse_command("config clear").type == CommandType::config_clear);
    CHECK(parse_command("config clear all").type == CommandType::invalid);
}

TEST_CASE("hex_decode")
{
    std::string out;
    CHECK(hex_decode("48656C6c6f", out));
    CHECK(out == "Hello");
    CHECK(hex_decode("", out));
    CHECK(out.empty());
    CHECK_FALSE(hex_decode("zz", out));
}

TEST_CASE("setting names round-trip")
{
    for (hub_config::Key key : hub_config::kAllKeys) {
        hub_config::Key parsed;
        REQUIRE(hub_config::key_from_name(hub_config::key_name(key), parsed));
        CHECK(parsed == key);
        CHECK(std::string(hub_config::key_name(key)).size() <= 15);  // NVS
    }
    hub_config::Key unused;
    CHECK_FALSE(hub_config::key_from_name("password", unused));
}

TEST_CASE("setting validation")
{
    using hub_config::Key;
    using hub_config::validate;
    CHECK(validate(Key::wifi_ssid, "Home").empty());
    CHECK_FALSE(validate(Key::wifi_ssid, "").empty());
    CHECK_FALSE(validate(Key::wifi_ssid, std::string(33, 'x')).empty());

    CHECK(validate(Key::wifi_password, "").empty());  // open network
    CHECK(validate(Key::wifi_password, "12345678").empty());
    CHECK_FALSE(validate(Key::wifi_password, "short").empty());

    CHECK(validate(Key::api_url, "https://abc.lambda-url.eu-central-1.on.aws/")
              .empty());
    CHECK_FALSE(validate(Key::api_url, "http://example.com").empty());
    CHECK_FALSE(validate(Key::api_url, "https://").empty());
    CHECK_FALSE(validate(Key::api_url, "https://a b").empty());

    CHECK(validate(Key::device_key,
                   "F4C15606ED3B.abcdefghijklmnopqrstuvwxyz_-0123456789AB")
              .empty());
    CHECK_FALSE(validate(Key::device_key, "nodot").empty());
    CHECK_FALSE(validate(Key::device_key, ".abcdefghijklmnopq").empty());
    CHECK_FALSE(validate(Key::device_key, "CAM.short").empty());
    CHECK_FALSE(validate(Key::device_key, "CAM.abcdefghijklmnop!").empty());

    CHECK(validate(Key::camera_plan, "05:00-09:00=30,*=120").empty());
    CHECK(validate(Key::camera_plan, "off").empty());
    CHECK(validate(Key::camera_plan, "05:00-09:00=30").empty());
    CHECK_FALSE(validate(Key::camera_plan, "every hour").empty());
    CHECK_FALSE(validate(Key::camera_plan, "*=2000").empty());

    CHECK(validate(Key::checkin_min, "0").empty());
    CHECK(validate(Key::checkin_min, "30").empty());
    CHECK(validate(Key::checkin_min, "1440").empty());
    CHECK_FALSE(validate(Key::checkin_min, "1441").empty());
    CHECK_FALSE(validate(Key::checkin_min, "-5").empty());
    CHECK_FALSE(validate(Key::checkin_min, "30m").empty());

    CHECK(validate(Key::transport, "wifi").empty());
    CHECK(validate(Key::transport, "modem").empty());
    CHECK_FALSE(validate(Key::transport, "lte").empty());

    CHECK(validate(Key::apn, "internet.telekom").empty());
    CHECK(validate(Key::apn, "web.vodafone.de").empty());
    CHECK(validate(Key::apn, "iot-1_x").empty());
    // It goes into an AT command: no quotes, separators or spaces.
    CHECK_FALSE(validate(Key::apn, "a\"b").empty());
    CHECK_FALSE(validate(Key::apn, "a;b").empty());
    CHECK_FALSE(validate(Key::apn, "my apn").empty());
    CHECK_FALSE(validate(Key::apn, std::string(64, 'a')).empty());

    CHECK(validate(Key::sim_pin, "1234").empty());
    CHECK(validate(Key::sim_pin, "0000").empty());
    // The modem driver sends 4-digit PINs only.
    CHECK_FALSE(validate(Key::sim_pin, "12345").empty());
    CHECK_FALSE(validate(Key::sim_pin, "12345678").empty());
    CHECK_FALSE(validate(Key::sim_pin, "123").empty());
    CHECK_FALSE(validate(Key::sim_pin, "12a4").empty());
}

TEST_CASE("secrets are never displayed")
{
    using hub_config::display_value;
    using hub_config::Key;
    CHECK(display_value(Key::wifi_ssid, "Home") == "Home");
    CHECK(display_value(Key::wifi_password, "secret123") == "(set)");
    CHECK(display_value(Key::wifi_password, "") == "(not set)");
    CHECK(display_value(Key::camera_plan, "") == "(default)");
    CHECK(display_value(Key::camera_plan, "*=60") == "*=60");
    CHECK(display_value(Key::device_key, "CAM1.abcdefghijklmnopqr") ==
          "CAM1.*** (set)");
    CHECK(display_value(Key::sim_pin, "1234") == "(set)");
    CHECK(display_value(Key::sim_pin, "") == "(not set)");
    CHECK(display_value(Key::transport, "") == "(default)");
    CHECK(display_value(Key::apn, "") == "(default)");
    CHECK(hub_config::is_secret(Key::sim_pin));
}

TEST_CASE("the transport is the Wi-Fi or the modem")
{
    using hub_config::Transport;
    hub_config::Settings settings;
    settings.api_url = "https://api.example.com/";
    settings.device_key = "CAM1.abcdefghijklmnopqr";

    // Without a modem in the build: always the Wi-Fi, which must be set.
    CHECK(hub_config::transport(settings, false) == Transport::wifi);
    CHECK_FALSE(hub_config::uplink_configured(settings, false));
    settings.transport = "modem";
    CHECK(hub_config::transport(settings, false) == Transport::wifi);

    // With a modem: no home Wi-Fi means the modem.
    settings.transport.clear();
    CHECK(hub_config::transport(settings, true) == Transport::modem);
    CHECK(hub_config::uplink_configured(settings, true));
    // A home Wi-Fi is used when it is set ...
    settings.wifi_ssid = "Home";
    settings.wifi_password = "12345678";
    CHECK(hub_config::transport(settings, true) == Transport::wifi);
    // ... unless the modem is chosen.
    settings.transport = "modem";
    CHECK(hub_config::transport(settings, true) == Transport::modem);
    settings.transport = "wifi";
    settings.wifi_ssid.clear();
    CHECK(hub_config::transport(settings, true) == Transport::wifi);
    CHECK_FALSE(hub_config::uplink_configured(settings, true));
    CHECK(std::string(hub_config::transport_name(Transport::modem)) == "modem");

    // The backend settings are always needed.
    settings.transport = "modem";
    settings.device_key.clear();
    CHECK_FALSE(hub_config::uplink_configured(settings, true));
}

TEST_CASE("uplink needs all settings")
{
    hub_config::Settings settings;
    CHECK_FALSE(hub_config::uplink_configured(settings));
    settings.wifi_ssid = "Home";
    settings.wifi_password = "12345678";
    settings.api_url = "https://api.example.com/";
    CHECK_FALSE(hub_config::uplink_configured(settings));
    settings.device_key = "CAM1.abcdefghijklmnopqr";
    CHECK(hub_config::uplink_configured(settings));
    CHECK(hub_config::api_base(settings.api_url) == "https://api.example.com");
}

TEST_CASE("camlist takes no arguments")
{
    CHECK(parse_command("camlist").type == CommandType::camlist);
    CHECK(parse_command("tch camlist\n").type == CommandType::camlist);
    CHECK(parse_command("camlist all").type == CommandType::invalid);
}

TEST_CASE("stop and the diagnostic commands")
{
    CHECK(parse_command("stop").type == CommandType::stop);
    CHECK(parse_command("stop now").type == CommandType::invalid);

    Command command = parse_command("heap");
    CHECK(command.type == CommandType::heap);
    CHECK_FALSE(command.blocks);
    CHECK(parse_command("heap blocks").blocks);
    CHECK(parse_command("heap all").type == CommandType::invalid);

    command = parse_command("ls");
    CHECK(command.type == CommandType::ls);
    CHECK(command.path.empty());
    command = parse_command("ls System Volume Information");
    CHECK(command.path == "System Volume Information");
}

TEST_CASE("cat takes a path and an optional range")
{
    Command command = parse_command("cat logs/log-00003.txt");
    CHECK(command.type == CommandType::cat);
    CHECK(command.path == "logs/log-00003.txt");
    CHECK(command.offset == 0);
    CHECK(command.length == -1);

    command = parse_command("cat a.h264 4096 192");
    CHECK(command.offset == 4096);
    CHECK(command.length == 192);

    CHECK(parse_command("cat").type == CommandType::invalid);
    CHECK(parse_command("cat a -5").type == CommandType::invalid);
    CHECK(parse_command("cat a 1 2 3").type == CommandType::invalid);
    CHECK(parse_command("cat a 0x10").type == CommandType::invalid);
}

TEST_CASE("rm takes -r and a path with spaces")
{
    Command command = parse_command("rm logs/log-00001.txt");
    CHECK(command.type == CommandType::rm);
    CHECK_FALSE(command.recursive);
    CHECK(command.path == "logs/log-00001.txt");

    command = parse_command("rm -r F4_C1_56_03_8A_31");
    CHECK(command.recursive);
    CHECK(command.path == "F4_C1_56_03_8A_31");

    command = parse_command("rm -r My Camera");
    CHECK(command.path == "My Camera");

    CHECK(parse_command("rm").type == CommandType::invalid);
    CHECK(parse_command("rm -r").type == CommandType::invalid);
}

TEST_CASE("sdbench sizes")
{
    Command command = parse_command("sdbench");
    CHECK(command.type == CommandType::sdbench);
    CHECK(command.total_kb == 4096);
    CHECK(command.chunk_kb == 8);

    command = parse_command("sdbench 1024 16");
    CHECK(command.total_kb == 1024);
    CHECK(command.chunk_kb == 16);

    CHECK(parse_command("sdbench 0").type == CommandType::invalid);
    CHECK(parse_command("sdbench 1024 65").type == CommandType::invalid);
    CHECK(parse_command("sdbench 1024 16 1").type == CommandType::invalid);
}

TEST_CASE("a refused SIM PIN is not sent again")
{
    using hub_config::PinDecision;
    using hub_config::sim_pin_decision;
    using hub_config::sim_pin_fingerprint;
    const std::uint32_t refused = sim_pin_fingerprint("1234");
    CHECK(refused != 0);
    CHECK(sim_pin_fingerprint("1234") == refused);
    CHECK(sim_pin_fingerprint("4321") != refused);

    // Nothing refused yet, attempts unknown or enough.
    CHECK(sim_pin_decision(0, "1234", -1) == PinDecision::send);
    CHECK(sim_pin_decision(0, "1234", 3) == PinDecision::send);
    CHECK(sim_pin_decision(0, "1234", 2) == PinDecision::send);
    // The same PIN again: not sent, whatever the attempts.
    CHECK(sim_pin_decision(refused, "1234", 3) == PinDecision::refused_before);
    CHECK(sim_pin_decision(refused, "1234", -1) == PinDecision::refused_before);
    // A new PIN is tried.
    CHECK(sim_pin_decision(refused, "4321", 2) == PinDecision::send);
    // One attempt left: a wrong PIN would lock the card.
    CHECK(sim_pin_decision(0, "1234", 1) == PinDecision::too_few_attempts);
    CHECK(sim_pin_decision(refused, "4321", 0) ==
          PinDecision::too_few_attempts);
}

TEST_CASE("PIN attempts from AT+SPIC")
{
    using hub_config::parse_spic_pin_attempts;
    CHECK(parse_spic_pin_attempts("+SPIC: 3,10,3,10") == 3);
    CHECK(parse_spic_pin_attempts("\r\n+SPIC: 1,10,0,10\r\n") == 1);
    CHECK(parse_spic_pin_attempts("+SPIC:0,10,3,10") == 0);
    CHECK(parse_spic_pin_attempts("OK") == -1);
    CHECK(parse_spic_pin_attempts("+SPIC: ,10") == -1);
    CHECK(parse_spic_pin_attempts("+CPIN: READY") == -1);
}
