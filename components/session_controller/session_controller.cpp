/*
 * This module runs the camera workflow. Modules report events (credentials,
 * Wi-Fi changes, session end, timeouts) to a queue; a single task feeds them
 * to the session state machine and executes the resulting actions.
 */

#include <atomic>
#include <cstdint>
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
#include "session_controller.h"
#include "session_fsm.hpp"
#include "system_diagnostics.h"

#ifndef CONFIG_SESSION_WIFI_JOIN_TIMEOUT_S
#error "main/Kconfig.projbuild was not processed"
#endif

#define EVENT_QUEUE_LENGTH 8
#define CONTROLLER_TASK_STACK 4096
#define SESSION_TASK_STACK 6144
#define CONTROLLER_TASK_PRIORITY 5
#define SESSION_TASK_PRIORITY 5
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
    CONFIG_SESSION_WIFI_JOIN_TIMEOUT_S * 1000u,
    CONFIG_SESSION_RETRY_DELAY_MIN_S * 1000u,
    CONFIG_SESSION_RETRY_DELAY_MAX_S * 1000u,
};

QueueHandle_t s_events;
QueueHandle_t s_session_requests;
esp_timer_handle_t s_timer;
std::atomic<std::uint32_t> s_timer_generation;
session::Fsm s_fsm(kConfig);

// Credentials and gateway used by the next actions.
char s_ssid[MAX_SSID_LEN + 1];
char s_password[MAX_PASSWORD_LEN + 1];
esp_ip4_addr_t s_gateway;

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
    }
    if (actions & session::kStopSession) {
        camera_protocol_request_stop();
    }
    if (actions & session::kDisconnectWifi) {
        camera_wifi_disconnect();
    }
    if (actions & session::kResumeBle) {
        camera_ble_resume();
    }
    if (actions & session::kConnectWifi) {
        esp_err_t error = camera_wifi_connect(s_ssid, s_password);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Could not join camera Wi-Fi: %s",
                     esp_err_to_name(error));
            on_wifi_failed(0);
        }
    }
    if (actions & session::kStartSession) {
        if (xQueueOverwrite(s_session_requests, &s_gateway) != pdTRUE) {
            ESP_LOGE(TAG, "Could not start camera session");
        }
    }
    if (actions & session::kStartTimer) {
        start_timer(result.timer_ms);
    }
}

void controller_task(void *arg)
{
    (void)arg;
    post_event(make_event(session::Event::start));
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
            ESP_LOGI(TAG, "Retrying camera activation in %u s",
                     (unsigned)(result.timer_ms / 1000));
        }
        execute(result);
        if (result.actions & session::kConnectWifi) {
            wipe(s_password, sizeof(s_password));
        }
    }
}

}  // namespace

esp_err_t session_controller_init(void)
{
    s_events = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(QueuedEvent));
    s_session_requests = xQueueCreate(1, sizeof(esp_ip4_addr_t));
    if (s_events == NULL || s_session_requests == NULL) {
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
        xTaskCreate(controller_task, "session_ctrl", CONTROLLER_TASK_STACK,
                    NULL, CONTROLLER_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create session tasks");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "Session controller started (Wi-Fi join timeout %u s, "
             "retry delay %u-%u s)",
             (unsigned)CONFIG_SESSION_WIFI_JOIN_TIMEOUT_S,
             (unsigned)CONFIG_SESSION_RETRY_DELAY_MIN_S,
             (unsigned)CONFIG_SESSION_RETRY_DELAY_MAX_S);
    return ESP_OK;
}
