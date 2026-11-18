#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "push_protocol.hpp"

using namespace uplink::push;

TEST_CASE("push: file names from the camera's wall clock")
{
    // 2026-11-08 15:04:05 on the camera's clock (read as UTC).
    CHECK(file_name(1794150245, 1176197582, "jpg") ==
          "2026-11-08_15-04-05_1176197582.jpg");
    CHECK(file_name(0, 42, "mp4") == "42.mp4");
    CHECK(wall_time(1794150245) == "2026-11-08 15:04");
    CHECK(wall_time(0).empty());
}

TEST_CASE("push: WebDAV URLs")
{
    CHECK(join_url("https://nas/dav/", {"F4_C1", "thumb", "a b.jpg"}) ==
          "https://nas/dav/F4_C1/thumb/a%20b.jpg");
    CHECK(join_url("https://nas/dav", {}) == "https://nas/dav");
    CHECK(encode_segment("ä") == "%C3%A4");
}
