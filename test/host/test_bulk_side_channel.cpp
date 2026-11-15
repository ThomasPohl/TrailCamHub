#include <doctest/doctest.h>

#include "bulk_side_channel.hpp"

using camera::artemis::BulkSideChannel;
using camera::artemis::is_bulk_side_channel;

TEST_CASE("bulk side channel: requested media is not a side channel")
{
    // Thumbnails, with and without a download running.
    CHECK_FALSE(is_bulk_side_channel(0x04, false));
    CHECK_FALSE(is_bulk_side_channel(0x04, true));
    // The file of a running download.
    CHECK_FALSE(is_bulk_side_channel(0x03, true));
    CHECK_FALSE(is_bulk_side_channel(0x05, true));
}

TEST_CASE("bulk side channel: the status records never are media")
{
    // The record that broke a thumbnail on 2026-11-15.
    CHECK(is_bulk_side_channel(0x06, false));
    CHECK(is_bulk_side_channel(0x06, true));
}

TEST_CASE("bulk side channel: nothing but thumbnails without a download")
{
    CHECK(is_bulk_side_channel(0x03, false));
    CHECK(is_bulk_side_channel(0x05, false));
    CHECK(is_bulk_side_channel(0x07, false));
}

TEST_CASE("bulk side channel: a repeated frame is not new")
{
    BulkSideChannel channel;
    CHECK(channel.note(0x06, 0));
    CHECK_FALSE(channel.note(0x06, 0));
    CHECK_FALSE(channel.note(0x06, 0));
    CHECK(channel.note(0x06, 1));
    CHECK_FALSE(channel.note(0x06, 1));
    // The same sequence on another subtype is another frame.
    CHECK(channel.note(0x07, 1));
    CHECK(channel.frames() == 3);
}

TEST_CASE("bulk side channel: reset forgets the last frame")
{
    BulkSideChannel channel;
    CHECK(channel.note(0x06, 5));
    channel.reset();
    CHECK(channel.frames() == 0);
    CHECK(channel.note(0x06, 5));
    CHECK(channel.frames() == 1);
}
