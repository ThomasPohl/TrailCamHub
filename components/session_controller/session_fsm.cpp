#include "session_fsm.hpp"

namespace session
{

Fsm::Fsm(const Config &config) : config_(config)
{
    if (config_.max_attempts == 0) {
        config_.max_attempts = 1;
    }
    if (config_.max_cameras == 0) {
        config_.max_cameras = 1;
    }
}

void Fsm::set_time_ms(std::uint32_t now_ms)
{
    now_ms_ = now_ms;
}

std::uint32_t Fsm::cameras_done() const
{
    return cameras_done_;
}

std::uint32_t Fsm::cameras_read() const
{
    return cameras_read_;
}

bool Fsm::searching_next() const
{
    return searching_next_;
}

bool Fsm::budget_spent() const
{
    return config_.camera_budget_ms != 0 &&
           now_ms_ - cycle_started_ms_ >= config_.camera_budget_ms;
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

void Fsm::set_uplink_join_timeout_ms(std::uint32_t timeout_ms)
{
    uplink_join_timeout_ms_ = timeout_ms;
}

bool Fsm::uplink_enabled() const
{
    return uplink_enabled_;
}

void Fsm::set_listing(bool listing)
{
    listing_ = listing;
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

bool Fsm::no_card() const
{
    return no_card_;
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
    const Result result = {state_, next,    actions,           timer_ms,
                           true,   outcome, CameraResult::none};
    state_ = next;
    if (next == State::idle) {
        attempt_ = 0;
        stop_pending_ = false;
        searching_next_ = false;
    }
    return result;
}

Result Fsm::camera_failed(std::uint32_t actions)
{
    if (attempt_ < config_.max_attempts) {
        return transition(State::backoff, actions | kStartTimer,
                          config_.retry_delay_ms);
    }
    if (camera_found_) {
        // Skip this camera; the others may still answer.
        return camera_ended(CameraResult::failed, actions);
    }
    return cameras_finished(actions);
}

// A camera is done: count it and go on with the next one.
Result Fsm::camera_ended(CameraResult result, std::uint32_t actions)
{
    cameras_done_++;
    switch (result) {
    case CameraResult::read:
        cameras_read_++;
        break;
    case CameraResult::partial:
        cameras_read_++;
        partial_ = true;
        break;
    case CameraResult::no_card:
        no_card_ = true;
        break;
    case CameraResult::failed:
    case CameraResult::none:
        break;
    }
    return next_camera(actions | kExcludeCamera, result);
}

Result Fsm::next_camera(std::uint32_t actions, CameraResult result)
{
    // A listing shows the media of one camera.
    if (listing_ || cameras_done_ >= config_.max_cameras || budget_spent()) {
        return cameras_finished(actions, result);
    }
    attempt_ = 1;
    camera_found_ = false;
    searching_next_ = true;
    Result next =
        transition(State::ble_activation, actions | kResumeBle | kStartTimer,
                   config_.next_camera_timeout_ms);
    next.camera_result = result;
    return next;
}

// The camera part is over: sync what was read, or report why nothing was.
Result Fsm::cameras_finished(std::uint32_t actions, CameraResult result)
{
    Result next;
    if (cameras_read_ > 0) {
        next = camera_done(actions);
    } else if (no_card_ && uplink_enabled_ && !listing_) {
        // Sync anyway, so that the backend shows the missing card.
        next = start_uplink(actions);
    } else if (no_card_) {
        next = transition(State::idle, actions, 0, Outcome::camera_no_card);
    } else {
        next = transition(State::idle, actions, 0, Outcome::camera_unreachable);
    }
    next.camera_result = result;
    return next;
}

Result Fsm::camera_done(std::uint32_t actions)
{
    if (listing_) {
        return transition(State::idle, actions, 0, Outcome::camera_listed);
    }
    if (uplink_enabled_) {
        return start_uplink(actions);
    }
    return transition(State::idle, actions, 0, Outcome::camera_scanned);
}

Result Fsm::start_uplink(std::uint32_t actions)
{
    return transition(
        State::uplink_join, actions | kConnectHomeWifi | kStartTimer,
        uplink_join_timeout_ms_ > 0 ? uplink_join_timeout_ms_
                                    : config_.wifi_join_timeout_ms);
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
            no_card_ = false;
            cameras_done_ = 0;
            cameras_read_ = 0;
            camera_found_ = false;
            searching_next_ = false;
            cycle_started_ms_ = now_ms_;
            return transition(State::ble_activation,
                              kClearExcludedCameras | kResumeBle | kStartTimer,
                              config_.ble_activation_timeout_ms);
        }
        if (event == Event::upload_requested && uplink_enabled_) {
            partial_ = false;
            no_card_ = false;
            cameras_done_ = 0;
            cameras_read_ = 0;
            return start_uplink(kNone);
        }
        break;

    case State::ble_activation:
        if (event == Event::credentials_received) {
            camera_found_ = true;
            searching_next_ = false;
            return transition(State::wifi_join,
                              kCancelTimer | kPauseBle | kConnectCameraWifi |
                                  kStartTimer,
                              config_.wifi_join_timeout_ms);
        }
        if (event == Event::timeout) {
            if (searching_next_) {
                // No further camera answered.
                return cameras_finished(kPauseBle);
            }
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
            return {state_,
                    state_,
                    kStopSession | kDisconnectWifi,
                    0,
                    true,
                    Outcome::none,
                    CameraResult::none};
        }
        if (event == Event::stop_requested) {
            // The session closes and then reports session_ended.
            const std::uint32_t actions =
                stop_pending_ ? kNone : kStopSession | kDisconnectWifi;
            stop_pending_ = true;
            return {state_, state_,        actions,           0,
                    true,   Outcome::none, CameraResult::none};
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
                return camera_ended(CameraResult::no_card, kDisconnectWifi);
            }
            // Whatever the session fetched is on the SD card; it is synced
            // after the last camera.
            return camera_ended(event == Event::session_partial
                                    ? CameraResult::partial
                                    : CameraResult::read,
                                kDisconnectWifi);
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
    return {state_, state_, kNone, 0, false, Outcome::none, CameraResult::none};
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

const char *to_string(CameraResult result)
{
    switch (result) {
    case CameraResult::none:
        return "none";
    case CameraResult::read:
        return "read";
    case CameraResult::partial:
        return "partial";
    case CameraResult::no_card:
        return "no-card";
    case CameraResult::failed:
        return "failed";
    }
    return "none";
}

const char *to_string(Outcome outcome)
{
    switch (outcome) {
    case Outcome::none:
        return "none";
    case Outcome::camera_scanned:
        return "camera-scanned";
    case Outcome::camera_listed:
        return "camera-listed";
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
                              bool camera_partial, bool camera_no_card)
{
    switch (state) {
    case State::idle:
        switch (last_outcome) {
        case Outcome::none:
            return FrontEndPhase::idle;
        case Outcome::camera_scanned:
        case Outcome::uplink_done:
            if (camera_no_card) {
                return FrontEndPhase::no_card;
            }
            return camera_partial ? FrontEndPhase::partial
                                  : FrontEndPhase::finished;
        case Outcome::camera_listed:
            return FrontEndPhase::listed;
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
    case FrontEndPhase::listed:
        return "listed";
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
