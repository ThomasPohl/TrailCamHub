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
    CHECK(std::string(ui::make_screen(Phase::not_found, "", 0).title) ==
          "NOT FOUND");
    CHECK(ui::make_screen(Phase::not_found, "", 0).tone == Tone::warning);
    CHECK(std::string(ui::make_screen(Phase::upload_failed, "", 0).title) ==
          "NO UPLOAD");
    CHECK(ui::make_screen(Phase::upload_failed, "", 0).tone == Tone::warning);
    CHECK(std::string(ui::make_screen(Phase::stopped, "", 0).title) ==
          "STOPPED");
    CHECK(ui::make_screen(Phase::stopped, "", 0).tone == Tone::neutral);
}

TEST_CASE("failed and stopped screens name the cause and how to go on")
{
    const Screen not_found = ui::make_screen(Phase::not_found, "", 0);
    CHECK(std::string(not_found.line1) == "Camera not reached");
    CHECK(std::string(not_found.line2) == "Press button to");
    CHECK(std::string(not_found.line3) == "search again");

    const Screen upload =
        ui::make_screen(Phase::upload_failed, "", 0, {2, 2, 2},
                        {"TrailCamHub-F4A1", "k7m2x9qa3b", "http://10.42.0.1"});
    CHECK(std::string(upload.line1) == "Home Wi-Fi not reached");
    CHECK(std::string(upload.line2) == "WiFi TrailCamHub-F4A1");
    CHECK(std::string(upload.hint) == "http://10.42.0.1");

    CHECK(std::string(ui::make_screen(Phase::stopped, "", 0).line1) ==
          "Search stopped");

    const Screen no_card = ui::make_screen(Phase::no_card, "", 0);
    CHECK(std::string(no_card.title) == "NO SD CARD");
    CHECK(no_card.tone == Tone::warning);
    CHECK(std::string(no_card.line1) == "Camera has no SD card");
    CHECK(std::string(no_card.line2) == "Press button to");
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
    for (Phase phase :
         {Phase::idle, Phase::searching, Phase::connecting, Phase::connected,
          Phase::retry_wait, Phase::finished, Phase::partial, Phase::not_found,
          Phase::no_card, Phase::upload_failed, Phase::stopped}) {
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

TEST_CASE("a session that ended early is not shown as finished")
{
    const Screen with_media = ui::make_screen(Phase::partial, "", 0, {3, 1, 3});
    CHECK(std::string(with_media.title) == "INCOMPLETE");
    CHECK(with_media.tone == Tone::warning);
    CHECK(std::string(with_media.line1) == "Saved 1 of 3 files");

    const Screen thumbnails_only = ui::make_screen(Phase::partial, "", 0);
    CHECK(std::string(thumbnails_only.line1) == "Session ended early");
}
