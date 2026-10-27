#pragma once

// State machine of a camera session: activate the camera over BLE, join its
// Wi-Fi, run the UDP session, and retry with an increasing delay after
// failures. It only decides; the controller executes the returned actions.
// This module has no ESP-IDF dependencies so it can be unit-tested on the
// host.

#include <cstdint>

namespace session
{

enum class State : std::uint8_t {
    idle,
    ble_activation,
    wifi_join,
    session,
    backoff,
};

enum class Event : std::uint8_t {
    start,
    credentials_received,
    wifi_connected,
    wifi_disconnected,
    wifi_failed,
    session_ended,
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
    kConnectWifi = 1u << 5,
    kStartSession = 1u << 6,
    kStartTimer = 1u << 7,
};

struct Config {
    std::uint32_t wifi_join_timeout_ms;
    std::uint32_t retry_delay_min_ms;
    std::uint32_t retry_delay_max_ms;
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
};

class Fsm
{
  public:
    explicit Fsm(const Config &config);

    Result handle(Event event);
    State state() const;
    // Delay that will be used for the next backoff.
    std::uint32_t next_retry_delay_ms() const;

  private:
    Result transition(State next, std::uint32_t actions,
                      std::uint32_t timer_ms = 0);
    Result enter_backoff(std::uint32_t actions);

    Config config_;
    State state_{State::idle};
    std::uint32_t retry_delay_ms_;
};

const char *to_string(State state);
const char *to_string(Event event);

}  // namespace session
