#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include "status_screen.hpp"

using ui::Phase;
using ui::Screen;
using ui::Tone;

TEST_CASE("each phase has a title and fitting tone")
{
    CHECK(std::string(ui::make_screen(Phase::idle, "", 0).title) == "READY");
    CHECK(ui::make_screen(Phase::idle, "", 0).tone == Tone::neutral);
    CHECK(ui::make_screen(Phase::searching, "", 0).tone == Tone::busy);
    CHECK(ui::make_screen(Phase::connecting, "", 0).tone == Tone::busy);
    CHECK(ui::make_screen(Phase::connected, "", 0).tone == Tone::ok);
    CHECK(ui::make_screen(Phase::retry_wait, "", 0).tone == Tone::warning);
}

TEST_CASE("camera Wi-Fi name is shown while connecting and connected")
{
    CHECK(
        std::string(ui::make_screen(Phase::connecting, "CAM_1234", 0).line1) ==
        "CAM_1234");
    CHECK(std::string(ui::make_screen(Phase::connected, "CAM_1234", 0).line1) ==
          "CAM_1234");
    CHECK(std::string(ui::make_screen(Phase::connected, nullptr, 0).line1) ==
          "Camera Wi-Fi");
    CHECK(std::string(ui::make_screen(Phase::connected, "", 0).line1) ==
          "Camera Wi-Fi");
}

TEST_CASE("long or non-ASCII names are cut and sanitized")
{
    const Screen screen = ui::make_screen(
        Phase::connected, "CAM-\xc3\xa4-0123456789012345678901234567", 0);
    CHECK(std::strlen(screen.line1) == ui::kLineColumns);
    CHECK(std::string(screen.line1).substr(0, 8) == "CAM-?"
                                                    "?-0");
}

TEST_CASE("retry countdown")
{
    CHECK(std::string(ui::make_screen(Phase::retry_wait, "", 42).line1) ==
          "Next try in 42 s");
    CHECK_FALSE(ui::same_content(ui::make_screen(Phase::retry_wait, "", 42),
                                 ui::make_screen(Phase::retry_wait, "", 41)));
    CHECK(ui::same_content(ui::make_screen(Phase::retry_wait, "", 42),
                           ui::make_screen(Phase::retry_wait, "", 42)));
}

TEST_CASE("media progress is shown while connected")
{
    CHECK(std::string(
              ui::make_screen(Phase::connected, "", 0, {0, 0, 0}).line2) ==
          "Session running");
    CHECK(std::string(
              ui::make_screen(Phase::connected, "", 0, {2, 2, 12}).line2) ==
          "Saving media 3/12");
    CHECK(std::string(
              ui::make_screen(Phase::connected, "", 0, {12, 11, 12}).line2) ==
          "Media saved (12)");
    CHECK(std::string(
              ui::make_screen(Phase::searching, "", 0, {2, 2, 12}).line2) ==
          "via Bluetooth");
}

TEST_CASE("idle screen shows how to reach the web UI")
{
    const Screen plain = ui::make_screen(Phase::idle, "", 0);
    CHECK(std::string(plain.line1) == "Press button to");
    CHECK(std::string(plain.line3).empty());

    const Screen web =
        ui::make_screen(Phase::idle, "", 0, {0, 0, 0},
                        {"TrailCamHub-F4A0", "k7m2x9qa3b", "http://10.42.0.1"});
    CHECK(std::string(web.line1) == "Press button to search");
    CHECK(std::string(web.line2) == "WiFi TrailCamHub-F4A0");
    CHECK(std::string(web.line3) == "Key k7m2x9qa3b");
    CHECK(std::string(web.hint) == "http://10.42.0.1");

    // Only the idle screen shows it.
    const Screen searching =
        ui::make_screen(Phase::searching, "", 0, {0, 0, 0},
                        {"TrailCamHub-F4A0", "k7m2x9qa3b", "http://10.42.0.1"});
    CHECK(std::string(searching.line2) == "via Bluetooth");
}

TEST_CASE("finished screen shows the result and the web UI")
{
    const Screen plain =
        ui::make_screen(Phase::finished, "TrailCam_DA63", 0, {2, 1, 2});
    CHECK(std::string(plain.title) == "FINISHED");
    CHECK(plain.tone == Tone::ok);
    CHECK(std::string(plain.line1) == "Saved 1 of 2 files");
    CHECK(std::string(plain.line2) == "Press button to");
    CHECK(std::string(plain.line3) == "search again");

    CHECK(std::string(ui::make_screen(Phase::finished, "", 0).line1) ==
          "Camera synced");

    const Screen web =
        ui::make_screen(Phase::finished, "", 0, {2, 2, 2},
                        {"TrailCamHub-F4A1", "k7m2x9qa3b", "http://10.42.0.1"});
    CHECK(std::string(web.line1) == "Saved 2 of 2 files");
    CHECK(std::string(web.line2) == "WiFi TrailCamHub-F4A1");
    CHECK(std::string(web.hint) == "http://10.42.0.1");
}

TEST_CASE("all lines fit the screen")
{
    for (Phase phase : {Phase::idle, Phase::searching, Phase::connecting,
                        Phase::connected, Phase::retry_wait, Phase::finished}) {
        const Screen screen =
            ui::make_screen(phase, "", 4294967u, {99998, 99998, 99999},
                            {"TrailCamHub-ABCDEFGHIJKLMNOPQRSTUVWXYZ",
                             std::string(64, 'k').c_str(), "http://10.42.0.1"});
        CHECK(std::strlen(screen.title) <= ui::kTitleColumns);
        CHECK(std::strlen(screen.line1) <= ui::kLineColumns);
        CHECK(std::strlen(screen.line2) <= ui::kLineColumns);
        CHECK(std::strlen(screen.line3) <= ui::kLineColumns);
        CHECK(std::strlen(screen.hint) <= ui::kLineColumns);
    }
}
