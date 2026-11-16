#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "deferred_command.hpp"

using camera::link::DeferredCommand;

TEST_CASE("no command is deferred at first")
{
    DeferredCommand deferred;
    CHECK_FALSE(deferred.waiting());
    CHECK_FALSE(deferred.due(1000));
    CHECK(deferred.time_until_retry_us(1000, 500000) == 500000);
    CHECK(deferred.held_us(1000) == 0);
}

TEST_CASE("a held command is due at once and keeps its packet")
{
    DeferredCommand deferred;
    const std::vector<std::uint8_t> packet = {1, 2, 3};
    CHECK_FALSE(deferred.hold(packet, 772, "ARTEMIS GetThumbnail", 1000));
    CHECK(deferred.waiting());
    CHECK(deferred.due(1000));
    CHECK(deferred.time_until_retry_us(1000, 500000) == 0);
    CHECK(deferred.packet() == packet);
    CHECK(deferred.cmd_id() == 772);
    CHECK(std::strcmp(deferred.context(), "ARTEMIS GetThumbnail") == 0);
    CHECK(deferred.held_us(71000) == 70000);
}

TEST_CASE("a failed send is tried again after a pause")
{
    DeferredCommand deferred;
    deferred.hold({1}, 772, "ARTEMIS GetThumbnail", 1000);
    deferred.note_send_failed(2000);
    CHECK(deferred.waiting());
    CHECK_FALSE(deferred.due(2000));
    CHECK_FALSE(deferred.due(2000 + DeferredCommand::kRetryIntervalUs - 1));
    CHECK(deferred.due(2000 + DeferredCommand::kRetryIntervalUs));
    CHECK(deferred.time_until_retry_us(2000, 500000) ==
          DeferredCommand::kRetryIntervalUs);
    // The wait never exceeds what the caller would sleep anyway.
    CHECK(deferred.time_until_retry_us(2000, 50000) == 50000);
}

TEST_CASE("a newer command supersedes the held one")
{
    DeferredCommand deferred;
    deferred.hold({1}, 772, "ARTEMIS GetThumbnail", 1000);
    deferred.note_send_failed(1000);
    CHECK(deferred.hold({2, 3}, 1285, "ARTEMIS Download", 5000));
    CHECK(deferred.packet() == std::vector<std::uint8_t>{2, 3});
    CHECK(deferred.cmd_id() == 1285);
    // The pause of the superseded command does not carry over.
    CHECK(deferred.due(5000));
    CHECK(deferred.held_us(5000) == 0);
}

TEST_CASE("a cleared command is gone")
{
    DeferredCommand deferred;
    deferred.hold({1}, 772, nullptr, 1000);
    CHECK(std::strcmp(deferred.context(), "") == 0);
    deferred.clear();
    CHECK_FALSE(deferred.waiting());
    CHECK_FALSE(deferred.due(10000));
    CHECK(deferred.packet().empty());
    CHECK(deferred.cmd_id() == 0);
}
