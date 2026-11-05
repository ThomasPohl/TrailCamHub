#include <doctest/doctest.h>

#include <string>

#include "session_fsm.hpp"

using namespace session;

namespace
{

// BLE timeout, Wi-Fi join timeout, retry delay, attempts.
const Config kConfig = {90000, 30000, 5000, 3};

bool has(const Result &result, Action action)
{
    return (result.actions & action) != 0;
}

// Brings a state machine into the camera session state.
void reach_session(Fsm &fsm)
{
    fsm.handle(Event::scan_requested);
    fsm.handle(Event::credentials_received);
    fsm.handle(Event::wifi_connected);
    REQUIRE(fsm.state() == State::session);
}

}  // namespace

TEST_CASE("the hub stays idle until a scan is requested")
{
    Fsm fsm(kConfig);
    CHECK(fsm.state() == State::idle);
    CHECK_FALSE(fsm.busy());
    CHECK_FALSE(fsm.handle(Event::timeout).handled);
    CHECK_FALSE(fsm.handle(Event::credentials_received).handled);
    CHECK(fsm.state() == State::idle);
}

TEST_CASE("scan without uplink: camera session, then back to idle")
{
    Fsm fsm(kConfig);

    Result result = fsm.handle(Event::scan_requested);
    CHECK(result.next == State::ble_activation);
    CHECK(has(result, kResumeBle));
    CHECK(has(result, kStartTimer));
    CHECK(result.timer_ms == 90000);
    CHECK(fsm.attempt() == 1);

    result = fsm.handle(Event::credentials_received);
    CHECK(result.next == State::wifi_join);
    CHECK(has(result, kPauseBle));
    CHECK(has(result, kConnectCameraWifi));
    CHECK(result.timer_ms == 30000);

    result = fsm.handle(Event::wifi_connected);
    CHECK(result.next == State::session);
    CHECK(has(result, kCancelTimer));
    CHECK(has(result, kStartSession));

    result = fsm.handle(Event::session_ended);
    CHECK(result.next == State::idle);
    CHECK(has(result, kDisconnectWifi));
    CHECK(result.outcome == Outcome::camera_scanned);
    CHECK(fsm.attempt() == 0);
}

TEST_CASE("scan with uplink: camera session, home Wi-Fi, backend sync")
{
    Fsm fsm(kConfig);
    fsm.set_uplink_enabled(true);
    reach_session(fsm);

    Result result = fsm.handle(Event::session_ended);
    CHECK(result.next == State::uplink_join);
    CHECK(has(result, kDisconnectWifi));
    CHECK(has(result, kConnectHomeWifi));
    CHECK(has(result, kStartTimer));
    CHECK(result.outcome == Outcome::none);

    result = fsm.handle(Event::wifi_connected);
    CHECK(result.next == State::uplink);
    CHECK(has(result, kStartUplink));
    CHECK(has(result, kCancelTimer));

    result = fsm.handle(Event::uplink_done);
    CHECK(result.next == State::idle);
    CHECK(has(result, kDisconnectWifi));
    CHECK(result.outcome == Outcome::uplink_done);
}

TEST_CASE("upload only skips the camera")
{
    Fsm fsm(kConfig);
    CHECK_FALSE(fsm.handle(Event::upload_requested).handled);

    fsm.set_uplink_enabled(true);
    const Result result = fsm.handle(Event::upload_requested);
    CHECK(result.next == State::uplink_join);
    CHECK(has(result, kConnectHomeWifi));
    CHECK_FALSE(has(result, kResumeBle));
}

TEST_CASE("camera failures are retried, then the cycle gives up")
{
    for (Event failure : {Event::timeout, Event::wifi_failed}) {
        Fsm fsm(kConfig);
        fsm.handle(Event::scan_requested);
        for (std::uint32_t attempt = 1; attempt < 3; attempt++) {
            fsm.handle(Event::credentials_received);
            Result result = fsm.handle(failure);
            CHECK(result.next == State::backoff);
            CHECK(has(result, kDisconnectWifi));
            CHECK(result.timer_ms == 5000);

            result = fsm.handle(Event::timeout);
            CHECK(result.next == State::ble_activation);
            CHECK(has(result, kResumeBle));
            CHECK(fsm.attempt() == attempt + 1);
        }
        fsm.handle(Event::credentials_received);
        const Result result = fsm.handle(failure);
        CHECK(result.next == State::idle);
        CHECK(result.outcome == Outcome::camera_unreachable);
    }
}

TEST_CASE("BLE activation timeout counts as a failed attempt")
{
    Fsm fsm({90000, 30000, 5000, 1});
    fsm.handle(Event::scan_requested);
    const Result result = fsm.handle(Event::timeout);
    CHECK(result.next == State::idle);
    CHECK(has(result, kPauseBle));
    CHECK(result.outcome == Outcome::camera_unreachable);
}

TEST_CASE("an unreachable home Wi-Fi ends the cycle")
{
    for (Event failure : {Event::timeout, Event::wifi_failed}) {
        Fsm fsm(kConfig);
        fsm.set_uplink_enabled(true);
        reach_session(fsm);
        fsm.handle(Event::session_ended);
        const Result result = fsm.handle(failure);
        CHECK(result.next == State::idle);
        CHECK(has(result, kDisconnectWifi));
        CHECK(result.outcome == Outcome::uplink_unreachable);
    }
}

TEST_CASE("Wi-Fi loss during a session stops it before moving on")
{
    Fsm fsm(kConfig);
    reach_session(fsm);

    Result result = fsm.handle(Event::wifi_disconnected);
    CHECK(result.handled);
    CHECK(result.next == State::session);
    CHECK(has(result, kStopSession));
    CHECK(has(result, kDisconnectWifi));

    result = fsm.handle(Event::session_ended);
    CHECK(result.next == State::idle);
}

TEST_CASE("requests while busy are ignored")
{
    Fsm fsm(kConfig);
    reach_session(fsm);
    CHECK(fsm.busy());
    CHECK_FALSE(fsm.handle(Event::scan_requested).handled);
    CHECK_FALSE(fsm.handle(Event::upload_requested).handled);
    // New credentials from a BLE reconnect must not disturb the session.
    CHECK_FALSE(fsm.handle(Event::credentials_received).handled);
    CHECK_FALSE(fsm.handle(Event::timeout).handled);
    CHECK(fsm.state() == State::session);
}

TEST_CASE("zero attempts are treated as one")
{
    Fsm fsm({90000, 30000, 5000, 0});
    fsm.handle(Event::scan_requested);
    CHECK(fsm.handle(Event::timeout).next == State::idle);
}

TEST_CASE("state, event and outcome names")
{
    CHECK(std::string(to_string(State::uplink_join)) == "UPLINK_JOIN");
    CHECK(std::string(to_string(Event::scan_requested)) == "scan requested");
    CHECK(std::string(to_string(Outcome::camera_unreachable)) ==
          "camera-unreachable");
    CHECK(std::string(to_string(Event::stop_requested)) == "stop requested");
    CHECK(std::string(to_string(Outcome::stopped)) == "stopped");
}

TEST_CASE("a stop request in idle is ignored")
{
    Fsm fsm(kConfig);
    CHECK_FALSE(fsm.handle(Event::stop_requested).handled);
    CHECK(fsm.state() == State::idle);
}

TEST_CASE("stop while waking the camera returns to idle")
{
    Fsm fsm(kConfig);
    fsm.set_uplink_enabled(true);
    fsm.handle(Event::scan_requested);

    const Result result = fsm.handle(Event::stop_requested);
    CHECK(result.next == State::idle);
    CHECK(has(result, kCancelTimer));
    CHECK(has(result, kPauseBle));
    CHECK(result.outcome == Outcome::stopped);
    CHECK(fsm.attempt() == 0);
}

TEST_CASE("stop while joining the camera Wi-Fi disconnects")
{
    Fsm fsm(kConfig);
    fsm.handle(Event::scan_requested);
    fsm.handle(Event::credentials_received);

    const Result result = fsm.handle(Event::stop_requested);
    CHECK(result.next == State::idle);
    CHECK(has(result, kCancelTimer));
    CHECK(has(result, kDisconnectWifi));
    CHECK(result.outcome == Outcome::stopped);
}

TEST_CASE("stop during the camera session waits for its end, then skips "
          "the uplink")
{
    Fsm fsm(kConfig);
    fsm.set_uplink_enabled(true);
    reach_session(fsm);

    Result result = fsm.handle(Event::stop_requested);
    CHECK(result.handled);
    CHECK(result.next == State::session);
    CHECK(has(result, kStopSession));
    CHECK(has(result, kDisconnectWifi));
    CHECK(result.outcome == Outcome::none);
    CHECK(fsm.stop_pending());

    // A second request does not repeat the actions.
    result = fsm.handle(Event::stop_requested);
    CHECK(result.handled);
    CHECK(result.actions == kNone);

    result = fsm.handle(Event::session_ended);
    CHECK(result.next == State::idle);
    CHECK_FALSE(has(result, kConnectHomeWifi));
    CHECK(result.outcome == Outcome::stopped);
    CHECK_FALSE(fsm.stop_pending());
}

TEST_CASE("stop during the retry delay ends the cycle")
{
    Fsm fsm(kConfig);
    fsm.handle(Event::scan_requested);
    REQUIRE(fsm.handle(Event::timeout).next == State::backoff);

    const Result result = fsm.handle(Event::stop_requested);
    CHECK(result.next == State::idle);
    CHECK(has(result, kCancelTimer));
    CHECK(result.outcome == Outcome::stopped);
}

TEST_CASE("stop while joining the home Wi-Fi ends the cycle")
{
    Fsm fsm(kConfig);
    fsm.set_uplink_enabled(true);
    reach_session(fsm);
    REQUIRE(fsm.handle(Event::session_ended).next == State::uplink_join);

    const Result result = fsm.handle(Event::stop_requested);
    CHECK(result.next == State::idle);
    CHECK(has(result, kCancelTimer));
    CHECK(has(result, kDisconnectWifi));
    CHECK(result.outcome == Outcome::stopped);
}

TEST_CASE("a running backend sync is not interrupted by a stop request")
{
    Fsm fsm(kConfig);
    fsm.set_uplink_enabled(true);
    reach_session(fsm);
    fsm.handle(Event::session_ended);
    REQUIRE(fsm.handle(Event::wifi_connected).next == State::uplink);

    CHECK_FALSE(fsm.handle(Event::stop_requested).handled);
    const Result result = fsm.handle(Event::uplink_done);
    CHECK(result.next == State::idle);
    CHECK(result.outcome == Outcome::uplink_done);
}

TEST_CASE("a new scan after a stop starts fresh")
{
    Fsm fsm(kConfig);
    reach_session(fsm);
    fsm.handle(Event::stop_requested);
    fsm.handle(Event::session_ended);

    reach_session(fsm);
    CHECK_FALSE(fsm.stop_pending());
    CHECK(fsm.handle(Event::session_ended).outcome == Outcome::camera_scanned);
}

TEST_CASE("front-end phase follows the state and the last outcome")
{
    CHECK(front_end_phase(State::idle, Outcome::none) == FrontEndPhase::idle);
    CHECK(front_end_phase(State::idle, Outcome::camera_scanned) ==
          FrontEndPhase::finished);
    CHECK(front_end_phase(State::idle, Outcome::uplink_done) ==
          FrontEndPhase::finished);
    CHECK(front_end_phase(State::idle, Outcome::camera_unreachable) ==
          FrontEndPhase::not_found);
    CHECK(front_end_phase(State::idle, Outcome::uplink_unreachable) ==
          FrontEndPhase::upload_failed);
    CHECK(front_end_phase(State::idle, Outcome::stopped) ==
          FrontEndPhase::stopped);
    // While a cycle runs, the last outcome does not matter.
    CHECK(front_end_phase(State::ble_activation, Outcome::stopped) ==
          FrontEndPhase::searching);
    CHECK(front_end_phase(State::ble_activation, Outcome::none) ==
          FrontEndPhase::searching);
    CHECK(front_end_phase(State::wifi_join, Outcome::none) ==
          FrontEndPhase::connecting);
    CHECK(front_end_phase(State::session, Outcome::none) ==
          FrontEndPhase::connected);
    CHECK(front_end_phase(State::backoff, Outcome::none) ==
          FrontEndPhase::retry_wait);
    CHECK(front_end_phase(State::uplink_join, Outcome::none) ==
          FrontEndPhase::uploading);
    CHECK(front_end_phase(State::uplink, Outcome::none) ==
          FrontEndPhase::uploading);
}

TEST_CASE("front-end phase names stay stable for the console and web UI")
{
    CHECK(std::string(to_string(FrontEndPhase::idle)) == "idle");
    CHECK(std::string(to_string(FrontEndPhase::searching)) == "searching");
    CHECK(std::string(to_string(FrontEndPhase::connecting)) == "connecting");
    CHECK(std::string(to_string(FrontEndPhase::connected)) == "connected");
    CHECK(std::string(to_string(FrontEndPhase::retry_wait)) == "retry_wait");
    CHECK(std::string(to_string(FrontEndPhase::uploading)) == "uploading");
    CHECK(std::string(to_string(FrontEndPhase::finished)) == "finished");
    CHECK(std::string(to_string(FrontEndPhase::partial)) == "partial");
    CHECK(std::string(to_string(FrontEndPhase::not_found)) == "not_found");
    CHECK(std::string(to_string(FrontEndPhase::no_card)) == "no_card");
    CHECK(std::string(to_string(FrontEndPhase::upload_failed)) ==
          "upload_failed");
    CHECK(std::string(to_string(FrontEndPhase::stopped)) == "stopped");
}

TEST_CASE("the end of a camera session is classified by what it did")
{
    CHECK(session_end_event(true, false, 0) == Event::session_ended);
    CHECK(session_end_event(true, false, 7) == Event::session_ended);
    CHECK(session_end_event(false, false, 3) == Event::session_partial);
    CHECK(session_end_event(false, false, 0) == Event::session_failed);
    CHECK(session_end_event(false, true, 0) == Event::session_no_card);
    // Stored media count more than a later error.
    CHECK(session_end_event(false, true, 2) == Event::session_partial);
}

TEST_CASE("a failed camera session is retried like an unreached camera")
{
    Fsm fsm(kConfig);
    fsm.set_uplink_enabled(true);
    reach_session(fsm);

    Result result = fsm.handle(Event::session_failed);
    CHECK(result.next == State::backoff);
    CHECK(has(result, kDisconnectWifi));
    CHECK(result.timer_ms == 5000);
    CHECK(result.outcome == Outcome::none);

    // The remaining attempts fail too: the cycle gives up without a sync.
    fsm.handle(Event::timeout);
    fsm.handle(Event::credentials_received);
    fsm.handle(Event::wifi_connected);
    REQUIRE(fsm.handle(Event::session_failed).next == State::backoff);
    fsm.handle(Event::timeout);
    fsm.handle(Event::credentials_received);
    fsm.handle(Event::wifi_connected);
    REQUIRE(fsm.attempt() == 3);
    result = fsm.handle(Event::session_failed);
    CHECK(result.next == State::idle);
    CHECK(result.outcome == Outcome::camera_unreachable);
    CHECK(front_end_phase(fsm.state(), result.outcome, fsm.partial()) ==
          FrontEndPhase::not_found);
}

TEST_CASE("a later attempt can still read the camera after a failed session")
{
    Fsm fsm(kConfig);
    reach_session(fsm);
    REQUIRE(fsm.handle(Event::session_failed).next == State::backoff);
    fsm.handle(Event::timeout);
    fsm.handle(Event::credentials_received);
    fsm.handle(Event::wifi_connected);
    const Result result = fsm.handle(Event::session_ended);
    CHECK(result.outcome == Outcome::camera_scanned);
    CHECK_FALSE(fsm.partial());
}

TEST_CASE("a session that stored media before failing is synced, as partial")
{
    Fsm fsm(kConfig);
    fsm.set_uplink_enabled(true);
    reach_session(fsm);

    Result result = fsm.handle(Event::session_partial);
    CHECK(result.next == State::uplink_join);
    CHECK(fsm.partial());
    fsm.handle(Event::wifi_connected);
    result = fsm.handle(Event::uplink_done);
    CHECK(result.outcome == Outcome::uplink_done);
    CHECK(front_end_phase(fsm.state(), result.outcome, fsm.partial()) ==
          FrontEndPhase::partial);

    // Without an uplink the cycle ends right away, also as partial.
    Fsm local(kConfig);
    reach_session(local);
    result = local.handle(Event::session_partial);
    CHECK(result.outcome == Outcome::camera_scanned);
    CHECK(front_end_phase(local.state(), result.outcome, local.partial()) ==
          FrontEndPhase::partial);

    // The next cycle starts without the flag.
    local.handle(Event::scan_requested);
    CHECK_FALSE(local.partial());
}

TEST_CASE("a camera without an SD card ends the cycle without retries")
{
    Fsm fsm(kConfig);
    fsm.set_uplink_enabled(true);
    reach_session(fsm);

    const Result result = fsm.handle(Event::session_no_card);
    CHECK(result.next == State::idle);
    CHECK(has(result, kDisconnectWifi));
    CHECK_FALSE(has(result, kConnectHomeWifi));
    CHECK(result.outcome == Outcome::camera_no_card);
    CHECK(std::string(to_string(result.outcome)) == "camera-no-card");
    CHECK(front_end_phase(fsm.state(), result.outcome) ==
          FrontEndPhase::no_card);
}

TEST_CASE("a stop wins over how the session ended")
{
    for (Event end : {Event::session_ended, Event::session_partial,
                      Event::session_failed, Event::session_no_card}) {
        Fsm fsm(kConfig);
        reach_session(fsm);
        fsm.handle(Event::stop_requested);
        CHECK(fsm.handle(end).outcome == Outcome::stopped);
    }
}
