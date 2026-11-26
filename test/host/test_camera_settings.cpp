#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "action_queue.hpp"
#include "camera_action.hpp"
#include "camera_settings.hpp"
#include "delete_request.hpp"
#include "device_info.hpp"
#include "uplink_protocol.hpp"

using namespace camera::settings;

namespace
{

// The device info of the mock's capture (PH970S, fw 2.3.6.5), shortened:
// the settings, the screen lock, the clock and the whole capability.
const std::string kDeviceInfo =
    "{\"devId\":\"C6:1E:0D:E0:0C:FB\",\"fwVerName\":\"2.3.6.5\","
    "\"modelName\":\"PH970S\",\"customName\":\"Zecre\",\"cameraNameOnOff\":0,"
    "\"cameraName\":\"0000\",\"longitude\":\"\",\"latitude\":\"\","
    "\"pwrFreq\":0,\"infraRedLum\":3,\"timeUTC\":1770401530,"
    "\"osdInfoEnable\":true,\"WiFiLedEnable\":true,\"batPercent\":100,"
    "\"workMode\":0,\"timelapseInterval\":3600,\"shootMode\":2,"
    "\"picResolution\":6,\"videoResolution\":2,\"continuousShootCnt\":1,"
    "\"recordDuration\":10,\"recordAudio\":1,\"dateFormat\":1,"
    "\"timeFormat\":0,\"languageCode\":\"en\",\"workTimeOnOff\":0,"
    "\"workTimeStartHour\":0,\"workTimeStartMin\":0,\"workTimeEndHour\":23,"
    "\"workTimeEndMin\":59,\"pirSens\":2,\"pirCoolingTime\":20,"
    "\"sidePirOnOff\":1,\"enableAiFilter\":0,\"detectionAreaVertex0X\":0,"
    "\"detectionAreaVertex0Y\":0,\"detectionAreaVertex1X\":0,"
    "\"detectionAreaVertex1Y\":0,\"detectionAreaVertex2X\":0,"
    "\"detectionAreaVertex2Y\":0,\"detectionAreaVertex3X\":0,"
    "\"detectionAreaVertex3Y\":0,\"secureCodeOnOff\":1,\"secureCode0\":4,"
    "\"secureCode1\":7,\"secureCode2\":1,\"secureCode3\":9,"
    "\"sdTotalMB\":29998,\"sdFreeMB\":20617,\"recordMode\":0,\"spkVol\":80,"
    "\"micVol\":90,\"lum\":0,"
    "\"capability\":{\"platform\":\"T23ZN\",\"languages\":[{\"langCode\":"
    "\"en\"},{\"langCode\":\"de\"},{\"langCode\":\"fr\"}],"
    "\"imgResolutions\":[{\"resolution\":1},{\"resolution\":2},"
    "{\"resolution\":6},{\"resolution\":10}],\"videoResolutions\":["
    "{\"resolution\":1},{\"resolution\":2},{\"resolution\":4}],"
    "\"enableAiFilter\":true,\"supportSidePir\":false,"
    "\"dispDetailVideoResolution\":false,\"supportScreen\":false,"
    "\"supportLockScreen\":false,\"supportMicVolume\":true,"
    "\"supportSpkVolume\":false,\"supportOsdInfo\":true,"
    "\"supportWiFiLED\":true,\"supportInfraRed\":true,"
    "\"supportAutoInfraRed\":false,\"supportOnlineService\":true},"
    "\"errorMsg\":\"Success\",\"result\":0,\"cmdId\":512}";

std::int64_t number_of(const Settings &settings, const std::string &name)
{
    const Value *value = settings.find(name);
    REQUIRE(value != nullptr);
    return value->number;
}

std::vector<Value> change_of(const std::string &object)
{
    std::vector<Value> change;
    std::string error;
    REQUIRE_MESSAGE(parse_change(object, change, error), error);
    return change;
}

}  // namespace

TEST_CASE("parse_settings reads the allowlisted settings and the capability")
{
    const Settings settings = parse_settings(kDeviceInfo);
    CHECK(number_of(settings, "workMode") == 0);
    CHECK(number_of(settings, "shootMode") == 2);
    CHECK(number_of(settings, "picResolution") == 6);
    CHECK(number_of(settings, "workTimeEndHour") == 23);
    CHECK(number_of(settings, "osdInfoEnable") == 1);
    CHECK(number_of(settings, "micVol") == 90);
    // The setting, not the capability flag of the same name.
    CHECK(number_of(settings, "enableAiFilter") == 0);
    REQUIRE(settings.find("languageCode") != nullptr);
    CHECK(settings.find("languageCode")->text == "en");

    // Never the screen lock, the clock or the unknown fields.
    for (const char *name :
         {"secureCodeOnOff", "secureCode0", "secureCode3", "timeUTC", "utcTime",
          "recordMode", "lum", "pwrFreq", "longitude", "cameraNameOnOff",
          "customName"}) {
        CHECK(settings.find(name) == nullptr);
    }

    const Capability &cap = settings.capability;
    CHECK(cap.present);
    CHECK(cap.image == std::vector<std::int64_t>{1, 2, 6, 10});
    CHECK(cap.video == std::vector<std::int64_t>{1, 2, 4});
    CHECK(cap.languages == std::vector<std::string>{"en", "de", "fr"});
    CHECK(cap.unsupported ==
          std::vector<std::string>{"supportSidePir", "supportSpkVolume"});
    CHECK_FALSE(cap.supports(*find_field("sidePirOnOff")));
    CHECK(cap.supports(*find_field("micVol")));
    CHECK(cap.supports(*find_field("workMode")));
}

TEST_CASE("a device info without settings has none")
{
    const Settings settings =
        parse_settings("{\"devId\":\"X\",\"batPercent\":5}");
    CHECK(settings.empty());
    CHECK(stored_fields(settings).empty());
    CHECK(report_fields(settings).empty());
}

TEST_CASE("the stored settings read back the same and hold no secret")
{
    const Settings settings = parse_settings(kDeviceInfo);
    const std::string stored =
        "{\"devId\":\"X\"" + stored_fields(settings) + "}";
    CHECK(stored.find("secureCode") == std::string::npos);
    CHECK(stored.find("timeUTC") == std::string::npos);

    const Settings back = parse_settings(stored);
    REQUIRE(back.values.size() == settings.values.size());
    for (std::size_t i = 0; i < back.values.size(); i++) {
        CHECK(back.values[i].name == settings.values[i].name);
        CHECK(back.values[i].number == settings.values[i].number);
        CHECK(back.values[i].text == settings.values[i].text);
    }
    CHECK(back.capability.image == settings.capability.image);
    CHECK(back.capability.video == settings.capability.video);
    CHECK(back.capability.languages == settings.capability.languages);
    CHECK(back.capability.unsupported == settings.capability.unsupported);
}

TEST_CASE("the device info keeps the settings in camera.json")
{
    camera::artemis::DeviceInfo info;
    REQUIRE(camera::artemis::parse_device_info(kDeviceInfo, info));
    CHECK(number_of(info.settings, "pirCoolingTime") == 20);
    const std::string stored = camera::artemis::serialize_device_info(info);
    CHECK(stored.find("secureCode") == std::string::npos);
    camera::artemis::DeviceInfo back;
    REQUIRE(camera::artemis::parse_device_info(stored, back));
    CHECK(back.dev_id == info.dev_id);
    CHECK(back.camera_name == "0000");
    CHECK(number_of(back.settings, "pirCoolingTime") == 20);
    CHECK(back.settings.capability.present);
}

TEST_CASE("the check-in form leaves out what the camera does not support")
{
    const std::string report = report_fields(parse_settings(kDeviceInfo));
    CHECK(report.rfind("\"settings\":{\"workMode\":0,\"shootMode\":2,", 0) ==
          0);
    CHECK(report.find("\"osdInfoEnable\":true") != std::string::npos);
    CHECK(report.find("\"languageCode\":\"en\"") != std::string::npos);
    CHECK(report.find("sidePirOnOff") == std::string::npos);
    CHECK(report.find("spkVol") == std::string::npos);
    CHECK(report.find("secureCode") == std::string::npos);
    CHECK(report.find("\"capability\":{\"imgResolutions\":[1,2,6,10],"
                      "\"videoResolutions\":[1,2,4],"
                      "\"languages\":[\"en\",\"de\",\"fr\"]}") !=
          std::string::npos);
}

TEST_CASE("the console shows the settings in the words camset takes (#197)")
{
    const Settings settings = parse_settings(kDeviceInfo);
    const std::string line = console_line(settings);
    CHECK(line.rfind("workMode=0 shootMode=2 ", 0) == 0);
    CHECK(line.find("osdInfoEnable=1") != std::string::npos);
    CHECK(line.find("languageCode=en") != std::string::npos);
    CHECK(line.find("sidePirOnOff") == std::string::npos);
    CHECK(line.find(';') == std::string::npos);
    CHECK(capability_line(settings.capability) ==
          "images=1,2,6,10 videos=1,2,4 languages=en,de,fr "
          "unsupported=supportSidePir,supportSpkVolume");
    CHECK(console_line(Settings{}).empty());
    CHECK(capability_line(Capability{}).empty());

    // What the console reads back is the object request_settings() takes.
    std::vector<Value> change;
    REQUIRE(decode_change("pirSens=2;osdInfoEnable=0;languageCode=de", change));
    CHECK(change_object(change) ==
          "{\"pirSens\":2,\"osdInfoEnable\":false,\"languageCode\":\"de\"}");
    std::vector<Value> parsed;
    std::string error;
    CHECK(parse_change(change_object(change), parsed, error));
    CHECK(parsed.size() == 3);
}

TEST_CASE("parse_change reads a flat object of known fields")
{
    const std::vector<Value> change = change_of(
        "{\"pirSens\":1, \"osdInfoEnable\":false,\"languageCode\":\"de\","
        "\"workMode\":2,\"WiFiLedEnable\":1}");
    // In the order of the table.
    REQUIRE(change.size() == 5);
    CHECK(change[0].name == "workMode");
    CHECK(change[0].number == 2);
    CHECK(change[1].name == "pirSens");
    CHECK(change[2].name == "osdInfoEnable");
    CHECK(change[2].number == 0);
    CHECK(change[3].name == "WiFiLedEnable");
    CHECK(change[3].number == 1);
    CHECK(change[4].text == "de");
}

TEST_CASE("parse_change refuses what the hub must not send")
{
    std::vector<Value> change;
    std::string error;
    CHECK_FALSE(parse_change("{\"secureCode0\":1}", change, error));
    CHECK(error == "the hub does not change secureCode0");
    CHECK_FALSE(parse_change("{\"utcTime\":1795338335}", change, error));
    CHECK_FALSE(parse_change("{\"securityCodeOnOff\":0}", change, error));
    CHECK_FALSE(parse_change("{}", change, error));
    CHECK(error == "no setting to change");
    CHECK_FALSE(parse_change("", change, error));
    CHECK_FALSE(parse_change("{\"workMode\":\"1\"}", change, error));
    CHECK(error == "the value of workMode has the wrong type");
    CHECK_FALSE(parse_change("{\"workMode\":true}", change, error));
    CHECK_FALSE(parse_change("{\"osdInfoEnable\":2}", change, error));
    CHECK_FALSE(parse_change("{\"languageCode\":3}", change, error));
    CHECK_FALSE(parse_change("{\"workMode\":1.5}", change, error));
    CHECK_FALSE(parse_change("{\"workMode\":{\"a\":1}}", change, error));
    CHECK_FALSE(parse_change("{\"workMode\":1,\"workMode\":2}", change, error));
    CHECK(error == "workMode is given twice");
    CHECK_FALSE(parse_change("{\"workMode\":1", change, error));
    CHECK(change.empty());
}

TEST_CASE("check_change checks ranges and the camera's capability")
{
    const Settings current = parse_settings(kDeviceInfo);
    CHECK(check_change(change_of("{\"workMode\":1,\"picResolution\":10}"),
                       current) == "");
    CHECK(check_change(change_of("{\"workMode\":3}"), current) ==
          "workMode must be 0 to 2");
    CHECK(check_change(change_of("{\"continuousShootCnt\":0}"), current) ==
          "continuousShootCnt must be 1 to 5");
    CHECK(check_change(change_of("{\"picResolution\":3}"), current) ==
          "the camera does not offer picResolution 3");
    CHECK(check_change(change_of("{\"videoResolution\":5}"), current) ==
          "the camera does not offer videoResolution 5");
    CHECK(check_change(change_of("{\"spkVol\":50}"), current) ==
          "the camera does not support spkVol");
    CHECK(check_change(change_of("{\"languageCode\":\"jp\"}"), current) ==
          "the camera does not offer the language jp");
    CHECK(check_change(change_of("{\"languageCode\":\"de\"}"), current) == "");
    CHECK(check_change(change_of("{\"detectionAreaVertex2Y\":1001}"),
                       current) == "detectionAreaVertex2Y must be 0 to 1000");

    // Without a device info only the ranges count.
    CHECK(check_change(change_of("{\"picResolution\":3,\"spkVol\":50}"),
                       Settings{}) == "");
    CHECK(check_change({}, current) == "no setting to change");
}

TEST_CASE("the 513 fields and the queue form of a change")
{
    const std::vector<Value> change = change_of(
        "{\"languageCode\":\"de\",\"osdInfoEnable\":true,\"workMode\":1}");
    CHECK(request_fields(change) ==
          ",\"workMode\":1,\"osdInfoEnable\":true,\"languageCode\":\"de\"");
    const std::string encoded = encode_change(change);
    CHECK(encoded == "workMode=1;osdInfoEnable=1;languageCode=de");
    CHECK(encoded.find(',') == std::string::npos);

    std::vector<Value> back;
    REQUIRE(decode_change(encoded, back));
    CHECK(request_fields(back) == request_fields(change));

    CHECK_FALSE(decode_change("", back));
    CHECK_FALSE(decode_change("workMode", back));
    CHECK_FALSE(decode_change("workMode=", back));
    CHECK_FALSE(decode_change("workMode=x", back));
    CHECK_FALSE(decode_change("secureCode0=1", back));
    CHECK_FALSE(decode_change("languageCode=d\"e", back));
}

TEST_CASE("apply_change updates the held settings")
{
    Settings settings = parse_settings(kDeviceInfo);
    apply_change(settings,
                 change_of("{\"pirSens\":0,\"languageCode\":\"fr\"}"));
    CHECK(number_of(settings, "pirSens") == 0);
    CHECK(settings.find("languageCode")->text == "fr");
    CHECK(number_of(settings, "workMode") == 0);

    Settings empty;
    apply_change(empty, change_of("{\"micVol\":40,\"workMode\":1}"));
    REQUIRE(empty.values.size() == 2);
    CHECK(empty.values[0].name == "workMode");
}

TEST_CASE("a settings change is an action of its own")
{
    using namespace camera::action;
    CHECK(cmd_id(Type::settings) == 513);
    CHECK(type_of_cmd(513) == Type::settings);
    CHECK(parse_type("settings") == Type::settings);
    CHECK(allowed(Type::settings, Source::backend));
    CHECK_FALSE(needs_confirmation(Type::settings));
    CHECK_FALSE(needs_media_list(Type::settings));

    CHECK(validate(Action{Type::settings, 0, "CAM", "pirSens=1"}) == "");
    CHECK(validate(Action{Type::settings, 0, "CAM", ""}) ==
          "the settings change cannot be read");
    CHECK(validate(Action{Type::settings, 0, "CAM", "pirSens=7"}) ==
          "pirSens must be 0 to 2");
    CHECK(validate(Action{Type::settings, 5, "CAM", "pirSens=1"}) ==
          "only a deletion takes a mediaId");
    CHECK(validate(Action{Type::photo, 0, "", "pirSens=1"}) ==
          "only a settings change takes parameters");

    camera::artemis::DeviceInfo info;
    REQUIRE(camera::artemis::parse_device_info(kDeviceInfo, info));
    CHECK(check_camera(Action{Type::settings, 0, "CAM", "pirSens=1"}, info) ==
          "");
    CHECK(check_camera(Action{Type::settings, 0, "CAM", "spkVol=10"}, info) ==
          "the camera does not support spkVol");

    CHECK(request_fields(Action{Type::settings, 0, "CAM", "pirSens=1"},
                         std::nullopt) == ",\"pirSens\":1");

    const Outcome done =
        interpret(Type::settings,
                  "{\"errorMsg\":\"Success\",\"result\":0,\"cmdId\":513}");
    CHECK(done.done);
    const Outcome refused =
        interpret(Type::settings,
                  "{\"errorMsg\":\"Failed\",\"result\":-1,\"cmdId\":513}");
    CHECK_FALSE(refused.done);
    CHECK(refused.reason == "the camera answered result -1: Failed");
}

TEST_CASE("the queue keeps a settings change across a restart (v3)")
{
    using namespace camera::action;
    using Kind = ActionQueue::AddResult::Kind;
    ActionQueue queue;
    CHECK(queue
              .add(Action{Type::settings, 0, "CAMA", "workMode=1;pirSens=2"},
                   Source::backend, "be:s1", 1)
              .kind == Kind::added);
    CHECK(queue
              .add(Action{Type::delete_media, 7, "CAMA"}, Source::backend,
                   "be:d1", 1)
              .kind == Kind::added);
    // A second change while the first waits is queued too: both run.
    CHECK(queue
              .add(Action{Type::settings, 0, "CAMA", "workMode=1;pirSens=2"},
                   Source::backend, "be:s2", 2)
              .kind == Kind::added);
    // The same command again is recognised by its key.
    CHECK(queue
              .add(Action{Type::settings, 0, "CAMA", "workMode=1;pirSens=2"},
                   Source::backend, "be:s1", 3)
              .kind == Kind::duplicate);

    const std::string text = queue.serialize();
    CHECK(text.rfind("v4,", 0) == 0);
    ActionQueue loaded;
    REQUIRE(loaded.parse(text));
    REQUIRE(loaded.entries().size() == 3);
    CHECK(loaded.entries()[0].action.type == Type::settings);
    CHECK(loaded.entries()[0].action.params == "workMode=1;pirSens=2");
    CHECK(loaded.entries()[0].action.cam_id == "CAMA");
    CHECK(loaded.entries()[1].action.params.empty());
    CHECK(describe(loaded.entries()[0]) ==
          "settings workMode=1;pirSens=2: pending");
}

TEST_CASE("a v2 file (without settings) is still read")
{
    camera::action::ActionQueue queue;
    REQUIRE(queue.parse("v2,3\n"
                        "1,delete,42,backend,pending,10,0,0,be:x,CAM,\n"
                        "2,photo,0,console,failed,10,11,1,,,snapRet -2\n"));
    REQUIRE(queue.entries().size() == 2);
    CHECK(queue.entries()[0].action.cam_id == "CAM");
    CHECK(queue.entries()[0].action.params.empty());
    CHECK(queue.entries()[1].reason == "snapRet -2");
}

TEST_CASE("a setSettings command is answered from the queue")
{
    using namespace camera::action;
    using Kind = ActionQueue::AddResult::Kind;
    ActionQueue::AddResult added;
    added.kind = Kind::added;
    uplink::DeleteDecision d = uplink::decide_settings(added, nullptr);
    CHECK_FALSE(d.answer);
    CHECK(d.read_needed);

    added.kind = Kind::invalid;
    added.reason = "pirSens must be 0 to 2";
    d = uplink::decide_settings(added, nullptr);
    CHECK(d.answer);
    CHECK_FALSE(d.done);
    CHECK(d.error == "pirSens must be 0 to 2");

    added.kind = Kind::duplicate;
    Entry entry;
    entry.state = State::done;
    d = uplink::decide_settings(added, &entry);
    CHECK(d.answer);
    CHECK(d.done);

    // Unlike a deletion, "not listed" is no success here.
    entry.state = State::failed;
    entry.reason = "the camera does not list this file any more";
    d = uplink::decide_settings(added, &entry);
    CHECK(d.answer);
    CHECK_FALSE(d.done);

    entry.state = State::pending;
    d = uplink::decide_settings(added, &entry);
    CHECK_FALSE(d.answer);
    CHECK_FALSE(d.read_needed);
}

TEST_CASE("the check-in carries the settings and reads setSettings")
{
    uplink::CheckinRequest request;
    uplink::CameraReport camera;
    camera.cam_id = "CAM";
    camera.settings = report_fields(parse_settings(kDeviceInfo));
    request.cameras.push_back(camera);
    const std::string body = uplink::build_checkin_json(request);
    CHECK(body.find(",\"settings\":{\"workMode\":0,") != std::string::npos);
    CHECK(body.find("\"capability\":{\"imgResolutions\":[1,2,6,10]") !=
          std::string::npos);
    CHECK(body.find("secureCode") == std::string::npos);

    uplink::CheckinResponse response;
    REQUIRE(uplink::parse_checkin_response(
        "{\"serverTime\":1,\"cameras\":[{\"camId\":\"CAM\",\"accepted\":true,"
        "\"resync\":false,\"thumbUploads\":[],\"commands\":[{\"id\":\"c1\","
        "\"type\":\"setSettings\",\"settings\":{\"pirSens\":1,"
        "\"osdInfoEnable\":false}}]}],\"config\":{}}",
        response));
    REQUIRE(response.cameras.size() == 1);
    REQUIRE(response.cameras[0].commands.size() == 1);
    const uplink::Command &command = response.cameras[0].commands[0];
    CHECK(command.type == "setSettings");
    CHECK(command.id == "c1");
    CHECK(command.settings == "{\"pirSens\":1,\"osdInfoEnable\":false}");
    CHECK(encode_change(change_of(command.settings)) ==
          "pirSens=1;osdInfoEnable=0");
}
