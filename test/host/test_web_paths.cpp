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
