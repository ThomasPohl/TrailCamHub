#include <doctest/doctest.h>

#include <string>

#include "command_parser.hpp"
#include "hub_config_rules.hpp"

using namespace hub_console;

TEST_CASE("simple commands")
{
    CHECK(parse_command("scan\n").type == CommandType::scan);
    CHECK(parse_command("  upload \r\n").type == CommandType::upload);
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
}

TEST_CASE("secrets are never displayed")
{
    using hub_config::display_value;
    using hub_config::Key;
    CHECK(display_value(Key::wifi_ssid, "Home") == "Home");
    CHECK(display_value(Key::wifi_password, "secret123") == "(set)");
    CHECK(display_value(Key::wifi_password, "") == "(not set)");
    CHECK(display_value(Key::device_key, "CAM1.abcdefghijklmnopqr") ==
          "CAM1.*** (set)");
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
