/*
 * This module runs TrailCamHub cycles. Requests (serial console, timer, other
 * front ends) and module reports (credentials, Wi-Fi changes, session and
 * uplink end, timeouts) go to a queue; a single task feeds them to the
 * session state machine and executes the resulting actions. The camera
 * session and the backend uplink run in their own tasks because they block.
 * Status changes are published to the registered listeners.
 */

#include <atomic>
#include <cstdint>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "camera_ble.h"
#include "camera_protocol.h"
#include "camera_wifi.h"
#include "hub_config.hpp"
#include "session_controller.h"
#include "session_fsm.hpp"
#include "system_diagnostics.h"
#include "uplink.hpp"

#ifndef CONFIG_SESSION_WIFI_JOIN_TIMEOUT_S
#error "main/Kconfig.projbuild was not processed"
#endif

#define EVENT_QUEUE_LENGTH 8
#define CONTROLLER_TASK_STACK 4096
#define SESSION_TASK_STACK 8192
// TLS handshakes need a large stack.
#define UPLINK_TASK_STACK 12288
#define CONTROLLER_TASK_PRIORITY 5
#define SESSION_TASK_PRIORITY 5
#define UPLINK_TASK_PRIORITY 5
#define MAX_SSID_LEN 32
#define MAX_PASSWORD_LEN 64

namespace
{

struct QueuedEvent {
    session::Event type;
    // Timer generation for timeouts; stale timeouts are dropped.
    std::uint32_t timer_generation;
    esp_ip4_addr_t gateway;
    esp_err_t error;
    std::uint8_t reason;
    // With scan_requested: only list the camera's media.
    bool list_only;
    char ssid[MAX_SSID_LEN + 1];
    char password[MAX_PASSWORD_LEN + 1];
};

const char *TAG = "trailcamhub_session";

const session::Config kConfig = {
    CONFIG_SESSION_BLE_ACTIVATION_TIMEOUT_S * 1000u,
    CONFIG_SESSION_WIFI_JOIN_TIMEOUT_S * 1000u,
    CONFIG_SESSION_RETRY_DELAY_S * 1000u,
    CONFIG_SESSION_MAX_ATTEMPTS,
};

QueueHandle_t s_events;
QueueHandle_t s_session_requests;
QueueHandle_t s_uplink_requests;
esp_timer_handle_t s_timer;
std::atomic<std::uint32_t> s_timer_generation;
// Mirrors the FSM state for other tasks (console requests, status).
std::atomic<session::State> s_state{session::State::idle};
session::Fsm s_fsm(kConfig);

// Camera credentials and gateway used by the next actions.
char s_ssid[MAX_SSID_LEN + 1];
char s_password[MAX_PASSWORD_LEN + 1];
esp_ip4_addr_t s_gateway;
// Settings read when the current cycle started (home Wi-Fi, backend).
hub_config::Settings s_settings;

// Outcome of the camera session and the sync of the running cycle; only
// touched by the controller task.
bool s_camera_ok;
bool s_list_only;
bool s_sync_ok;

// Published status; written by the controller task, read by any task.
SemaphoreHandle_t s_status_lock;
session_status_t s_status;
// Outcome of the last finished cycle, for the front-end phase when idle.
// Guarded by s_status_lock like s_status.
session::Outcome s_last_outcome = session::Outcome::none;

struct Listener {
    session_status_listener_t callback;
    void *context;
};
Listener s_listeners[SESSION_CONTROLLER_MAX_LISTENERS];

// One "TCH EVENT" line for the web installer and other console clients.
void console_event(const char *format, ...)
    __attribute__((format(printf, 1, 2)));
void console_event(const char *format, ...)
{
    char line[200];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    printf("TCH EVENT %s\n", line);
    fflush(stdout);
}

void post_event(const QueuedEvent &event)
{
    if (xQueueSend(s_events, &event, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Session event queue full; dropping \"%s\"",
                 session::to_string(event.type));
    }
}

QueuedEvent make_event(session::Event type)
{
    QueuedEvent event = {};
    event.type = type;
    return event;
}

void wipe(void *data, size_t size)
{
    volatile std::uint8_t *bytes = static_cast<volatile std::uint8_t *>(data);
    for (size_t i = 0; i < size; i++) {
        bytes[i] = 0;
    }
}

// --- Status ---

session_phase_t phase_of(session::State state)
{
    switch (state) {
    case session::State::idle:
        return SESSION_PHASE_IDLE;
    case session::State::ble_activation:
        return SESSION_PHASE_CAMERA_WAKE;
    case session::State::wifi_join:
        return SESSION_PHASE_CAMERA_WIFI_JOIN;
    case session::State::session:
        return SESSION_PHASE_CAMERA_SESSION;
    case session::State::backoff:
        return SESSION_PHASE_CAMERA_RETRY_WAIT;
    case session::State::uplink_join:
        return SESSION_PHASE_UPLINK_WIFI_JOIN;
    case session::State::uplink:
        return SESSION_PHASE_UPLINK;
    }
    return SESSION_PHASE_IDLE;
}

session_result_t result_of(session::Outcome outcome)
{
    switch (outcome) {
    case session::Outcome::none:
        return SESSION_RESULT_NONE;
    case session::Outcome::camera_scanned:
        return SESSION_RESULT_CAMERA_SCANNED;
    case session::Outcome::uplink_done:
        return SESSION_RESULT_UPLINK_DONE;
    case session::Outcome::camera_unreachable:
        return SESSION_RESULT_CAMERA_UNREACHABLE;
    case session::Outcome::camera_no_card:
        return SESSION_RESULT_CAMERA_NO_CARD;
    case session::Outcome::uplink_unreachable:
        return SESSION_RESULT_UPLINK_UNREACHABLE;
    case session::Outcome::stopped:
        return SESSION_RESULT_STOPPED;
    }
    return SESSION_RESULT_NONE;
}

session_front_end_phase_t front_end_of(session::FrontEndPhase phase)
{
    switch (phase) {
    case session::FrontEndPhase::idle:
        return SESSION_FRONT_END_IDLE;
    case session::FrontEndPhase::searching:
        return SESSION_FRONT_END_SEARCHING;
    case session::FrontEndPhase::connecting:
        return SESSION_FRONT_END_CONNECTING;
    case session::FrontEndPhase::connected:
        return SESSION_FRONT_END_CONNECTED;
    case session::FrontEndPhase::retry_wait:
        return SESSION_FRONT_END_RETRY_WAIT;
    case session::FrontEndPhase::uploading:
        return SESSION_FRONT_END_UPLOADING;
    case session::FrontEndPhase::finished:
        return SESSION_FRONT_END_FINISHED;
    case session::FrontEndPhase::partial:
        return SESSION_FRONT_END_PARTIAL;
    case session::FrontEndPhase::not_found:
        return SESSION_FRONT_END_NOT_FOUND;
    case session::FrontEndPhase::no_card:
        return SESSION_FRONT_END_NO_CARD;
    case session::FrontEndPhase::upload_failed:
        return SESSION_FRONT_END_UPLOAD_FAILED;
    case session::FrontEndPhase::stopped:
        return SESSION_FRONT_END_STOPPED;
    }
    return SESSION_FRONT_END_IDLE;
}

// Sets the front-end phase of `status` for `state`.
void set_front_end_phase(session_status_t &status, session::State state,
                         session::Outcome last_outcome,
                         bool camera_partial = false)
{
    const session::FrontEndPhase phase =
        session::front_end_phase(state, last_outcome, camera_partial);
    status.front_end_phase = front_end_of(phase);
    status.front_end_phase_name = session::to_string(phase);
}

// Updates the published status after a handled event and tells the
// listeners. Runs in the controller task only.
void publish_status(const session::Result &result)
{
    session_status_t status;
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    const bool cycle_started = result.previous == session::State::idle &&
                               result.next != session::State::idle;
    s_status.phase = phase_of(result.next);
    s_status.phase_name = session::to_string(result.next);
    s_status.stopping = s_fsm.stop_pending();
    s_status.attempt = s_fsm.attempt();
    s_status.retry_delay_ms =
        result.next == session::State::backoff ? result.timer_ms : 0;
    s_status.cycle_finished = result.outcome != session::Outcome::none;
    if (cycle_started) {
        s_last_outcome = session::Outcome::none;
        s_status.last_result = SESSION_RESULT_NONE;
        s_status.last_result_name = session::to_string(session::Outcome::none);
        s_status.camera_ssid[0] = '\0';
    }
    s_status.camera_ok = s_camera_ok;
    s_status.camera_partial = s_fsm.partial();
    s_status.list_only = s_list_only;
    s_status.sync_ok = s_sync_ok;
    if (s_status.cycle_finished) {
        s_last_outcome = result.outcome;
        s_status.last_result = result_of(result.outcome);
        s_status.last_result_name = session::to_string(result.outcome);
    }
    set_front_end_phase(s_status, result.next, s_last_outcome, s_fsm.partial());
    // The SSID belongs to the camera part of the cycle. It stays set after the
    // cycle ends, so a front end can show it with the result until the next
    // cycle starts (cleared above).
    if (result.next == session::State::wifi_join ||
        result.next == session::State::session) {
        snprintf(s_status.camera_ssid, sizeof(s_status.camera_ssid), "%s",
                 s_ssid);
    }
    status = s_status;
    Listener listeners[SESSION_CONTROLLER_MAX_LISTENERS];
    memcpy(listeners, s_listeners, sizeof(listeners));
    xSemaphoreGive(s_status_lock);

    for (const Listener &listener : listeners) {
        if (listener.callback != NULL) {
            listener.callback(&status, listener.context);
        }
    }
}

// --- Module callbacks (called from other tasks) ---

esp_err_t on_credentials(const char *ssid, const char *password)
{
    if (strlen(ssid) > MAX_SSID_LEN || strlen(password) > MAX_PASSWORD_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    QueuedEvent event = make_event(session::Event::credentials_received);
    strcpy(event.ssid, ssid);
    strcpy(event.password, password);
    post_event(event);
    wipe(event.password, sizeof(event.password));
    return ESP_OK;
}

void on_wifi_connected(const esp_ip4_addr_t *gateway)
{
    QueuedEvent event = make_event(session::Event::wifi_connected);
    event.gateway = *gateway;
    post_event(event);
}

void on_wifi_disconnected(std::uint8_t reason)
{
    QueuedEvent event = make_event(session::Event::wifi_disconnected);
    event.reason = reason;
    post_event(event);
}

void on_wifi_failed(std::uint8_t reason)
{
    QueuedEvent event = make_event(session::Event::wifi_failed);
    event.reason = reason;
    post_event(event);
}

void on_timer(void *arg)
{
    (void)arg;
    QueuedEvent event = make_event(session::Event::timeout);
    event.timer_generation = s_timer_generation.load();
    post_event(event);
}

// --- Session task: runs one camera session per request ---

void session_task(void *arg)
{
    (void)arg;
    esp_ip4_addr_t gateway;
    while (true) {
        xQueueReceive(s_session_requests, &gateway, portMAX_DELAY);
        diagnostics_log_memory("camera Wi-Fi connected");
        const esp_err_t error = camera_protocol_run(&gateway);
        const uint32_t stored = camera_protocol_stored_items();
        QueuedEvent event = make_event(session::session_end_event(
            error == ESP_OK, camera_protocol_media_list_error(), stored));
        event.error = error;
        ESP_LOGI(TAG, "Camera session ended: %s, %u item(s) stored (%s)",
                 esp_err_to_name(error), (unsigned)stored,
                 session::to_string(event.type));
        post_event(event);
    }
}

// --- Uplink task: runs one backend sync per request ---

void uplink_task(void *arg)
{
    (void)arg;
    std::uint8_t request;
    while (true) {
        xQueueReceive(s_uplink_requests, &request, portMAX_DELAY);
        diagnostics_log_memory("home Wi-Fi connected");
        uplink::Report report;
        QueuedEvent event = make_event(session::Event::uplink_done);
        event.error = uplink::run(s_settings, report);
        if (event.error == ESP_OK) {
            console_event("uplink ok media=%d thumbs=%d/%d not-on-sd=%d "
                          "commands=%d",
                          report.media_reported, report.thumbs_uploaded,
                          report.thumbs_requested, report.thumbs_not_on_sd,
                          report.commands_received);
        } else {
            console_event("uplink failed %s", report.error.c_str());
        }
        post_event(event);
    }
}

// --- Actions ---

void cancel_timer()
{
    s_timer_generation++;
    esp_timer_stop(s_timer);
}

void start_timer(std::uint32_t duration_ms)
{
    cancel_timer();
    esp_err_t error =
        esp_timer_start_once(s_timer, (std::uint64_t)duration_ms * 1000u);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not start session timer: %s",
                 esp_err_to_name(error));
    }
}

void execute(const session::Result &result)
{
    const std::uint32_t actions = result.actions;
    if (actions & session::kCancelTimer) {
        cancel_timer();
    }
    if (actions & session::kPauseBle) {
        camera_ble_pause();
        diagnostics_log_memory("BLE activation paused");
    }
    if (actions & session::kStopSession) {
        camera_protocol_request_stop();
    }
    if (actions & session::kDisconnectWifi) {
        camera_wifi_disconnect();
    }
    if (actions & session::kResumeBle) {
        camera_ble_resume();
        diagnostics_log_memory("BLE activation resumed");
    }
    if (actions & session::kConnectCameraWifi) {
        esp_err_t error = camera_wifi_connect(s_ssid, s_password);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Could not join camera Wi-Fi: %s",
                     esp_err_to_name(error));
            on_wifi_failed(0);
        }
    }
    if (actions & session::kConnectHomeWifi) {
        esp_err_t error = camera_wifi_connect_home(
            s_settings.wifi_ssid.c_str(), s_settings.wifi_password.c_str());
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Could not join home Wi-Fi: %s",
                     esp_err_to_name(error));
            on_wifi_failed(0);
        }
    }
    if (actions & session::kStartSession) {
        if (xQueueOverwrite(s_session_requests, &s_gateway) != pdTRUE) {
            ESP_LOGE(TAG, "Could not start camera session");
        }
    }
    if (actions & session::kStartUplink) {
        const std::uint8_t request = 1;
        if (xQueueOverwrite(s_uplink_requests, &request) != pdTRUE) {
            ESP_LOGE(TAG, "Could not start uplink");
        }
    }
    if (actions & session::kStartTimer) {
        start_timer(result.timer_ms);
    }
}

bool is_session_end(session::Event event)
{
    return event == session::Event::session_ended ||
           event == session::Event::session_partial ||
           event == session::Event::session_failed ||
           event == session::Event::session_no_card;
}

// Reads the stored settings when a cycle starts; the uplink only runs if
// they are complete.
void load_settings_for_cycle()
{
    if (hub_config::load(s_settings) != ESP_OK) {
        s_settings = hub_config::Settings{};
    }
    s_fsm.set_uplink_enabled(hub_config::uplink_configured(s_settings) &&
                             !s_list_only);
}

void controller_task(void *arg)
{
    (void)arg;
    QueuedEvent event;
    while (true) {
        xQueueReceive(s_events, &event, portMAX_DELAY);
        if (event.type == session::Event::timeout &&
            event.timer_generation != s_timer_generation.load()) {
            continue;
        }
        if (event.type == session::Event::credentials_received &&
            s_fsm.state() == session::State::ble_activation) {
            strcpy(s_ssid, event.ssid);
            strcpy(s_password, event.password);
        }
        wipe(event.password, sizeof(event.password));
        if (event.type == session::Event::wifi_connected) {
            s_gateway = event.gateway;
        }
        if (!s_fsm.busy() && (event.type == session::Event::scan_requested ||
                              event.type == session::Event::upload_requested)) {
            s_camera_ok = false;
            s_sync_ok = false;
            s_list_only = event.list_only;
            camera_protocol_set_list_only(s_list_only);
        }
        if (is_session_end(event.type) &&
            s_fsm.state() == session::State::session && !s_fsm.stop_pending()) {
            // Read in full or in part; a retry after a failed attempt sets it
            // again.
            s_camera_ok = event.type == session::Event::session_ended ||
                          event.type == session::Event::session_partial;
        }
        if (event.type == session::Event::uplink_done &&
            s_fsm.state() == session::State::uplink) {
            s_sync_ok = event.error == ESP_OK;
        }
        if (!s_fsm.busy() && (event.type == session::Event::scan_requested ||
                              event.type == session::Event::upload_requested)) {
            load_settings_for_cycle();
        }

        const session::Result result = s_fsm.handle(event.type);
        if (!result.handled) {
            ESP_LOGD(TAG, "Ignoring \"%s\" in state %s",
                     session::to_string(event.type),
                     session::to_string(result.previous));
            continue;
        }
        if (result.next != result.previous) {
            ESP_LOGI(TAG, "Session state %s -> %s (%s)",
                     session::to_string(result.previous),
                     session::to_string(result.next),
                     session::to_string(event.type));
        } else {
            ESP_LOGI(TAG, "Session state %s: handling %s",
                     session::to_string(result.next),
                     session::to_string(event.type));
        }
        if (result.next == session::State::backoff) {
            ESP_LOGI(TAG, "Retrying camera activation in %u s (attempt %u/%u)",
                     (unsigned)(result.timer_ms / 1000),
                     (unsigned)(s_fsm.attempt() + 1),
                     (unsigned)kConfig.max_attempts);
        }
        s_state = result.next;
        execute(result);
        if (result.actions & session::kConnectCameraWifi) {
            wipe(s_password, sizeof(s_password));
        }
        if (result.outcome != session::Outcome::none) {
            ESP_LOGI(TAG, "Cycle finished: %s",
                     session::to_string(result.outcome));
            diagnostics_log_memory("cycle finished");
        }
        publish_status(result);
    }
}

}  // namespace

esp_err_t session_controller_init(void)
{
    s_status_lock = xSemaphoreCreateMutex();
    if (s_status_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_status.phase = SESSION_PHASE_IDLE;
    s_status.phase_name = session::to_string(session::State::idle);
    s_status.max_attempts = kConfig.max_attempts;
    s_status.last_result = SESSION_RESULT_NONE;
    s_status.last_result_name = session::to_string(session::Outcome::none);
    set_front_end_phase(s_status, session::State::idle, session::Outcome::none);

    s_events = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(QueuedEvent));
    s_session_requests = xQueueCreate(1, sizeof(esp_ip4_addr_t));
    s_uplink_requests = xQueueCreate(1, sizeof(std::uint8_t));
    if (s_events == NULL || s_session_requests == NULL ||
        s_uplink_requests == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = on_timer,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "session",
        .skip_unhandled_events = false,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_timer), TAG,
                        "Could not create session timer");

    static const camera_wifi_callbacks_t wifi_callbacks = {
        .on_connected = on_wifi_connected,
        .on_disconnected = on_wifi_disconnected,
        .on_failed = on_wifi_failed,
    };
    ESP_RETURN_ON_ERROR(camera_wifi_init(&wifi_callbacks), TAG,
                        "Could not initialize camera Wi-Fi");
    diagnostics_log_memory("Wi-Fi initialized");
    return ESP_OK;
}

esp_err_t session_controller_start(void)
{
    ESP_RETURN_ON_ERROR(camera_ble_start(on_credentials), TAG,
                        "Could not start camera BLE");

    if (xTaskCreate(session_task, "camera_session", SESSION_TASK_STACK, NULL,
                    SESSION_TASK_PRIORITY, NULL) != pdPASS ||
        xTaskCreate(uplink_task, "uplink", UPLINK_TASK_STACK, NULL,
                    UPLINK_TASK_PRIORITY, NULL) != pdPASS ||
        xTaskCreate(controller_task, "session_ctrl", CONTROLLER_TASK_STACK,
                    NULL, CONTROLLER_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create session tasks");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "Session controller idle; waiting for a scan request "
             "(%u attempts)",
             (unsigned)CONFIG_SESSION_MAX_ATTEMPTS);
    return ESP_OK;
}

esp_err_t session_controller_request_scan(void)
{
    if (s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state.load() != session::State::idle) {
        return ESP_ERR_INVALID_STATE;
    }
    post_event(make_event(session::Event::scan_requested));
    return ESP_OK;
}

esp_err_t session_controller_request_list(void)
{
    if (s_events == NULL || s_state.load() != session::State::idle) {
        return ESP_ERR_INVALID_STATE;
    }
    QueuedEvent event = make_event(session::Event::scan_requested);
    event.list_only = true;
    post_event(event);
    return ESP_OK;
}

esp_err_t session_controller_request_upload(void)
{
    if (s_events == NULL || s_state.load() != session::State::idle) {
        return ESP_ERR_INVALID_STATE;
    }
    hub_config::Settings settings;
    if (hub_config::load(settings) != ESP_OK ||
        !hub_config::uplink_configured(settings)) {
        return ESP_ERR_NOT_FOUND;
    }
    post_event(make_event(session::Event::upload_requested));
    return ESP_OK;
}

esp_err_t session_controller_request_stop(void)
{
    const session::State state = s_state.load();
    if (s_events == NULL || state == session::State::idle ||
        state == session::State::uplink) {
        return ESP_ERR_INVALID_STATE;
    }
    post_event(make_event(session::Event::stop_requested));
    return ESP_OK;
}

esp_err_t
session_controller_add_status_listener(session_status_listener_t listener,
                                       void *context)
{
    if (listener == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_status_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = ESP_ERR_NO_MEM;
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    for (Listener &slot : s_listeners) {
        if (slot.callback == NULL) {
            slot = {listener, context};
            error = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(s_status_lock);
    return error;
}

void session_controller_get_status(session_status_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_status_lock == NULL) {
        *out = {};
        out->phase_name = session::to_string(session::State::idle);
        out->last_result_name = session::to_string(session::Outcome::none);
        set_front_end_phase(*out, session::State::idle, session::Outcome::none);
        return;
    }
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_status_lock);
}

const char *session_controller_state_name(void)
{
    return session::to_string(s_state.load());
}
