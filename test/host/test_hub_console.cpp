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
    CHECK(parse_command("restart").type == CommandType::restart);
    CHECK(parse_command("ping").type == CommandType::ping);
    CHECK(parse_command("restart now").type == CommandType::invalid);
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

TEST_CASE("mode: settings override the preset where the board allows")
{
    using namespace hub_config;
    Capabilities main_board;
    main_board.modem = true;
    main_board.battery_gauge = true;
    main_board.deep_sleep = true;
    Preset autonomous;
    autonomous.power = Power::battery;
    autonomous.direction = Direction::push;

    Settings settings;
    Mode mode = resolve_mode(settings, main_board, autonomous);
    CHECK(mode.power == Power::battery);
    CHECK(mode.power_origin == ModeOrigin::preset);
    CHECK(mode.direction == Direction::push);
    CHECK(mode.transport == Transport::modem);  // no home Wi-Fi set
    CHECK(sleeps(mode, main_board));
    CHECK(wifi_power_save(mode));
    CHECK(checks_in(mode));
    CHECK(reads_on_schedule(mode, false));

    settings.power = "mains";
    settings.wifi_ssid = "Home";
    mode = resolve_mode(settings, main_board, autonomous);
    CHECK(mode.power == Power::mains);
    CHECK(mode.power_origin == ModeOrigin::setting);
    CHECK(mode.transport == Transport::wifi);
    CHECK_FALSE(sleeps(mode, main_board));
    CHECK_FALSE(wifi_power_save(mode));
    // Mains, push: scheduled reads only with somewhere to push to.
    CHECK(reads_on_schedule(mode, true));
    CHECK_FALSE(reads_on_schedule(mode, false));

    // Pull needs a web UI: the main board falls back to push.
    settings.direction = "pull";
    mode = resolve_mode(settings, main_board, autonomous);
    CHECK(mode.direction == Direction::push);
    CHECK(mode.direction_origin == ModeOrigin::preset);
    CHECK(capability_problem(Key::direction, "pull", main_board) != "");
    CHECK(capability_problem(Key::transport, "modem", main_board).empty());
}

TEST_CASE("mode: an on-site dongle pulls and stays awake")
{
    using namespace hub_config;
    Capabilities dongle;
    dongle.display = true;
    dongle.button = true;
    dongle.web_ui = true;
    dongle.deep_sleep = true;
    const Preset on_site;  // mains + pull

    Settings settings;
    Mode mode = resolve_mode(settings, dongle, on_site);
    CHECK(mode.power == Power::mains);
    CHECK(mode.direction == Direction::pull);
    CHECK(mode.transport == Transport::wifi);
    CHECK_FALSE(sleeps(mode, dongle));
    CHECK_FALSE(checks_in(mode));
    CHECK_FALSE(reads_on_schedule(mode, false));
    CHECK(capability_problem(Key::transport, "modem", dongle) ==
          "this board has no modem");

    // Without a web UI there is nothing to pull from.
    Capabilities no_ui = dongle;
    no_ui.web_ui = false;
    mode = resolve_mode(settings, no_ui, on_site);
    CHECK(mode.direction == Direction::push);
    CHECK_FALSE(reads_on_schedule(mode, false));

    // Battery on a build without deep sleep is refused.
    Capabilities no_sleep = dongle;
    no_sleep.deep_sleep = false;
    CHECK(capability_problem(Key::power, "battery", no_sleep) != "");
    settings.power = "battery";
    mode = resolve_mode(settings, no_sleep, on_site);
    CHECK_FALSE(sleeps(mode, no_sleep));
}

TEST_CASE("mode settings are validated")
{
    using hub_config::Key;
    using hub_config::validate;
    CHECK(validate(Key::power, "mains").empty());
    CHECK(validate(Key::power, "battery").empty());
    CHECK_FALSE(validate(Key::power, "solar").empty());
    CHECK(validate(Key::direction, "push").empty());
    CHECK(validate(Key::direction, "pull").empty());
    CHECK_FALSE(validate(Key::direction, "both").empty());
    CHECK(validate(Key::destination, "backend").empty());
    CHECK_FALSE(validate(Key::destination, "ftp").empty());
    CHECK(hub_config::display_value(Key::power, "") == "(default)");
}

TEST_CASE("transport: Ethernet first on boards that have it")
{
    using namespace hub_config;
    Capabilities wired;
    wired.ethernet = true;
    Settings settings;
    CHECK(transport(settings, wired) == Transport::ethernet);
    settings.wifi_ssid = "Home";
    CHECK(transport(settings, wired) == Transport::ethernet);
    settings.transport = "wifi";
    CHECK(transport(settings, wired) == Transport::wifi);
    // A transport the board lacks falls back.
    settings.transport = "modem";
    CHECK(transport(settings, wired) == Transport::ethernet);
    CHECK(std::string(transport_name(Transport::ethernet)) == "ethernet");

    // Over Ethernet only the backend settings are needed.
    Settings backend;
    backend.api_url = "https://example.com/";
    backend.device_key = "hub1.abcdefghijklmnopqr";
    CHECK(uplink_configured(backend, wired));
    Capabilities wifi_only;
    CHECK_FALSE(uplink_configured(backend, wifi_only));

    CHECK(validate(Key::transport, "ethernet").empty());
    CHECK(capability_problem(Key::transport, "ethernet", wifi_only) ==
          "this board has no Ethernet");
    CHECK(capability_problem(Key::transport, "ethernet", wired).empty());

    // Boards without Ethernet keep the old rule.
    Capabilities modem_board;
    modem_board.modem = true;
    Settings empty;
    CHECK(transport(empty, modem_board) == Transport::modem);
    empty.wifi_ssid = "Home";
    CHECK(transport(empty, modem_board) == Transport::wifi);
}

TEST_CASE("destinations: settings and what each needs")
{
    using namespace hub_config;
    CHECK(validate(Key::destination, "webdav").empty());
    CHECK(validate(Key::destination, "telegram").empty());
    CHECK(validate(Key::webdav_url, "https://nas.local/dav/trailcam").empty());
    CHECK(validate(Key::webdav_url, "http://192.168.1.5:5005/cam").empty());
    CHECK_FALSE(validate(Key::webdav_url, "ftp://nas").empty());
    CHECK(validate(Key::webdav_user, "thomas").empty());
    CHECK_FALSE(validate(Key::webdav_user, "a:b").empty());
    CHECK(validate(Key::telegram_token,
                   "123456789:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsaw")
              .empty());
    CHECK_FALSE(validate(Key::telegram_token, "123456789").empty());
    CHECK_FALSE(validate(Key::telegram_token, "abc:AAHdqTcvCH1vGWJxfSeofSAs0K5")
                    .empty());
    CHECK(validate(Key::telegram_chat, "123456").empty());
    CHECK(validate(Key::telegram_chat, "-100123456").empty());
    CHECK_FALSE(validate(Key::telegram_chat, "-").empty());
    CHECK_FALSE(validate(Key::telegram_chat, "@me").empty());
    CHECK(display_value(Key::telegram_token,
                        "123456789:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsaw") ==
          "123456789:*** (set)");
    CHECK(display_value(Key::webdav_password, "x") == "(set)");
    CHECK(is_secret(Key::telegram_token));

    Capabilities modem;
    modem.modem = true;
    Capabilities wifi_only;
    CHECK(capability_problem(Key::destination, "telegram", modem).empty());
    CHECK_FALSE(
        capability_problem(Key::destination, "telegram", wifi_only).empty());
    CHECK(capability_problem(Key::destination, "webdav", wifi_only).empty());

    Settings s;
    s.destination = "webdav";
    s.wifi_ssid = "Home";
    CHECK(destination(s) == Destination::webdav);
    CHECK_FALSE(destination_configured(s));
    s.webdav_url = "https://nas.local/dav";
    CHECK(destination_configured(s));
    CHECK(uplink_configured(s, wifi_only));

    Settings t;
    t.destination = "telegram";
    t.telegram_token = "123456789:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsaw";
    CHECK(destination_configured(t));
    CHECK(uplink_configured(t, modem));  // no Wi-Fi: the modem
    CHECK(std::string(destination_name(
              resolve_mode(t, modem, Preset{}).destination)) == "telegram");
}

TEST_CASE("health check URL")
{
    using hub_config::Key;
    using hub_config::validate;
    CHECK(validate(Key::health_url, "https://hc-ping.com/abc-123").empty());
    CHECK(validate(Key::health_url, "http://10.0.0.2/ping").empty());
    CHECK_FALSE(validate(Key::health_url, "hc-ping.com/abc").empty());
    CHECK_FALSE(validate(Key::health_url, "https://a b").empty());
    CHECK(hub_config::display_value(Key::health_url, "") == "(not set)");
}

TEST_CASE("web UI password setting")
{
    using hub_config::Key;
    using hub_config::validate;
    CHECK(validate(Key::web_password, "abcdefgh").empty());
    CHECK(validate(Key::web_password, "with space ok!").empty());
    CHECK(validate(Key::web_password, std::string(63, 'x')).empty());
    CHECK_FALSE(validate(Key::web_password, "short").empty());
    CHECK_FALSE(validate(Key::web_password, std::string(64, 'x')).empty());
    CHECK_FALSE(validate(Key::web_password, "tab\there!").empty());
    CHECK(hub_config::is_secret(Key::web_password));
    CHECK(hub_config::display_value(Key::web_password, "secret123") == "(set)");
    CHECK(hub_config::display_value(Key::web_password, "") == "(default)");
}

TEST_CASE("camera login settings")
{
    using hub_config::Key;
    using hub_config::validate;
    CHECK(validate(Key::camera_user, "admin").empty());
    CHECK(validate(Key::camera_password, "my pass!").empty());
    CHECK_FALSE(validate(Key::camera_password, std::string(33, 'x')).empty());
    CHECK_FALSE(validate(Key::camera_user, "a\nb").empty());
    CHECK(hub_config::is_secret(Key::camera_password));
    CHECK_FALSE(hub_config::is_secret(Key::camera_user));
    CHECK(hub_config::display_value(Key::camera_password, "x") == "(set)");
    CHECK(hub_config::display_value(Key::camera_user, "") == "(default)");
}

TEST_CASE("every setting can be named")
{
    for (const char *name : {"health_url", "web_password", "telegram_chat",
                             "wifi_ssid", "camera_user", "camera_password"}) {
        hub_config::Key key;
        CHECK_MESSAGE(hub_config::key_from_name(name, key), name);
        CHECK(std::string(hub_config::key_name(key)) == name);
    }
}
