#pragma once

// State machine of one TrailCamHub cycle. The hub stays idle until a cycle is
// requested (serial command, timer); then it activates a camera over BLE,
// joins its Wi-Fi, runs the UDP session and goes on with the next camera:
// BLE again, skipping the cameras done in this cycle, until no further one
// answers within next_camera_timeout_ms, max_cameras are done or the camera
// time budget is spent. Then - if an uplink is configured - it joins the home
// Wi-Fi and syncs with the backend. Failed attempts are retried a bounded
// number of times per camera; a camera that still fails is skipped, so that
// it does not block the others. An "upload only" request skips the camera
// part. A stop request ends the camera part of a running cycle early and
// returns to idle without the backend sync; a running sync is not
// interrupted.
//
// Who requests cycles and what happens after them (sleep, staying awake for
// a local UI) is up to the application; the state machine is the same for
// all of them.
//
// It only decides; the controller executes the returned actions. This module
// has no ESP-IDF dependencies so it can be unit-tested on the host.

#include <cstdint>

namespace session
{

enum class State : std::uint8_t {
    idle,
    ble_activation,
    wifi_join,
    session,
    backoff,
    uplink_join,
    uplink,
};

enum class Event : std::uint8_t {
    scan_requested,
    upload_requested,
    stop_requested,
    credentials_received,
    wifi_connected,
    wifi_disconnected,
    wifi_failed,
    // The camera session ended: all its work done (session_ended), some
    // media stored before an error (session_partial), nothing stored
    // (session_failed), or the camera reported an error for its media list,
    // e.g. no SD card (session_no_card). See session_end_event().
    session_ended,
    session_partial,
    session_failed,
    session_no_card,
    uplink_done,
    timeout,
};

// Actions as bit flags. The controller executes them in the order of the
// declarations below, except kExcludeCamera, which runs before kResumeBle.
enum Action : std::uint32_t {
    kNone = 0,
    kCancelTimer = 1u << 0,
    kPauseBle = 1u << 1,
    kStopSession = 1u << 2,
    kDisconnectWifi = 1u << 3,
    kResumeBle = 1u << 4,
    kConnectCameraWifi = 1u << 5,
    kConnectHomeWifi = 1u << 6,
    kStartSession = 1u << 7,
    kStartUplink = 1u << 8,
    kStartTimer = 1u << 9,
    // The last camera activated over BLE is done for this cycle: skip it
    // when looking for the next one.
    kExcludeCamera = 1u << 10,
    // A new cycle starts: no camera is done yet.
    kClearExcludedCameras = 1u << 11,
};

// How one camera of a cycle ended; reported with the transition that ends
// it (Result::camera_result).
enum class CameraResult : std::uint8_t {
    none,     // no camera ended with this transition
    read,     // its session did all its work
    partial,  // its session stored some media before an error
    no_card,  // it has no SD card
    failed,   // found over BLE, but not read after all attempts
};

// How a cycle ended; reported with the transition back to idle.
enum class Outcome : std::uint8_t {
    none,                // the cycle is still running (or nothing happened)
    camera_scanned,      // camera session done, no uplink configured
    camera_listed,       // listing of the camera's media done (no sync)
    uplink_done,         // camera session (if any) and uplink done
    camera_unreachable,  // no camera was read (all attempts failed)
    // The camera has no SD card (media list error) and no sync followed:
    // none is configured, or the cycle only lists. With a sync the cycle ends
    // with uplink_done or uplink_unreachable and Fsm::no_card() is set.
    camera_no_card,
    uplink_unreachable,  // the home Wi-Fi could not be joined
    stopped,             // ended early by a stop request
};

// What the front ends (console, web UI, display) show. The camera and uplink
// states are folded together, and idle tells a successful cycle apart.
enum class FrontEndPhase : std::uint8_t {
    idle,
    searching,   // waking the camera over BLE
    connecting,  // joining the camera Wi-Fi
    connected,   // camera session
    retry_wait,  // waiting for the next camera attempt
    uploading,   // home Wi-Fi and backend sync
    finished,    // idle after a successful cycle
    listed,      // idle after a successful listing
    // Idle after a cycle whose camera session ended with an error after
    // storing some media (the sync still ran, if configured).
    partial,
    // Idle after a cycle that did not succeed; shown until the next one.
    not_found,      // all camera attempts failed
    no_card,        // the camera has no SD card
    upload_failed,  // the home Wi-Fi could not be joined
    stopped,        // ended early by a stop request
};

// Front-end phase for the FSM `state`; `last_outcome` is the outcome of the
// last finished cycle (none while a cycle runs or before the first one),
// `camera_partial` whether its camera session ended early (Fsm::partial())
// and `camera_no_card` whether the camera had no SD card (Fsm::no_card()).
FrontEndPhase front_end_phase(State state, Outcome last_outcome,
                              bool camera_partial = false,
                              bool camera_no_card = false);

// The event that reports the end of a camera session: `ok` if it did all its
// work, `media_list_error` if the camera reported an error for its media list
// (no SD card), `stored_items` thumbnails and files it stored.
Event session_end_event(bool ok, bool media_list_error,
                        std::uint32_t stored_items);

struct Config {
    std::uint32_t ble_activation_timeout_ms;
    std::uint32_t wifi_join_timeout_ms;
    std::uint32_t retry_delay_ms;
    // Camera attempts per camera (at least 1).
    std::uint32_t max_attempts;
    // How long to look for a further camera after one is done.
    std::uint32_t next_camera_timeout_ms;
    // Cameras per cycle (at least 1); 1 reads only the first that answers.
    std::uint32_t max_cameras;
    // No further camera is started once the camera part has run this long;
    // 0: no limit. See set_time_ms().
    std::uint32_t camera_budget_ms;
};

struct Result {
    State previous;
    State next;
    // Bitwise OR of Action values.
    std::uint32_t actions;
    // Duration for kStartTimer.
    std::uint32_t timer_ms;
    // False if the event was ignored in the current state.
    bool handled;
    // Set when the cycle finished with this transition.
    Outcome outcome;
    // Set when a camera ended with this transition.
    CameraResult camera_result;
};

class Fsm
{
  public:
    explicit Fsm(const Config &config);

    Result handle(Event event);
    State state() const;
    bool busy() const;

    // Whether a cycle continues with the backend sync. Read when a cycle
    // starts; the controller sets it from the stored configuration.
    void set_uplink_enabled(bool enabled);
    bool uplink_enabled() const;

    // How long joining the uplink may take (the home Wi-Fi, or attaching
    // the modem, which takes longer); 0: wifi_join_timeout_ms. Read when the
    // uplink is joined.
    void set_uplink_join_timeout_ms(std::uint32_t timeout_ms);

    // Whether a scan only lists the camera's media: it ends after the camera
    // session with camera_listed and never syncs. Read when a cycle starts.
    void set_listing(bool listing);

    // The time of the next event, in milliseconds of any monotonic clock;
    // used for the camera time budget. Call before handle().
    void set_time_ms(std::uint32_t now_ms);

    // Attempt for the current camera (1-based; 0 when idle).
    std::uint32_t attempt() const;

    // Cameras done in this cycle (read, without card or given up) and of
    // those the ones read (fully or in part). Kept after the cycle ends,
    // cleared when the next one starts.
    std::uint32_t cameras_done() const;
    std::uint32_t cameras_read() const;

    // Looking for a further camera: a BLE timeout ends the camera part.
    bool searching_next() const;

    // True between a stop request during the camera session and the end of
    // that session (the session needs a moment to close cleanly).
    bool stop_pending() const;

    // A camera session of the current (or last) cycle ended with an error
    // after storing some media. Cleared when a cycle starts.
    bool partial() const;

    // A camera of the current (or last) cycle reported no SD card. The cycle
    // still syncs (if configured), so that the backend learns about it.
    // Cleared when a cycle starts.
    bool no_card() const;

  private:
    Result transition(State next, std::uint32_t actions,
                      std::uint32_t timer_ms = 0,
                      Outcome outcome = Outcome::none);
    Result camera_failed(std::uint32_t actions);
    Result camera_ended(CameraResult result, std::uint32_t actions);
    Result next_camera(std::uint32_t actions, CameraResult result);
    Result cameras_finished(std::uint32_t actions,
                            CameraResult result = CameraResult::none);
    Result camera_done(std::uint32_t actions);
    bool budget_spent() const;
    Result start_uplink(std::uint32_t actions);
    Result stop(std::uint32_t actions);

    Config config_;
    State state_{State::idle};
    bool uplink_enabled_{false};
    std::uint32_t uplink_join_timeout_ms_{0};
    bool listing_{false};
    std::uint32_t attempt_{0};
    bool stop_pending_{false};
    bool partial_{false};
    bool no_card_{false};
    std::uint32_t cameras_done_{0};
    std::uint32_t cameras_read_{0};
    // A camera returned its credentials in the current round, so a failure
    // belongs to a camera (it is skipped after all attempts).
    bool camera_found_{false};
    bool searching_next_{false};
    std::uint32_t now_ms_{0};
    std::uint32_t cycle_started_ms_{0};
};

const char *to_string(State state);
const char *to_string(Event event);
const char *to_string(Outcome outcome);
// Stable names: "read", "partial", "no-card", "failed" ("none").
const char *to_string(CameraResult result);
// Stable names used by the console ("phase ...") and the web UI JSON.
const char *to_string(FrontEndPhase phase);

}  // namespace session
