#pragma once

// State machine of one TrailCamHub cycle. The hub stays idle until a cycle is
// requested (serial command, timer); then it activates the camera over BLE,
// joins its Wi-Fi, runs the UDP session and - if an uplink is configured -
// joins the home Wi-Fi and syncs with the backend. Failed camera attempts are
// retried a bounded number of times; afterwards the hub returns to idle and
// reports the outcome. An "upload only" request skips the camera part.
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
    credentials_received,
    wifi_connected,
    wifi_disconnected,
    wifi_failed,
    session_ended,
    uplink_done,
    timeout,
    // The user stops the cycle (for example a long button press).
    user_stop,
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
    uplink_unreachable,  // the home Wi-Fi could not be joined
    stopped,             // the user stopped the cycle
};

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

  private:
    Result transition(State next, std::uint32_t actions,
                      std::uint32_t timer_ms = 0,
                      Outcome outcome = Outcome::none);
    Result camera_failed(std::uint32_t actions);
    Result camera_done(std::uint32_t actions);
    Result start_uplink(std::uint32_t actions);

    Config config_;
    State state_{State::idle};
    bool uplink_enabled_{false};
    std::uint32_t attempt_{0};
    // Set by user_stop during a session; the session ends in idle.
    bool stop_requested_{false};
};

const char *to_string(State state);
const char *to_string(Event event);
const char *to_string(Outcome outcome);

}  // namespace session
