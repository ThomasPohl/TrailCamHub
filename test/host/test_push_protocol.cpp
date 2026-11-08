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

TEST_CASE("push: JSON values at the top level")
{
    const std::string json =
        R"({"ok":true,"a":{"id":5,"b":"x\"y"},"id":7,"s":"t\/u\n"})";
    CHECK(json_value(json, "ok") == "true");
    CHECK(json_value(json, "id") == "7");
    CHECK(json_value(json, "a") == R"({"id":5,"b":"x\"y"})");
    CHECK(json_unquote(json_value(json_value(json, "a"), "b")) == "x\"y");
    CHECK(json_unquote(json_value(json, "s")) == "t/u\n");
    CHECK(json_value(json, "missing").empty());
    CHECK(api_ok(json));
    CHECK_FALSE(api_ok(R"({"ok":false,"error_code":409})"));
}

TEST_CASE("push: Telegram updates")
{
    const std::string json = R"({"ok":true,"result":[
      {"update_id":100,"message":{"message_id":1,"from":{"id":11,"is_bot":false},
        "chat":{"id":11,"type":"private"},"date":1,"text":"/start"}},
      {"update_id":101,"callback_query":{"id":"abc","from":{"id":11},
        "message":{"message_id":2,"chat":{"id":-1001,"type":"group"}},
        "chat_instance":"x","data":"v:123:F4_C1_56"}},
      {"update_id":102,"edited_message":{"chat":{"id":11}}}
    ]})";
    std::vector<Update> updates;
    REQUIRE(parse_updates(json, updates));
    REQUIRE(updates.size() == 3);
    CHECK(updates[0].update_id == 100);
    CHECK(updates[0].chat_id == 11);
    CHECK(updates[0].text == "/start");
    CHECK(updates[1].callback_id == "abc");
    CHECK(updates[1].chat_id == -1001);
    CHECK(updates[1].callback_data == "v:123:F4_C1_56");
    CHECK(updates[2].update_id == 102);
    CHECK(updates[2].chat_id == 0);

    CHECK(parse_updates(R"({"ok":true,"result":[]})", updates));
    CHECK(updates.empty());
    CHECK_FALSE(parse_updates(R"({"ok":false,"error_code":409})", updates));
}

TEST_CASE("push: commands and buttons")
{
    CHECK(command_of("/scan") == "/scan");
    CHECK(command_of("/status@TrailBot now") == "/status");
    CHECK(command_of("hello").empty());

    const std::string data = video_button("F4_C1_56_06_ED_3B", 1176197582);
    CHECK(data == "v:1176197582:F4_C1_56_06_ED_3B");
    CHECK(data.size() <= 64);
    std::string camera;
    std::int64_t media = 0;
    REQUIRE(parse_video_button(data, camera, media));
    CHECK(camera == "F4_C1_56_06_ED_3B");
    CHECK(media == 1176197582);
    CHECK_FALSE(parse_video_button("x:1:a", camera, media));
    CHECK_FALSE(parse_video_button("v::a", camera, media));
    CHECK(video_button(std::string(100, 'c'), 1).size() == 64);

    CHECK(
        inline_keyboard("Get \"video\"", "v:1:a") ==
        R"({"inline_keyboard":[[{"text":"Get \"video\"","callback_data":"v:1:a"}]]})");
}

TEST_CASE("push: multipart body")
{
    const std::string head = multipart_head("XYZ", {{"chat_id", "11"}}, "photo",
                                            "a.jpg", "image/jpeg");
    CHECK(head == "--XYZ\r\nContent-Disposition: form-data; "
                  "name=\"chat_id\"\r\n\r\n11\r\n--XYZ\r\nContent-Disposition: "
                  "form-data; name=\"photo\"; filename=\"a.jpg\"\r\n"
                  "Content-Type: image/jpeg\r\n\r\n");
    CHECK(multipart_tail("XYZ") == "\r\n--XYZ--\r\n");
}
