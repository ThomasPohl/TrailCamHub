#include <doctest/doctest.h>

#include "media_store.hpp"

using camera::store::media_extension;
using camera::store::sanitize_camera_id;

TEST_CASE("sanitize_camera_id keeps safe characters and neutralises the rest")
{
    CHECK(sanitize_camera_id("TC100-ED3B") == "TC100-ED3B");
    CHECK(sanitize_camera_id("F4:C1:56:06:ED:3B") == "F4_C1_56_06_ED_3B");
    CHECK(sanitize_camera_id("a/b\\c..d") == "a_b_c..d");
    CHECK(sanitize_camera_id("") == "unknown");
    CHECK(sanitize_camera_id("..") == "_..");
    CHECK(sanitize_camera_id(".") == "_.");
}

TEST_CASE("sanitize_camera_id caps the length")
{
    std::string long_id(100, 'x');
    CHECK(sanitize_camera_id(long_id).size() == 48);
}

TEST_CASE("media_extension detects the file type from its first bytes")
{
    const std::uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xe1};
    const std::uint8_t mp4[] = {0,   0,   0,   0x20, 'f', 't',
                                'y', 'p', 'i', 's',  'o', 'm'};
    const std::uint8_t mov[] = {0,   0,   0,   0x14, 'f', 't',
                                'y', 'p', 'q', 't',  ' ', ' '};
    const std::uint8_t avi[] = {'R', 'I', 'F', 'F', 0,   0,
                                0,   0,   'A', 'V', 'I', ' '};
    const std::uint8_t other[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    CHECK(std::string(media_extension(jpeg, sizeof(jpeg))) == "jpg");
    CHECK(std::string(media_extension(mp4, sizeof(mp4))) == "mp4");
    CHECK(std::string(media_extension(mov, sizeof(mov))) == "mov");
    CHECK(std::string(media_extension(avi, sizeof(avi))) == "avi");
    CHECK(std::string(media_extension(other, sizeof(other))) == "bin");
    CHECK(std::string(media_extension(mp4, 6)) == "bin");
}
