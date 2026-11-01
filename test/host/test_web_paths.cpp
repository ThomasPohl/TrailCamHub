#include <doctest/doctest.h>

#include <string>

#include "web_paths.hpp"

TEST_CASE("url_decode handles escapes and rejects malformed input")
{
    std::string out;
    CHECK(web::url_decode("TC100%2Fthumb/100_36.jpg", out));
    CHECK(out == "TC100/thumb/100_36.jpg");
    CHECK(web::url_decode("a+b%20c", out));
    CHECK(out == "a b c");
    CHECK_FALSE(web::url_decode("bad%2", out));
    CHECK_FALSE(web::url_decode("bad%zz", out));
    CHECK_FALSE(web::url_decode("nul%00byte", out));
}

TEST_CASE("sanitize_path normalizes and stays below the root")
{
    std::string out;
    CHECK(web::sanitize_path("", out));
    CHECK(out.empty());
    CHECK(web::sanitize_path("/", out));
    CHECK(out.empty());
    CHECK(web::sanitize_path("/cam//media/./100_1.jpg", out));
    CHECK(out == "cam/media/100_1.jpg");
    CHECK(web::sanitize_path("logs/", out));
    CHECK(out == "logs");
    CHECK_FALSE(web::sanitize_path("..", out));
    CHECK_FALSE(web::sanitize_path("cam/../../etc", out));
    CHECK_FALSE(web::sanitize_path("cam\\x", out));
    CHECK_FALSE(web::sanitize_path("cam/\x01", out));
    // ".." only as a whole segment is special.
    CHECK(web::sanitize_path("a..b/c..", out));
    CHECK(out == "a..b/c..");
}

TEST_CASE("content_type by extension")
{
    CHECK(std::string(web::content_type("100_1.JPG")) == "image/jpeg");
    CHECK(std::string(web::content_type("100_2.mp4")) == "video/mp4");
    CHECK(std::string(web::content_type("log-00001.txt")) ==
          "text/plain; charset=utf-8");
    CHECK(std::string(web::content_type("catalog.csv")) ==
          "text/plain; charset=utf-8");
    CHECK(std::string(web::content_type("100_2.aac")) == "audio/aac");
    CHECK(std::string(web::content_type("100_3.part")) ==
          "application/octet-stream");
}

TEST_CASE("append_json_string escapes quotes, backslashes and controls")
{
    std::string out;
    web::append_json_string(out, "a\"b\\c\nd");
    CHECK(out == "\"a\\\"b\\\\c\\u000ad\"");
}

TEST_CASE("is_temporary_file marks files the firmware still writes")
{
    CHECK(web::is_temporary_file("cam/media/100_1.part"));
    CHECK(web::is_temporary_file("cam/catalog.csv.tmp"));
    CHECK(web::is_temporary_file("X.PART"));
    CHECK_FALSE(web::is_temporary_file("cam/media/100_1.jpg"));
    CHECK_FALSE(web::is_temporary_file("logs/log-00001.txt"));
}

TEST_CASE("mp4_sources names the raw streams of an MP4 file")
{
    std::string video;
    std::string audio;
    REQUIRE(web::mp4_sources("cam/media/100_10.mp4", video, audio));
    CHECK(video == "cam/media/100_10.h264");
    CHECK(audio == "cam/media/100_10.aac");
    REQUIRE(web::mp4_sources("a.MP4", video, audio));
    CHECK(video == "a.h264");
    CHECK_FALSE(web::mp4_sources("cam/media/100_10.h264", video, audio));
    CHECK_FALSE(web::mp4_sources(".mp4", video, audio));
    CHECK_FALSE(web::mp4_sources("mp4", video, audio));
}

TEST_CASE("parse_range handles one byte range")
{
    std::uint64_t first = 99;
    std::uint64_t last = 99;
    using R = web::RangeResult;
    CHECK(web::parse_range("bytes=0-99", 1000, first, last) == R::kPartial);
    CHECK((first == 0 && last == 99));
    CHECK(web::parse_range("bytes=500-", 1000, first, last) == R::kPartial);
    CHECK((first == 500 && last == 999));
    CHECK(web::parse_range("bytes=-100", 1000, first, last) == R::kPartial);
    CHECK((first == 900 && last == 999));
    CHECK(web::parse_range("bytes=-5000", 1000, first, last) == R::kPartial);
    CHECK((first == 0 && last == 999));
    CHECK(web::parse_range("bytes=900-5000", 1000, first, last) == R::kPartial);
    CHECK((first == 900 && last == 999));
    CHECK(web::parse_range("bytes=1000-", 1000, first, last) ==
          R::kUnsatisfiable);
    CHECK(web::parse_range("bytes=-0", 1000, first, last) == R::kUnsatisfiable);
    CHECK(web::parse_range("bytes=0-0", 0, first, last) == R::kUnsatisfiable);
}

TEST_CASE("parse_range ignores headers it cannot use")
{
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    using R = web::RangeResult;
    CHECK(web::parse_range("", 1000, first, last) == R::kNone);
    CHECK(web::parse_range("items=0-9", 1000, first, last) == R::kNone);
    CHECK(web::parse_range("bytes=0-9,20-29", 1000, first, last) == R::kNone);
    CHECK(web::parse_range("bytes=9-0", 1000, first, last) == R::kNone);
    CHECK(web::parse_range("bytes=a-b", 1000, first, last) == R::kNone);
    CHECK(web::parse_range("bytes=5", 1000, first, last) == R::kNone);
}

TEST_CASE("json_int and json_string read the fields of camera.json")
{
    const std::string json =
        "{\"devId\":\"F4:C1:56:03:8A:31\",\"batPercent\":80,"
        "\"readAt\":0,\"lastSyncAt\": 1730400000,\"neg\":-1,"
        "\"name\":\"a\\\"b\"}";
    long long value = 7;
    std::string text;
    REQUIRE(web::json_int(json, "batPercent", value));
    CHECK(value == 80);
    REQUIRE(web::json_int(json, "lastSyncAt", value));
    CHECK(value == 1730400000);
    REQUIRE(web::json_int(json, "neg", value));
    CHECK(value == -1);
    REQUIRE(web::json_int(json, "readAt", value));
    CHECK(value == 0);
    CHECK_FALSE(web::json_int(json, "missing", value));
    CHECK_FALSE(web::json_int(json, "devId", value));  // a string
    REQUIRE(web::json_string(json, "devId", text));
    CHECK(text == "F4:C1:56:03:8A:31");
    REQUIRE(web::json_string(json, "name", text));
    CHECK(text == "a\"b");
    CHECK_FALSE(web::json_string(json, "batPercent", text));  // an integer
    CHECK_FALSE(web::json_string(json, "missing", text));
}
