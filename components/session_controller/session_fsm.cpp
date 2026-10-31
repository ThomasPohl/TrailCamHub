#include "session_fsm.hpp"

namespace session
{

Fsm::Fsm(const Config &config) : config_(config)
{
    if (config_.max_attempts == 0) {
        config_.max_attempts = 1;
    }
}

State Fsm::state() const
{
    return state_;
}

bool Fsm::busy() const
{
    return state_ != State::idle;
}

void Fsm::set_uplink_enabled(bool enabled)
{
    uplink_enabled_ = enabled;
}

bool Fsm::uplink_enabled() const
{
    return uplink_enabled_;
}

std::uint32_t Fsm::attempt() const
{
    return attempt_;
}

Result Fsm::transition(State next, std::uint32_t actions,
                       std::uint32_t timer_ms, Outcome outcome)
{
    const Result result = {state_, next, actions, timer_ms, true, outcome};
    state_ = next;
    if (next == State::idle) {
        attempt_ = 0;
        stop_requested_ = false;
    }
    return result;
}

Result Fsm::camera_failed(std::uint32_t actions)
{
    if (attempt_ < config_.max_attempts) {
        return transition(State::backoff, actions | kStartTimer,
                          config_.retry_delay_ms);
    }
    return transition(State::idle, actions, 0, Outcome::camera_unreachable);
}

Result Fsm::camera_done(std::uint32_t actions)
{
    if (uplink_enabled_) {
        return start_uplink(actions);
    }
    return transition(State::idle, actions, 0, Outcome::camera_scanned);
}

Result Fsm::start_uplink(std::uint32_t actions)
{
    return transition(State::uplink_join,
                      actions | kConnectHomeWifi | kStartTimer,
                      config_.wifi_join_timeout_ms);
}

Result Fsm::handle(Event event)
{
    switch (state_) {
    case State::idle:
        if (event == Event::scan_requested) {
            attempt_ = 1;
            return transition(State::ble_activation, kResumeBle | kStartTimer,
                              config_.ble_activation_timeout_ms);
        }
        if (event == Event::upload_requested && uplink_enabled_) {
            return start_uplink(kNone);
        }
        break;

    case State::ble_activation:
        if (event == Event::credentials_received) {
            return transition(State::wifi_join,
                              kCancelTimer | kPauseBle | kConnectCameraWifi |
                                  kStartTimer,
                              config_.wifi_join_timeout_ms);
        }
        if (event == Event::timeout) {
            return camera_failed(kPauseBle);
        }
        if (event == Event::user_stop) {
            return transition(State::idle, kCancelTimer | kPauseBle, 0,
                              Outcome::stopped);
        }
        break;

    case State::wifi_join:
        if (event == Event::wifi_connected) {
            return transition(State::session, kCancelTimer | kStartSession);
        }
        if (event == Event::wifi_failed || event == Event::timeout) {
            return camera_failed(kCancelTimer | kDisconnectWifi);
        }
        if (event == Event::user_stop) {
            return transition(State::idle, kCancelTimer | kDisconnectWifi, 0,
                              Outcome::stopped);
        }
        // Disconnects while joining are retried by the Wi-Fi module.
        break;

    case State::session:
        if (event == Event::wifi_disconnected) {
            // Stay in this state until the session has ended.
            return {state_, state_, kStopSession | kDisconnectWifi,
                    0,      true,   Outcome::none};
        }
        if (event == Event::user_stop) {
            // Stay in this state until the session has ended.
            stop_requested_ = true;
            return {state_, state_, kStopSession | kDisconnectWifi,
                    0,      true,   Outcome::none};
        }
        if (event == Event::session_ended) {
            if (stop_requested_) {
                return transition(State::idle, kDisconnectWifi, 0,
                                  Outcome::stopped);
            }
            // Whatever the session fetched is on the SD card; sync it.
            return camera_done(kDisconnectWifi);
        }
        break;

    case State::backoff:
        if (event == Event::timeout) {
            attempt_++;
            return transition(State::ble_activation, kResumeBle | kStartTimer,
                              config_.ble_activation_timeout_ms);
        }
        if (event == Event::user_stop) {
            return transition(State::idle, kCancelTimer, 0, Outcome::stopped);
        }
        break;

    case State::uplink_join:
        if (event == Event::wifi_connected) {
            return transition(State::uplink, kCancelTimer | kStartUplink);
        }
        if (event == Event::wifi_failed || event == Event::timeout) {
            return transition(State::idle, kCancelTimer | kDisconnectWifi, 0,
                              Outcome::uplink_unreachable);
        }
        if (event == Event::user_stop) {
            return transition(State::idle, kCancelTimer | kDisconnectWifi, 0,
                              Outcome::stopped);
        }
        break;

    case State::uplink:
        if (event == Event::uplink_done) {
            return transition(State::idle, kDisconnectWifi, 0,
                              Outcome::uplink_done);
        }
        // A lost connection makes the running uplink fail by itself.
        break;
    }
    return {state_, state_, kNone, 0, false, Outcome::none};
}

const char *to_string(State state)
{
    switch (state) {
    case State::idle:
        return "IDLE";
    case State::ble_activation:
        return "BLE_ACTIVATION";
    case State::wifi_join:
        return "WIFI_JOIN";
    case State::session:
        return "SESSION";
    case State::backoff:
        return "BACKOFF";
    case State::uplink_join:
        return "UPLINK_JOIN";
    case State::uplink:
        return "UPLINK";
    }
    return "UNKNOWN";
}

const char *to_string(Event event)
{
    switch (event) {
    case Event::scan_requested:
        return "scan requested";
    case Event::upload_requested:
        return "upload requested";
    case Event::credentials_received:
        return "credentials received";
    case Event::wifi_connected:
        return "Wi-Fi connected";
    case Event::wifi_disconnected:
        return "Wi-Fi disconnected";
    case Event::wifi_failed:
        return "Wi-Fi failed";
    case Event::session_ended:
        return "session ended";
    case Event::uplink_done:
        return "uplink done";
    case Event::timeout:
        return "timeout";
    case Event::user_stop:
        return "user stop";
    }
    return "unknown";
}

const char *to_string(Outcome outcome)
{
    switch (outcome) {
    case Outcome::none:
        return "none";
    case Outcome::camera_scanned:
        return "camera-scanned";
    case Outcome::uplink_done:
        return "uplink-done";
    case Outcome::camera_unreachable:
        return "camera-unreachable";
    case Outcome::uplink_unreachable:
        return "uplink-unreachable";
    case Outcome::stopped:
        return "stopped";
    }
    return "unknown";
}

}  // namespace session
