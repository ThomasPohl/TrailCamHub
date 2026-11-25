// The camera session (camera_protocol.cpp and what it is built from) against
// the camera mock, over real UDP sockets on the loopback interface. These
// tests check what a session does as a whole: its result, what it stores on
// the "SD card", what it asks the camera for and how it ends when the camera
// misbehaves. They were written before the session was split up (#65) and
// run unchanged on the new structure.
//
// The same cases also run as session_virtual_tests (HOST_VIRTUAL_LINK, #354):
// on an in-process network with a virtual clock (virtual_net.hpp), without
// sleeping. There the cases that depend on time are exact, too.
//
// If the environment variable HOST_SESSION_SIGNATURE_DIR names a directory,
// every case also writes <case>.sig there: result, files on the card and the
// log without times, so two builds can be compared with diff.

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "camera_action.hpp"
#include "camera_actions.hpp"
#include "camera_protocol.h"
#include "host_esp.hpp"
#include "mock_server.hpp"
#ifdef HOST_VIRTUAL_LINK
#include "virtual_net.hpp"
#endif

namespace fs = std::filesystem;
namespace act = camera::action;

namespace
{

struct Outcome {
    esp_err_t error = ESP_FAIL;
    bool work_completed = false;
    bool media_list_error = false;
    uint32_t stored = 0;
    uint32_t listed = 0;
    std::string camera_id;
    int battery = -1;
    std::vector<std::string> list_lines;
    std::vector<std::string> progress;
};

std::vector<std::string> s_list_lines;
std::vector<std::string> s_progress;

void on_list(const char *line)
{
    s_list_lines.push_back(line);
}

void on_progress(uint32_t done, uint32_t stored, uint32_t total)
{
    s_progress.push_back(std::to_string(done) + "/" + std::to_string(stored) +
                         "/" + std::to_string(total));
}

// The state a test sets before a session; every case starts from this one.
void reset_world()
{
    host_esp::reset_sd_card();
    host_esp::clear_log();
    camera_protocol_set_list_only(false);
    camera_protocol_set_actions_only(false);
    camera_protocol_set_all_videos(false);
    camera_protocol_set_login(nullptr, nullptr);
    camera_protocol_set_list_callback(on_list);
    camera_protocol_set_progress_callback(on_progress);
    s_list_lines.clear();
    s_progress.clear();
#ifdef HOST_VIRTUAL_LINK
    // The same "random" packet loss every time.
    std::srand(1);
    virtual_net::reset();
#endif
}

Outcome run_session()
{
    esp_ip4_addr_t gateway = {};
    gateway.addr = htonl(INADDR_LOOPBACK);
    s_list_lines.clear();
    s_progress.clear();
    Outcome o;
    o.error = camera_protocol_run(&gateway);
    o.work_completed = camera_protocol_work_completed();
    o.media_list_error = camera_protocol_media_list_error();
    o.stored = camera_protocol_stored_items();
    o.listed = camera_protocol_listed_files();
    char id[64] = {};
    camera_protocol_last_camera_id(id, sizeof(id));
    o.camera_id = id;
    o.battery = camera_protocol_last_battery_percent();
    o.list_lines = s_list_lines;
    o.progress = s_progress;
    return o;
}

// Lines of the log that are the same in every run: no times, no speeds, and
// not the lines that depend on how fast the two sides happen to be.
std::string normalized_log()
{
    static const std::regex kVolatile(
        "heartbeat|Heartbeat|ALIVE|Retransmitted|Sent .*after .* without a "
        "slot|UDP |Received DATA|Received ARTEMIS DATA|Camera discovery "
        "reply|PRE_LOGIN response received|Login sets the camera clock");
    std::string out;
    for (const host_esp::LogLine &line : host_esp::log_lines()) {
        // ERROR 1, WARN 2, INFO 3; DEBUG lines are not part of it.
        if (line.level < 1 || line.level > 3 || line.tag == "trailcamhub_udp") {
            continue;
        }
        if (std::regex_search(line.text, kVolatile)) {
            continue;
        }
        std::string text = line.text;
        text = std::regex_replace(text, std::regex("[0-9]+ ms"), "N ms");
        text = std::regex_replace(text, std::regex("[0-9]+ KB/s"), "N KB/s");
        text = std::regex_replace(text, std::regex("token [0-9]+"), "token T");
        text = std::regex_replace(text, std::regex("[0-9]{9,}"), "T");
        // The DATA index counts the heartbeats sent so far, a matter of time.
        text = std::regex_replace(text, std::regex("index [0-9]+"), "index N");
        out += std::to_string(line.level) + " " + text + "\n";
    }
    return out;
}

// Every file on the card with its size, and the text of the small ones that
// say what the session decided (catalog, camera info, queue).
std::string card_snapshot()
{
    std::map<std::string, std::string> files;
    for (const auto &entry : fs::recursive_directory_iterator(HOST_SD_ROOT)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string name = fs::relative(entry.path(), HOST_SD_ROOT).string();
        std::string detail = std::to_string(entry.file_size()) + " bytes";
        const std::string leaf = entry.path().filename().string();
        if (leaf == "catalog.csv" || leaf == "camera.json" ||
            leaf == "actions.csv") {
            std::ifstream in(entry.path());
            std::stringstream text;
            text << in.rdbuf();
            std::string body = text.str();
            // Times of the day the test ran at.
            body = std::regex_replace(
                body, std::regex("\"(readAt|lastSyncAt)\":[0-9]+"), "\"$1\":T");
            body = std::regex_replace(body, std::regex(",[0-9]{10},[0-9]{10},"),
                                      ",T,T,");
            detail += "\n" + body;
        }
        files[name] = detail;
    }
    std::string out;
    for (const auto &[name, detail] : files) {
        out += name + ": " + detail + "\n";
    }
    return out;
}

size_t count_files(const std::string &directory_part, const char *extension)
{
    size_t count = 0;
    for (const auto &entry : fs::recursive_directory_iterator(HOST_SD_ROOT)) {
        if (entry.is_regular_file() &&
            entry.path().parent_path().filename() == directory_part &&
            entry.path().extension() == extension) {
            ++count;
        }
    }
    return count;
}

// The text of <case>.sig: what a case did, for comparing two builds. A case
// whose course depends on the time (a stop after 1.5 s, random packet loss)
// is `exact` = false in real time: only how it ended is written. On the
// virtual clock every case is exact.
std::string signature_text(const Outcome &o, const camera_mock::Stats &stats,
                           bool exact)
{
#ifdef HOST_VIRTUAL_LINK
    exact = true;
#endif
    std::ostringstream out;
    if (!exact) {
        out << "result: error=" << o.error << " completed=" << o.work_completed
            << " id=" << o.camera_id << "\n";
        return out.str();
    }
    out << "result: error=" << o.error << " completed=" << o.work_completed
        << " list_error=" << o.media_list_error << " stored=" << o.stored
        << " listed=" << o.listed << " id=" << o.camera_id
        << " battery=" << o.battery << "\n";
    out << "list lines:\n";
    for (const auto &l : o.list_lines) {
        out << "  " << l << "\n";
    }
    out << "progress:\n";
    for (const auto &p : o.progress) {
        out << "  " << p << "\n";
    }
    out << "mock requests:";
    for (const auto &[cmd, n] : stats.requests) {
        if (cmd != 525) {
            out << " " << cmd << "x" << n;
        }
    }
    out << "\ncard:\n" << card_snapshot();
    out << "log:\n" << normalized_log();
    return out.str();
}

void write_signature(const char *name, const Outcome &o,
                     const camera_mock::Stats &stats, bool exact = true)
{
    const char *dir = std::getenv("HOST_SESSION_SIGNATURE_DIR");
    if (dir == nullptr) {
        return;
    }
    fs::create_directories(dir);
    std::ofstream out(std::string(dir) + "/" + name + ".sig");
    out << signature_text(o, stats, exact);
}

// A mock with `media` recordings on its card.
camera_mock::Options options_with(size_t media)
{
    camera_mock::Options options;
    options.media_count = media;
    return options;
}

}  // namespace

TEST_CASE("session: scan of a small card stores thumbnails and photos")
{
    reset_world();
    host_session::MockServer mock(options_with(6));
    REQUIRE(mock.ok());
    const Outcome o = run_session();
    write_signature("scan_small", o, mock.stats());

    CHECK(o.error == ESP_OK);
    CHECK(o.work_completed);
    CHECK_FALSE(o.media_list_error);
    // 6 thumbnails and the 3 photos of the 6 recordings (the videos are not
    // downloaded in this build).
    CHECK(o.stored == 9);
    CHECK(o.camera_id == "C6_1E_0D_E0_0C_FB");
    CHECK(o.battery == 100);
    CHECK(count_files("thumb", ".jpg") == 6);
    CHECK(count_files("media", ".bin") + count_files("media", ".jpg") == 3);
    CHECK(fs::exists(std::string(HOST_SD_ROOT) +
                     "/C6_1E_0D_E0_0C_FB/catalog.csv"));
    CHECK(fs::exists(std::string(HOST_SD_ROOT) +
                     "/C6_1E_0D_E0_0C_FB/camera.json"));
    // The download list is announced, then each file is reported.
    REQUIRE_FALSE(o.progress.empty());
    CHECK(o.progress.front() == "0/0/3");
    CHECK(o.progress.back() == "3/3/3");
    const camera_mock::Stats stats = mock.stats();
    CHECK(stats.requests.at(512) == 1);
    CHECK(stats.requests.at(768) == 1);
    CHECK(stats.requests.at(772) == 6);
    CHECK(stats.requests.at(1285) == 3);
}

TEST_CASE("session: a second session on the same card fetches nothing new")
{
    reset_world();
    host_session::MockServer mock(options_with(6));
    REQUIRE(mock.ok());
    const Outcome first = run_session();
    REQUIRE(first.work_completed);
    host_esp::clear_log();
    const Outcome second = run_session();
    write_signature("scan_again", second, mock.stats());

    CHECK(second.error == ESP_OK);
    CHECK(second.work_completed);
    CHECK(second.stored == 0);
    CHECK(mock.stats().requests.at(772) == 6);
    CHECK(mock.stats().requests.at(1285) == 3);
}

TEST_CASE("session: a long list is read page by page")
{
    reset_world();
    host_session::MockServer mock(options_with(90));
    REQUIRE(mock.ok());
    const Outcome o = run_session();
    write_signature("scan_90", o, mock.stats());

    CHECK(o.error == ESP_OK);
    CHECK(o.work_completed);
    CHECK(count_files("thumb", ".jpg") == 90);
    CHECK(mock.stats().requests.at(768) >= 2);
}

TEST_CASE("session: an empty card is a finished session")
{
    reset_world();
    host_session::MockServer mock(options_with(0));
    REQUIRE(mock.ok());
    const Outcome o = run_session();
    write_signature("scan_empty", o, mock.stats());

    CHECK(o.error == ESP_OK);
    CHECK(o.work_completed);
    CHECK(o.stored == 0);
    CHECK(mock.stats().requests.count(772) == 0);
}

TEST_CASE("session: two cameras one after the other keep their own state")
{
    reset_world();
    host_session::MockServer mock(options_with(4));
    REQUIRE(mock.ok());
    const Outcome first = run_session();
    REQUIRE(first.work_completed);

    camera_mock::Identity other;
    other.dev_id[5] = 0x01;
    mock.set_identity(other);
    host_esp::clear_log();
    const Outcome second = run_session();
    write_signature("two_cameras", second, mock.stats());

    CHECK(second.error == ESP_OK);
    CHECK(second.work_completed);
    CHECK(second.camera_id == "C6_1E_0D_E0_0C_01");
    // The second camera's thumbnails are fetched although the first one's are
    // known: nothing of the first catalog leaks into the second session.
    CHECK(second.stored == first.stored);
    CHECK(fs::exists(std::string(HOST_SD_ROOT) + "/C6_1E_0D_E0_0C_01/thumb"));
    CHECK(fs::exists(std::string(HOST_SD_ROOT) + "/C6_1E_0D_E0_0C_FB/thumb"));
    CHECK(count_files("thumb", ".jpg") == 8);
}

TEST_CASE("session: listing only reads the list and touches nothing")
{
    reset_world();
    host_session::MockServer mock(options_with(5));
    REQUIRE(mock.ok());
    camera_protocol_set_list_only(true);
    const Outcome o = run_session();
    write_signature("list_only", o, mock.stats());

    CHECK(o.error == ESP_OK);
    CHECK(o.listed == 5);
    CHECK(o.stored == 0);
    REQUIRE_FALSE(o.list_lines.empty());
    CHECK(o.list_lines.back().find("list done") == 0);
    CHECK(mock.stats().requests.count(772) == 0);
    CHECK(count_files("thumb", ".jpg") == 0);
    CHECK_FALSE(fs::exists(std::string(HOST_SD_ROOT) +
                           "/C6_1E_0D_E0_0C_FB/catalog.csv"));
}

TEST_CASE("session: a queued photo is taken before the list is read")
{
    reset_world();
    act::service::init();
    const act::Action photo(act::Type::photo);
    const auto added =
        act::service::submit(photo, act::Source::console, "host-test-photo");
    REQUIRE(added.kind == act::ActionQueue::AddResult::Kind::added);
    host_session::MockServer mock(options_with(3));
    REQUIRE(mock.ok());
    const Outcome o = run_session();
    write_signature("action_photo", o, mock.stats());

    CHECK(o.error == ESP_OK);
    CHECK(o.work_completed);
    CHECK(mock.stats().photos == 1);
    CHECK(mock.stats().requests.at(641) == 1);
    CHECK_FALSE(act::service::has_pending());
}

TEST_CASE("session: a short session for actions reads no media")
{
    reset_world();
    act::service::init();
    REQUIRE(act::service::submit(act::Action(act::Type::photo),
                                 act::Source::console, "host-test-photo-2")
                .kind == act::ActionQueue::AddResult::Kind::added);
    host_session::MockServer mock(options_with(3));
    REQUIRE(mock.ok());
    camera_protocol_set_actions_only(true);
    const Outcome o = run_session();
    write_signature("actions_only", o, mock.stats());

    CHECK(o.error == ESP_OK);
    CHECK(o.work_completed);
    CHECK(o.stored == 0);
    CHECK(mock.stats().photos == 1);
    CHECK(mock.stats().requests.count(768) == 0);
    CHECK(mock.stats().requests.count(772) == 0);
}

TEST_CASE("session: a settings change goes to the camera and is stored")
{
    reset_world();
    act::service::init();
    // For this camera only (its directory name, as the backend's camId).
    const auto change = act::service::submit(
        act::Action(act::Type::settings, 0, "C6_1E_0D_E0_0C_FB",
                    "pirSens=0;languageCode=de"),
        act::Source::backend, "be:host-test-settings");
    REQUIRE(change.kind == act::ActionQueue::AddResult::Kind::added);
    // Not offered by the captured camera (supportSpkVolume false): refused
    // without a 513.
    const auto refused = act::service::submit(
        act::Action(act::Type::settings, 0, "C6_1E_0D_E0_0C_FB", "spkVol=10"),
        act::Source::backend, "be:host-test-settings-2");
    REQUIRE(refused.kind == act::ActionQueue::AddResult::Kind::added);
    host_session::MockServer mock(options_with(3));
    REQUIRE(mock.ok());
    camera_protocol_set_actions_only(true);
    const Outcome o = run_session();

    CHECK(o.error == ESP_OK);
    CHECK(o.work_completed);
    CHECK(mock.stats().settings_changes == 1);
    CHECK(mock.stats().requests.at(513) == 1);
    act::Entry entry;
    REQUIRE(act::service::find(change.id, entry));
    CHECK(entry.state == act::State::done);
    REQUIRE(act::service::find(refused.id, entry));
    CHECK(entry.state == act::State::failed);
    CHECK(entry.reason == "the camera does not support spkVol");

    // The stored copy has the new values for the next check-in.
    std::ifstream file(std::string(HOST_SD_ROOT) +
                       "/C6_1E_0D_E0_0C_FB/camera.json");
    const std::string stored((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    CHECK(stored.find("\"pirSens\":0") != std::string::npos);
    CHECK(stored.find("\"languageCode\":\"de\"") != std::string::npos);
    CHECK(stored.find("secureCode") == std::string::npos);
}

namespace
{
const char *const kCameraDir = "C6_1E_0D_E0_0C_FB";

// Names of the files in thumb/ or media/ of the test camera, sorted.
std::vector<std::string> files_in(const char *sub)
{
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(
             std::string(HOST_SD_ROOT) + "/" + kCameraDir + "/" + sub, ec)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::int64_t media_id_of(const std::string &file_name)
{
    return std::stoll(file_name.substr(0, file_name.find('.')));
}

// The hub has a thumbnail or a file of this recording.
bool has_files_of(std::int64_t media_id)
{
    const std::string prefix = std::to_string(media_id) + ".";
    for (const char *sub : {"thumb", "media"}) {
        for (const std::string &name : files_in(sub)) {
            if (name.compare(0, prefix.size(), prefix) == 0) {
                return true;
            }
        }
    }
    return false;
}

act::ActionQueue::AddResult submit_delete(std::int64_t media_id,
                                          std::uint8_t scope, const char *key)
{
    return act::service::submit(
        act::Action(act::Type::delete_media, media_id, kCameraDir, "", scope),
        act::Source::backend, key);
}
}  // namespace

TEST_CASE("session: a deletion applies to the camera, the hub or both (#461)")
{
    reset_world();
    act::service::init();
    {
        host_session::MockServer mock(options_with(6));
        REQUIRE(mock.ok());
        REQUIRE(run_session().work_completed);
    }
    // The three photos have a thumbnail and a file on the hub.
    const std::vector<std::string> photos = files_in("media");
    REQUIRE(photos.size() == 3);
    const std::int64_t both = media_id_of(photos[0]);
    const std::int64_t camera_only = media_id_of(photos[1]);
    const std::int64_t hub_only = media_id_of(photos[2]);
    REQUIRE(has_files_of(both));
    REQUIRE(has_files_of(camera_only));
    REQUIRE(has_files_of(hub_only));

    const auto a = submit_delete(both, act::kScopeBoth, "be:host-scope-1");
    const auto b =
        submit_delete(camera_only, act::kScopeCamera, "be:host-scope-2");
    const auto c = submit_delete(hub_only, act::kScopeHub, "be:host-scope-3");
    REQUIRE(a.kind == act::ActionQueue::AddResult::Kind::added);
    REQUIRE(b.kind == act::ActionQueue::AddResult::Kind::added);
    REQUIRE(c.kind == act::ActionQueue::AddResult::Kind::added);
    {
        host_session::MockServer mock(options_with(6));
        REQUIRE(mock.ok());
        camera_protocol_set_actions_only(true);
        const Outcome o = run_session();
        CHECK(o.error == ESP_OK);
        CHECK(o.work_completed);
        // The hub's own copies need no command to the camera.
        CHECK(mock.stats().deletions == 2);
        CHECK(mock.stats().requests.at(773) == 2);
    }
    act::Entry entry;
    for (const std::uint32_t id : {a.id, b.id, c.id}) {
        REQUIRE(act::service::find(id, entry));
        CHECK(entry.state == act::State::done);
    }
    CHECK_FALSE(has_files_of(both));
    CHECK(has_files_of(camera_only));
    CHECK_FALSE(has_files_of(hub_only));

    // The camera still lists the three (the mock replays its capture), but
    // what the hub deleted is not fetched again; nothing else is new.
    {
        host_session::MockServer mock(options_with(6));
        REQUIRE(mock.ok());
        camera_protocol_set_actions_only(false);
        const Outcome o = run_session();
        CHECK(o.error == ESP_OK);
        CHECK(o.work_completed);
        CHECK(o.stored == 0);
    }
    CHECK_FALSE(has_files_of(both));
    CHECK(has_files_of(camera_only));
    CHECK_FALSE(has_files_of(hub_only));
}

TEST_CASE(
    "session: a failed deletion on the camera leaves the hub's files (#461)")
{
    reset_world();
    act::service::init();
    {
        host_session::MockServer mock(options_with(6));
        REQUIRE(mock.ok());
        REQUIRE(run_session().work_completed);
    }
    const std::int64_t id = media_id_of(files_in("media")[0]);
    // A recording the camera does not list (any more): the camera part is
    // gone anyway, so the hub's files go and the person is told "done".
    const std::int64_t unknown = id + 1000000;
    const auto added =
        submit_delete(unknown, act::kScopeBoth, "be:host-scope-4");
    REQUIRE(added.kind == act::ActionQueue::AddResult::Kind::added);
    {
        host_session::MockServer mock(options_with(6));
        REQUIRE(mock.ok());
        camera_protocol_set_actions_only(true);
        REQUIRE(run_session().work_completed);
        CHECK(mock.stats().deletions == 0);
    }
    act::Entry entry;
    REQUIRE(act::service::find(added.id, entry));
    CHECK(entry.state == act::State::failed);
    CHECK(entry.reason.find("does not list this file") != std::string::npos);
    // Nothing else was touched.
    CHECK(has_files_of(id));
    camera_protocol_set_actions_only(false);
}

TEST_CASE("session: what the camera lost itself is deleted on the hub (#461)")
{
    reset_world();
    {
        host_session::MockServer mock(options_with(6));
        REQUIRE(mock.ok());
        REQUIRE(run_session().work_completed);
    }
    REQUIRE(files_in("thumb").size() == 6);
    // The camera's card lost the last three recordings (deleted there, or
    // formatted and filled again).
    {
        host_session::MockServer mock(options_with(3));
        REQUIRE(mock.ok());
        const Outcome o = run_session();
        CHECK(o.error == ESP_OK);
        CHECK(o.work_completed);
    }
    const std::vector<std::string> thumbs = files_in("thumb");
    CHECK(thumbs.size() == 3);
    // What is left belongs to the recordings the camera still lists.
    for (const std::string &name : files_in("media")) {
        CHECK(has_files_of(media_id_of(name)));
        CHECK(std::find(thumbs.begin(), thumbs.end(),
                        std::to_string(media_id_of(name)) + ".jpg") !=
              thumbs.end());
    }
}

TEST_CASE("session: the camera closing the session ends it")
{
    reset_world();
    host_session::MockServer mock(options_with(400));
    REQUIRE(mock.ok());
    host_session::Later closer(1500, [&mock] { mock.close_session(); });
    const Outcome o = run_session();
    closer.join();
    write_signature("camera_closes", o, mock.stats(), false);

    CHECK(o.error == ESP_ERR_INVALID_STATE);
    CHECK_FALSE(o.work_completed);
    // What was stored before is kept, and the catalog is saved.
    CHECK(o.stored > 0);
    CHECK(fs::exists(std::string(HOST_SD_ROOT) +
                     "/C6_1E_0D_E0_0C_FB/catalog.csv"));
}

TEST_CASE("session: a stop request ends it and keeps what was stored")
{
    reset_world();
    host_session::MockServer mock(options_with(400));
    REQUIRE(mock.ok());
    host_session::Later stopper(1500, [] { camera_protocol_request_stop(); });
    const Outcome o = run_session();
    stopper.join();
    write_signature("stop_requested", o, mock.stats(), false);

    CHECK(o.error == ESP_OK);
    CHECK_FALSE(o.work_completed);
    CHECK(o.stored > 0);
}

TEST_CASE("session: a camera that goes silent after the login times out")
{
    reset_world();
    host_session::MockServer mock(options_with(3));
    REQUIRE(mock.ok());
    camera_mock::Faults faults;
    faults.mute = true;
    mock.set_faults(faults);
    const Outcome o = run_session();
    write_signature("mute", o, mock.stats());

    CHECK(o.error == ESP_ERR_TIMEOUT);
    CHECK_FALSE(o.work_completed);
    CHECK(o.stored == 0);
}

TEST_CASE("session: records that stop mid-transfer are given up on")
{
    reset_world();
    host_session::MockServer mock(options_with(1));
    REQUIRE(mock.ok());
    camera_mock::Faults faults;
    faults.abort_after_frames = 2;
    mock.set_faults(faults);
    const Outcome o = run_session();
    write_signature("abort_records", o, mock.stats());

    // The session does not hang: it gives the thumbnail and the photo up and
    // closes; nothing complete was stored.
    CHECK(o.error == ESP_OK);
    CHECK(o.work_completed);
    CHECK(o.stored == 0);
    CHECK(count_files("thumb", ".jpg") == 0);
}

TEST_CASE("session: lost and doubled packets do not change the result")
{
    reset_world();
    host_session::MockServer mock(options_with(8));
    REQUIRE(mock.ok());
    camera_mock::Faults faults;
    faults.drop_percent = 10;
    faults.duplicate_percent = 10;
    mock.set_faults(faults);
    const Outcome o = run_session();
    write_signature("lossy", o, mock.stats(), false);

    CHECK(o.error == ESP_OK);
    CHECK(o.work_completed);
    CHECK(count_files("thumb", ".jpg") == 8);
}

#ifdef HOST_VIRTUAL_LINK
namespace
{

// One of the cases that depend on time, run from scratch; the text of what
// it did.
std::string timed_case(int which)
{
    reset_world();
    host_session::MockServer mock(options_with(which == 3 ? 8 : 400));
    camera_mock::Faults faults;
    std::unique_ptr<host_session::Later> later;
    if (which == 0) {
        later.reset(
            new host_session::Later(1500, [&mock] { mock.close_session(); }));
    } else if (which == 1) {
        later.reset(new host_session::Later(
            1500, [] { camera_protocol_request_stop(); }));
    } else if (which == 2) {
        faults.mute = true;
    } else {
        faults.drop_percent = 10;
        faults.duplicate_percent = 10;
    }
    mock.set_faults(faults);
    const Outcome o = run_session();
    return signature_text(o, mock.stats(), true);
}

}  // namespace

TEST_CASE("virtual session: the cases that depend on time repeat exactly")
{
    // Camera closes, stop request, silent camera, lossy link: the second run
    // of each is the same as the first, down to the log.
    for (int which = 0; which < 4; ++which) {
        const std::string first = timed_case(which);
        const std::string second = timed_case(which);
        INFO("case " << which);
        if (first != second) {
            // For finding out what differs: diff <dir>/timed_N_*.sig.
            const char *dir = std::getenv("HOST_SESSION_SIGNATURE_DIR");
            if (dir != nullptr) {
                fs::create_directories(dir);
                std::ofstream(std::string(dir) + "/timed_" +
                              std::to_string(which) + "_first.sig")
                    << first;
                std::ofstream(std::string(dir) + "/timed_" +
                              std::to_string(which) + "_second.sig")
                    << second;
            }
        }
        CHECK(first == second);
        CHECK_FALSE(first.empty());
    }
}

TEST_CASE("virtual session: waiting costs no real time")
{
    // The silent camera case waits out the session's timeouts: many seconds
    // of virtual time.
    const auto started = std::chrono::steady_clock::now();
    timed_case(2);
    const auto real_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    const std::int64_t virtual_ms = virtual_net::elapsed_us() / 1000;
    CHECK(virtual_ms >= 1000);
    CHECK(real_ms < virtual_ms);
}
#endif
