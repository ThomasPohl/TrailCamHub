#include <doctest/doctest.h>

#include "action_confirm.hpp"
#include "camera_action.hpp"

using namespace camera::action;
using camera::artemis::DeviceInfo;
using camera::artemis::MediaCatalog;
using camera::artemis::MediaItem;
using camera::artemis::MediaList;

namespace
{
MediaItem item(std::int64_t dir, std::int64_t num, std::int64_t type,
               std::int64_t id)
{
    MediaItem m;
    m.dir_num = dir;
    m.num = num;
    m.file_type = type;
    m.id = id;
    return m;
}
}  // namespace

TEST_CASE("actions have stable names and camera commands")
{
    CHECK(std::string(type_name(Type::photo)) == "photo");
    CHECK(std::string(type_name(Type::delete_media)) == "delete");
    CHECK(parse_type("video-start") == Type::video_start);
    CHECK_FALSE(parse_type("nonsense").has_value());
    CHECK(cmd_id(Type::photo) == 641);
    CHECK(cmd_id(Type::video_start) == 643);
    CHECK(cmd_id(Type::video_stop) == 644);
    CHECK(cmd_id(Type::delete_media) == 773);
    CHECK(cmd_id(Type::format_sd) == 518);
    CHECK(type_of_cmd(773) == Type::delete_media);
    CHECK_FALSE(type_of_cmd(512).has_value());
}

TEST_CASE("formatting is never remote and deleting needs the hub or its page")
{
    CHECK(allowed(Type::photo, Source::telegram));
    CHECK(allowed(Type::video_start, Source::backend));
    CHECK(allowed(Type::delete_media, Source::console));
    CHECK(allowed(Type::delete_media, Source::web_ui));
    CHECK_FALSE(allowed(Type::delete_media, Source::telegram));
    CHECK_FALSE(allowed(Type::delete_media, Source::backend));
    CHECK(allowed(Type::format_sd, Source::console));
    CHECK(allowed(Type::format_sd, Source::web_ui));
    CHECK_FALSE(allowed(Type::format_sd, Source::telegram));
    CHECK_FALSE(allowed(Type::format_sd, Source::backend));
    CHECK(needs_confirmation(Type::format_sd));
    CHECK(needs_confirmation(Type::delete_media));
    CHECK_FALSE(needs_confirmation(Type::photo));
    CHECK(needs_media_list(Type::delete_media));
    CHECK_FALSE(needs_media_list(Type::format_sd));
}

TEST_CASE("validation")
{
    CHECK(validate(Action{Type::photo, 0}).empty());
    CHECK(validate(Action{Type::delete_media, 42}).empty());
    CHECK_FALSE(validate(Action{Type::delete_media, 0}).empty());
    CHECK_FALSE(validate(Action{Type::delete_media, -5}).empty());
    CHECK_FALSE(validate(Action{Type::photo, 42}).empty());
}

TEST_CASE("a full card stops recording actions only")
{
    DeviceInfo info;
    info.sd_total_mb = 1000;
    info.sd_free_mb = 0;
    CHECK_FALSE(check_camera(Type::photo, info).empty());
    CHECK_FALSE(check_camera(Type::video_start, info).empty());
    CHECK(check_camera(Type::video_stop, info).empty());
    CHECK(check_camera(Type::format_sd, info).empty());
    info.sd_free_mb = 5;
    CHECK(check_camera(Type::photo, info).empty());
    // Unknown sizes do not block anything.
    CHECK(check_camera(Type::photo, DeviceInfo{}).empty());
}

TEST_CASE("a deletion is found by mediaId in the fresh list, not by dir/num")
{
    MediaCatalog catalog;
    MediaList list;
    list.files = {item(100, 7, 2, 111), item(100, 6, 0, 222)};
    catalog.merge(list);

    const auto target = find_target(catalog, 111);
    REQUIRE(target.has_value());
    CHECK(target->file_type == 2);
    CHECK(target->dir_num == 100);
    CHECK(target->media_num == 7);
    CHECK(request_fields(Action{Type::delete_media, 111}, target) ==
          ",\"fileType\":2,\"dirNum\":100,\"mediaNum\":7");

    // Unknown to the catalog.
    CHECK_FALSE(find_target(catalog, 999).has_value());

    // Known, but not in the list of this session (the card was cleared or the
    // file deleted): dir/num may belong to another recording now.
    catalog.begin_listing();
    MediaList next;
    next.files = {item(100, 6, 0, 222)};
    catalog.merge_page(next);
    CHECK_FALSE(find_target(catalog, 111).has_value());
    CHECK(find_target(catalog, 222).has_value());
}

TEST_CASE("only a deletion has request fields")
{
    CHECK(request_fields(Action{Type::photo, 0}, std::nullopt).empty());
    CHECK(request_fields(Action{Type::format_sd, 0}, std::nullopt).empty());
    CHECK(request_fields(Action{Type::delete_media, 1}, std::nullopt).empty());
}

TEST_CASE("success is the command's own field, not result")
{
    CHECK(interpret(Type::photo,
                    "{\"snapRet\":0,\"errorMsg\":\"Success\",\"result\":0,"
                    "\"cmdId\":641}")
              .done);
    CHECK(interpret(Type::video_start,
                    "{\"recordRet\":0,\"errorMsg\":\"Success\",\"result\":0,"
                    "\"cmdId\":643}")
              .done);
    CHECK(interpret(Type::video_stop,
                    "{\"errorMsg\":\"Success\",\"result\":0,\"cmdId\":644}")
              .done);
    CHECK(interpret(Type::delete_media,
                    "{\"delRet\":0,\"result\":0,\"cmdId\":773}")
              .done);
    CHECK(interpret(Type::format_sd,
                    "{\"errorMsg\":\"Success\",\"sdTotalMB\":122236,"
                    "\"sdFreeMB\":122234,\"formatRet\":0,\"result\":0,"
                    "\"cmdId\":518}")
              .done);

    // result 0, but nothing happened.
    const Outcome gone = interpret(
        Type::delete_media, "{\"delRet\":-3,\"result\":0,\"cmdId\":773}");
    CHECK_FALSE(gone.done);
    CHECK(gone.code == -3);
    CHECK(gone.reason.find("no such file") != std::string::npos);

    const Outcome snap = interpret(
        Type::photo, "{\"snapRet\":-2,\"errorMsg\":\"SD full\",\"result\":0}");
    CHECK_FALSE(snap.done);
    CHECK(snap.code == -2);
    CHECK(snap.reason == "the camera answered snapRet -2: SD full");

    // The field is missing: not a success, whatever result says.
    const Outcome missing = interpret(Type::photo, "{\"result\":0}");
    CHECK_FALSE(missing.done);
    CHECK(missing.reason == "the answer has no snapRet");
    CHECK_FALSE(interpret(Type::format_sd, "{\"result\":0}").done);
    CHECK_FALSE(interpret(Type::video_stop, "{\"errorMsg\":\"x\"}").done);
    CHECK_FALSE(interpret(Type::video_stop, "{\"result\":-1}").done);
}

TEST_CASE("the camera's message is only quoted if it is short plain text")
{
    const std::string json = std::string("{\"formatRet\":-1,\"errorMsg\":\"") +
                             std::string(80, 'x') + "\"}";
    CHECK(interpret(Type::format_sd, json).reason ==
          "the camera answered formatRet -1");
    CHECK(interpret(Type::format_sd, "{\"formatRet\":-1,\"errorMsg\":\"\"}")
              .reason == "the camera answered formatRet -1");
}

TEST_CASE("failure carries a reason and a code")
{
    const Outcome outcome = failure("not sent");
    CHECK_FALSE(outcome.done);
    CHECK(outcome.reason == "not sent");
    CHECK(outcome.code == -1);
}

TEST_CASE("the confirmation code is needed within a minute, once")
{
    ConfirmGate gate;
    const std::int64_t t0 = 1000000;
    const std::string code = gate.request("format", t0, 3821);
    CHECK(code == "4821");
    CHECK(gate.open_subject() == "format");

    CHECK(gate.confirm("format", "0000", t0 + 1) ==
          ConfirmGate::Result::wrong_code);
    CHECK(gate.confirm("delete 5", code, t0 + 1) ==
          ConfirmGate::Result::wrong_subject);
    CHECK(gate.confirm("format", code, t0 + 5000000) ==
          ConfirmGate::Result::confirmed);
    // Used up.
    CHECK(gate.confirm("format", code, t0 + 5000001) ==
          ConfirmGate::Result::no_request);
}

TEST_CASE("an old or guessed confirmation is dropped")
{
    ConfirmGate gate;
    const std::string code = gate.request("format", 0, 1);
    CHECK(gate.confirm("format", code, ConfirmGate::kValidityUs + 1) ==
          ConfirmGate::Result::expired);
    CHECK(gate.confirm("format", code, ConfirmGate::kValidityUs + 2) ==
          ConfirmGate::Result::no_request);

    const std::string next = gate.request("format", 0, 7);
    for (unsigned i = 0; i < ConfirmGate::kMaxWrongCodes; ++i) {
        CHECK(gate.confirm("format", "1", 10) ==
              ConfirmGate::Result::wrong_code);
    }
    // Three wrong codes ended it; the right one does not help any more.
    CHECK(gate.confirm("format", next, 11) == ConfirmGate::Result::no_request);

    // A new request replaces the old one.
    gate.request("delete 5", 0, 1);
    const std::string second = gate.request("format", 0, 2);
    CHECK(gate.open_subject() == "format");
    CHECK(gate.confirm("format", second, 1) == ConfirmGate::Result::confirmed);
}

TEST_CASE("codes always have four digits")
{
    ConfirmGate gate;
    for (std::uint32_t entropy : {0u, 1u, 8999u, 9000u, 4294967295u}) {
        const std::string code = gate.request("format", 0, entropy);
        CHECK(code.size() == 4);
    }
}
