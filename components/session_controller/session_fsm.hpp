#pragma once

// State machine of one TrailCamHub cycle. The hub stays idle until a cycle is
// requested (serial command, timer); then it activates the camera over BLE,
// joins its Wi-Fi, runs the UDP session and - if an uplink is configured -
// joins the home Wi-Fi and syncs with the backend. Failed camera attempts are
// retried a bounded number of times; afterwards the hub returns to idle and
// reports the outcome. An "upload only" request skips the camera part. A stop
// request ends the camera part of a running cycle early and returns to idle
// without the backend sync; a running sync is not interrupted.
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
// declarations below.
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
};

// How a cycle ended; reported with the transition back to idle.
enum class Outcome : std::uint8_t {
    none,                // the cycle is still running (or nothing happened)
    camera_scanned,      // camera session done, no uplink configured
    uplink_done,         // camera session (if any) and uplink done
    camera_unreachable,  // all camera attempts failed
    camera_no_card,      // the camera has no SD card (media list error)
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
// last finished cycle (none while a cycle runs or before the first one), and
// `camera_partial` whether its camera session ended early (Fsm::partial()).
FrontEndPhase front_end_phase(State state, Outcome last_outcome,
                              bool camera_partial = false);

// The event that reports the end of a camera session: `ok` if it did all its
// work, `media_list_error` if the camera reported an error for its media list
// (no SD card), `stored_items` thumbnails and files it stored.
Event session_end_event(bool ok, bool media_list_error,
                        std::uint32_t stored_items);

struct Config {
    std::uint32_t ble_activation_timeout_ms;
    std::uint32_t wifi_join_timeout_ms;
    std::uint32_t retry_delay_ms;
    // Camera attempts per cycle (at least 1).
    std::uint32_t max_attempts;
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

    // Camera attempt within the current cycle (1-based; 0 when idle).
    std::uint32_t attempt() const;

    // True between a stop request during the camera session and the end of
    // that session (the session needs a moment to close cleanly).
    bool stop_pending() const;

    // The camera session of the current (or last) cycle ended with an error
    // after storing some media. Cleared when a cycle starts.
    bool partial() const;

  private:
    Result transition(State next, std::uint32_t actions,
                      std::uint32_t timer_ms = 0,
                      Outcome outcome = Outcome::none);
    Result camera_failed(std::uint32_t actions);
    Result camera_done(std::uint32_t actions);
    Result start_uplink(std::uint32_t actions);
    Result stop(std::uint32_t actions);

    Config config_;
    State state_{State::idle};
    bool uplink_enabled_{false};
    std::uint32_t attempt_{0};
    bool stop_pending_{false};
    bool partial_{false};
};

const char *to_string(State state);
const char *to_string(Event event);
const char *to_string(Outcome outcome);
// Stable names used by the console ("phase ...") and the web UI JSON.
const char *to_string(FrontEndPhase phase);

}  // namespace session
