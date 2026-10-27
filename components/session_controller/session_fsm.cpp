#include "session_fsm.hpp"

namespace session
{

Fsm::Fsm(const Config &config)
    : config_(config), retry_delay_ms_(config.retry_delay_min_ms)
{
}

State Fsm::state() const
{
    return state_;
}

std::uint32_t Fsm::next_retry_delay_ms() const
{
    return retry_delay_ms_;
}

Result Fsm::transition(State next, std::uint32_t actions,
                       std::uint32_t timer_ms)
{
    const Result result = {state_, next, actions, timer_ms, true};
    state_ = next;
    return result;
}

Result Fsm::enter_backoff(std::uint32_t actions)
{
    const std::uint32_t delay = retry_delay_ms_;
    retry_delay_ms_ = retry_delay_ms_ >= config_.retry_delay_max_ms / 2
                          ? config_.retry_delay_max_ms
                          : retry_delay_ms_ * 2;
    return transition(State::backoff, actions | kStartTimer, delay);
}

Result Fsm::handle(Event event)
{
    switch (state_) {
    case State::idle:
        if (event == Event::start) {
            return transition(State::ble_activation, kResumeBle);
        }
        break;

    case State::ble_activation:
        if (event == Event::credentials_received) {
            return transition(State::wifi_join,
                              kPauseBle | kConnectWifi | kStartTimer,
                              config_.wifi_join_timeout_ms);
        }
        break;

    case State::wifi_join:
        if (event == Event::wifi_connected) {
            // A session was reached; start over with the shortest delay.
            retry_delay_ms_ = config_.retry_delay_min_ms;
            return transition(State::session, kCancelTimer | kStartSession);
        }
        if (event == Event::wifi_failed || event == Event::timeout) {
            return enter_backoff(kCancelTimer | kDisconnectWifi);
        }
        // Disconnects while joining are retried by the Wi-Fi module.
        break;

    case State::session:
        if (event == Event::wifi_disconnected) {
            // Stay in this state until the session has ended.
            return {state_, state_, kStopSession | kDisconnectWifi, 0, true};
        }
        if (event == Event::session_ended) {
            return enter_backoff(kDisconnectWifi);
        }
        break;

    case State::backoff:
        if (event == Event::timeout) {
            return transition(State::ble_activation, kResumeBle);
        }
        break;
    }
    return {state_, state_, kNone, 0, false};
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
    }
    return "UNKNOWN";
}

const char *to_string(Event event)
{
    switch (event) {
    case Event::start:
        return "start";
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
    case Event::timeout:
        return "timeout";
    }
    return "unknown";
}

}  // namespace session
