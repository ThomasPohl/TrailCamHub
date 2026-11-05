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

bool Fsm::stop_pending() const
{
    return stop_pending_;
}

bool Fsm::partial() const
{
    return partial_;
}

Event session_end_event(bool ok, bool media_list_error,
                        std::uint32_t stored_items)
{
    if (ok) {
        return Event::session_ended;
    }
    if (stored_items > 0) {
        return Event::session_partial;
    }
    if (media_list_error) {
        return Event::session_no_card;
    }
    return Event::session_failed;
}

Result Fsm::transition(State next, std::uint32_t actions,
                       std::uint32_t timer_ms, Outcome outcome)
{
    const Result result = {state_, next, actions, timer_ms, true, outcome};
    state_ = next;
    if (next == State::idle) {
        attempt_ = 0;
        stop_pending_ = false;
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

Result Fsm::stop(std::uint32_t actions)
{
    return transition(State::idle, actions, 0, Outcome::stopped);
}

Result Fsm::handle(Event event)
{
    switch (state_) {
    case State::idle:
        if (event == Event::scan_requested) {
            attempt_ = 1;
            partial_ = false;
            return transition(State::ble_activation, kResumeBle | kStartTimer,
                              config_.ble_activation_timeout_ms);
        }
        if (event == Event::upload_requested && uplink_enabled_) {
            partial_ = false;
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
        if (event == Event::stop_requested) {
            return stop(kCancelTimer | kPauseBle);
        }
        break;

    case State::wifi_join:
        if (event == Event::wifi_connected) {
            return transition(State::session, kCancelTimer | kStartSession);
        }
        if (event == Event::wifi_failed || event == Event::timeout) {
            return camera_failed(kCancelTimer | kDisconnectWifi);
        }
        if (event == Event::stop_requested) {
            return stop(kCancelTimer | kDisconnectWifi);
        }
        // Disconnects while joining are retried by the Wi-Fi module.
        break;

    case State::session:
        if (event == Event::wifi_disconnected) {
            // Stay in this state until the session has ended.
            return {state_, state_, kStopSession | kDisconnectWifi,
                    0,      true,   Outcome::none};
        }
        if (event == Event::stop_requested) {
            // The session closes and then reports session_ended.
            const std::uint32_t actions =
                stop_pending_ ? kNone : kStopSession | kDisconnectWifi;
            stop_pending_ = true;
            return {state_, state_, actions, 0, true, Outcome::none};
        }
        if (event == Event::session_ended || event == Event::session_partial ||
            event == Event::session_failed || event == Event::session_no_card) {
            if (stop_pending_) {
                return stop(kDisconnectWifi);
            }
            if (event == Event::session_failed) {
                // Nothing was fetched: like a camera that was not reached.
                return camera_failed(kDisconnectWifi);
            }
            if (event == Event::session_no_card) {
                // Another attempt would end the same way.
                return transition(State::idle, kDisconnectWifi, 0,
                                  Outcome::camera_no_card);
            }
            // Whatever the session fetched is on the SD card; sync it.
            partial_ = event == Event::session_partial;
            return camera_done(kDisconnectWifi);
        }
        break;

    case State::backoff:
        if (event == Event::timeout) {
            attempt_++;
            return transition(State::ble_activation, kResumeBle | kStartTimer,
                              config_.ble_activation_timeout_ms);
        }
        if (event == Event::stop_requested) {
            return stop(kCancelTimer);
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
        if (event == Event::stop_requested) {
            return stop(kCancelTimer | kDisconnectWifi);
        }
        break;

    case State::uplink:
        if (event == Event::uplink_done) {
            return transition(State::idle, kDisconnectWifi, 0,
                              Outcome::uplink_done);
        }
        // A lost connection makes the running uplink fail by itself. A stop
        // request is ignored: the sync is the last step of the cycle and
        // aborting it would leave the backend half updated.
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
    case Event::stop_requested:
        return "stop requested";
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
    case Event::session_partial:
        return "session ended early";
    case Event::session_failed:
        return "session failed";
    case Event::session_no_card:
        return "session found no SD card";
    case Event::uplink_done:
        return "uplink done";
    case Event::timeout:
        return "timeout";
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
    case Outcome::camera_no_card:
        return "camera-no-card";
    case Outcome::uplink_unreachable:
        return "uplink-unreachable";
    case Outcome::stopped:
        return "stopped";
    }
    return "unknown";
}

FrontEndPhase front_end_phase(State state, Outcome last_outcome,
                              bool camera_partial)
{
    switch (state) {
    case State::idle:
        switch (last_outcome) {
        case Outcome::none:
            return FrontEndPhase::idle;
        case Outcome::camera_scanned:
        case Outcome::uplink_done:
            return camera_partial ? FrontEndPhase::partial
                                  : FrontEndPhase::finished;
        case Outcome::camera_unreachable:
            return FrontEndPhase::not_found;
        case Outcome::camera_no_card:
            return FrontEndPhase::no_card;
        case Outcome::uplink_unreachable:
            return FrontEndPhase::upload_failed;
        case Outcome::stopped:
            return FrontEndPhase::stopped;
        }
        return FrontEndPhase::idle;
    case State::ble_activation:
        return FrontEndPhase::searching;
    case State::wifi_join:
        return FrontEndPhase::connecting;
    case State::session:
        return FrontEndPhase::connected;
    case State::backoff:
        return FrontEndPhase::retry_wait;
    case State::uplink_join:
    case State::uplink:
        return FrontEndPhase::uploading;
    }
    return FrontEndPhase::idle;
}

const char *to_string(FrontEndPhase phase)
{
    switch (phase) {
    case FrontEndPhase::idle:
        return "idle";
    case FrontEndPhase::searching:
        return "searching";
    case FrontEndPhase::connecting:
        return "connecting";
    case FrontEndPhase::connected:
        return "connected";
    case FrontEndPhase::retry_wait:
        return "retry_wait";
    case FrontEndPhase::uploading:
        return "uploading";
    case FrontEndPhase::finished:
        return "finished";
    case FrontEndPhase::partial:
        return "partial";
    case FrontEndPhase::not_found:
        return "not_found";
    case FrontEndPhase::no_card:
        return "no_card";
    case FrontEndPhase::upload_failed:
        return "upload_failed";
    case FrontEndPhase::stopped:
        return "stopped";
    }
    return "idle";
}

}  // namespace session
