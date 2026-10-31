/*
 * This module runs TrailCamHub cycles. Requests (serial console, timer) and
 * module reports (credentials, Wi-Fi changes, session and uplink end,
 * timeouts) go to a queue; a single task feeds them to the session state
 * machine and executes the resulting actions. The camera session and the
 * backend uplink run in their own tasks because they block.
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
esp_timer_handle_t s_auto_scan_timer;
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

session_status_cb_t s_status_callback;
// Camera Wi-Fi name for the status callback; unlike the password it is kept.
char s_status_ssid[MAX_SSID_LEN + 1];

session_status_t to_status(const session::Result &result)
{
    switch (result.next) {
    case session::State::idle:
        return result.outcome == session::Outcome::camera_scanned ||
                       result.outcome == session::Outcome::uplink_done
                   ? SESSION_STATUS_FINISHED
                   : SESSION_STATUS_IDLE;
    case session::State::ble_activation:
        return SESSION_STATUS_SEARCHING;
    case session::State::wifi_join:
        return SESSION_STATUS_CONNECTING;
    case session::State::session:
        return SESSION_STATUS_CONNECTED;
    case session::State::backoff:
        return SESSION_STATUS_RETRY_WAIT;
    case session::State::uplink_join:
    case session::State::uplink:
        return SESSION_STATUS_UPLOADING;
    }
    return SESSION_STATUS_IDLE;
}

// Reports a state change to the display, the web UI or a console.
void notify_status(const session::Result &result)
{
    if (s_status_callback == NULL) {
        return;
    }
    session_status_info_t info = {};
    info.status = to_status(result);
    if (result.next == session::State::wifi_join ||
        result.next == session::State::session ||
        info.status == SESSION_STATUS_FINISHED) {
        strcpy(info.ssid, s_status_ssid);
    }
    if (result.next == session::State::backoff) {
        info.retry_delay_ms = result.timer_ms;
    }
    s_status_callback(&info);
}

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

void on_auto_scan_timer(void *arg)
{
    (void)arg;
    if (s_state.load() == session::State::idle) {
        post_event(make_event(session::Event::scan_requested));
    }
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
        QueuedEvent event = make_event(session::Event::session_ended);
        event.error = camera_protocol_run(&gateway);
        ESP_LOGI(TAG, "Camera session ended: %s", esp_err_to_name(event.error));
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

// Reads the stored settings when a cycle starts; the uplink only runs if
// they are complete.
void load_settings_for_cycle()
{
    if (hub_config::load(s_settings) != ESP_OK) {
        s_settings = hub_config::Settings{};
    }
    s_fsm.set_uplink_enabled(hub_config::uplink_configured(s_settings));
}

void controller_task(void *arg)
{
    (void)arg;
#if CONFIG_SESSION_SCAN_ON_BOOT
    post_event(make_event(session::Event::scan_requested));
#endif
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
            strcpy(s_status_ssid, event.ssid);
        }
        if (event.type == session::Event::scan_requested && !s_fsm.busy()) {
            s_status_ssid[0] = '\0';
        }
        wipe(event.password, sizeof(event.password));
        if (event.type == session::Event::wifi_connected) {
            s_gateway = event.gateway;
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
        if (result.next != result.previous) {
            console_event("state %s", session::to_string(result.next));
        }
        execute(result);
        notify_status(result);
        if (result.actions & session::kConnectCameraWifi) {
            wipe(s_password, sizeof(s_password));
        }
        if (result.outcome != session::Outcome::none) {
            ESP_LOGI(TAG, "Cycle finished: %s",
                     session::to_string(result.outcome));
            console_event("done %s", session::to_string(result.outcome));
            diagnostics_log_memory("cycle finished");
        }
    }
}

}  // namespace

esp_err_t session_controller_init(void)
{
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
    const esp_timer_create_args_t auto_scan_args = {
        .callback = on_auto_scan_timer,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "auto_scan",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&auto_scan_args, &s_auto_scan_timer),
                        TAG, "Could not create auto-scan timer");

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
#if CONFIG_SESSION_AUTO_SCAN_INTERVAL_MIN > 0
    ESP_RETURN_ON_ERROR(
        esp_timer_start_periodic(
            s_auto_scan_timer,
            (std::uint64_t)CONFIG_SESSION_AUTO_SCAN_INTERVAL_MIN * 60u *
                1000000u),
        TAG, "Could not start auto-scan timer");
#endif
    ESP_LOGI(TAG,
             "Session controller idle; waiting for a scan request "
             "(%u attempts, auto scan every %u min, 0 = off)",
             (unsigned)CONFIG_SESSION_MAX_ATTEMPTS,
             (unsigned)CONFIG_SESSION_AUTO_SCAN_INTERVAL_MIN);
    return ESP_OK;
}

void session_controller_set_status_callback(session_status_cb_t callback)
{
    s_status_callback = callback;
}

void session_controller_request_search(void)
{
    session_controller_request_scan();
}

void session_controller_request_stop(void)
{
    if (s_events != NULL) {
        post_event(make_event(session::Event::user_stop));
    }
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

const char *session_controller_state_name(void)
{
    return session::to_string(s_state.load());
}
