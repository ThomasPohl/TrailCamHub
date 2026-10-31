/*
 * This module shows the camera workflow status on the T-Dongle display and
 * turns presses of its button into search/stop requests. One task
 * polls the button, redraws the screen when its content changes and counts
 * down the retry delay.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "button_detector.hpp"
#include "dongle_ui.h"
#include "screen_renderer.hpp"
#include "st7735_display.h"
#include "status_led.h"
#include "status_screen.hpp"

// The button of the dongle is the BOOT button; it pulls to ground.
#if CONFIG_BOARD_TDONGLE_C5
#define BUTTON_GPIO GPIO_NUM_28
#else
#define BUTTON_GPIO GPIO_NUM_0
#endif
#define BUTTON_POLL_MS 10
#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_LONG_PRESS_MS 1500
#define UI_TASK_STACK 4096
#define UI_TASK_PRIORITY 3
#define MAX_SSID_LEN 32

#define MAX_WEB_TEXT 64

static const char *TAG = "trailcamhub_ui";

namespace
{

// Shows the tone on the RGB LED, or keeps it off at brightness 0.
void show_led(ui::Tone tone)
{
    const uint8_t brightness = CONFIG_DONGLE_LED_BRIGHTNESS;
    switch (tone) {
    case ui::Tone::busy:
        status_led_set(255, 160, 0, brightness);
        return;
    case ui::Tone::ok:
        status_led_set(0, 255, 40, brightness);
        return;
    case ui::Tone::warning:
        status_led_set(255, 60, 0, brightness);
        return;
    case ui::Tone::neutral:
        break;
    }
    status_led_set(255, 255, 255, brightness);
}

SemaphoreHandle_t s_lock;
TaskHandle_t s_task;
dongle_ui_callbacks_t s_callbacks;
uint16_t *s_frame;

// Shared with set_status() under s_lock.
ui::Phase s_phase = ui::Phase::idle;
char s_ssid[MAX_SSID_LEN + 1];
int64_t s_retry_deadline_us;
ui::MediaProgress s_media;
char s_web_ssid[MAX_WEB_TEXT + 1];
char s_web_key[MAX_WEB_TEXT + 1];
char s_web_url[MAX_WEB_TEXT + 1];

static_assert(ST7735_WIDTH == ui::kScreenWidth &&
                  ST7735_HEIGHT == ui::kScreenHeight,
              "renderer and panel sizes differ");

// Draws the screen into the frame, in the byte order of the SPI transfer.
void render(const ui::Screen &screen)
{
    ui::render(screen, s_frame);
    for (int i = 0; i < ST7735_WIDTH * ST7735_HEIGHT; i++) {
        s_frame[i] =
            static_cast<uint16_t>((s_frame[i] << 8) | (s_frame[i] >> 8));
    }
}

ui::Screen current_screen()
{
    ui::Phase phase;
    char ssid[MAX_SSID_LEN + 1];
    int64_t deadline_us;
    ui::MediaProgress media;
    char web_ssid[MAX_WEB_TEXT + 1];
    char web_key[MAX_WEB_TEXT + 1];
    char web_url[MAX_WEB_TEXT + 1];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    phase = s_phase;
    memcpy(ssid, s_ssid, sizeof(ssid));
    deadline_us = s_retry_deadline_us;
    media = s_media;
    memcpy(web_ssid, s_web_ssid, sizeof(web_ssid));
    memcpy(web_key, s_web_key, sizeof(web_key));
    memcpy(web_url, s_web_url, sizeof(web_url));
    xSemaphoreGive(s_lock);

    uint32_t retry_in_s = 0;
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us > 0) {
        retry_in_s = static_cast<uint32_t>((remaining_us + 999999) / 1000000);
    }
    return ui::make_screen(phase, ssid, retry_in_s, media,
                           {web_ssid, web_key, web_url});
}

void ui_task(void *arg)
{
    (void)arg;
    ui::ButtonDetector button(BUTTON_DEBOUNCE_MS, BUTTON_LONG_PRESS_MS);
    ui::Screen shown = {};
    bool has_shown = false;

    while (true) {
        const ui::Screen screen = current_screen();
        if (!has_shown || !ui::same_content(screen, shown)) {
            if (!has_shown || screen.tone != shown.tone) {
                show_led(screen.tone);
            }
            render(screen);
            if (st7735_present(s_frame) == ESP_OK) {
                shown = screen;
                has_shown = true;
            } else {
                ESP_LOGW(TAG, "Could not update the display");
            }
        }

        const uint32_t now_ms =
            static_cast<uint32_t>(esp_timer_get_time() / 1000);
        switch (button.update(gpio_get_level(BUTTON_GPIO) == 0, now_ms)) {
        case ui::ButtonDetector::Event::short_press:
            ESP_LOGI(TAG, "Button: search for the camera");
            if (s_callbacks.on_search != NULL) {
                s_callbacks.on_search();
            }
            break;
        case ui::ButtonDetector::Event::long_press:
            ESP_LOGI(TAG, "Button: stop");
            if (s_callbacks.on_stop != NULL) {
                s_callbacks.on_stop();
            }
            break;
        case ui::ButtonDetector::Event::none:
            break;
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

ui::Phase to_ui_phase(dongle_ui_phase_t phase)
{
    switch (phase) {
    case DONGLE_UI_SEARCHING:
        return ui::Phase::searching;
    case DONGLE_UI_CONNECTING:
        return ui::Phase::connecting;
    case DONGLE_UI_CONNECTED:
        return ui::Phase::connected;
    case DONGLE_UI_RETRY_WAIT:
        return ui::Phase::retry_wait;
    case DONGLE_UI_FINISHED:
        return ui::Phase::finished;
    case DONGLE_UI_IDLE:
        break;
    }
    return ui::Phase::idle;
}

}  // namespace

esp_err_t dongle_ui_start(const dongle_ui_callbacks_t *callbacks)
{
    if (callbacks != NULL) {
        s_callbacks = *callbacks;
    }
    s_lock = xSemaphoreCreateMutex();
    s_frame = static_cast<uint16_t *>(
        heap_caps_malloc(ST7735_WIDTH * ST7735_HEIGHT * sizeof(uint16_t),
                         MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (s_lock == NULL || s_frame == NULL) {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t button = {};
    button.pin_bit_mask = 1ULL << BUTTON_GPIO;
    button.mode = GPIO_MODE_INPUT;
    button.pull_up_en = GPIO_PULLUP_ENABLE;
    esp_err_t error = gpio_config(&button);
    if (error != ESP_OK) {
        return error;
    }
    // The LED is bright until it gets data, so switch it off first.
    error = status_led_init();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not set up the RGB LED: %s",
                 esp_err_to_name(error));
    }
    error = st7735_init();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not initialize the display: %s",
                 esp_err_to_name(error));
        return error;
    }
    if (xTaskCreate(ui_task, "dongle_ui", UI_TASK_STACK, NULL, UI_TASK_PRIORITY,
                    &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void dongle_ui_set_status(dongle_ui_phase_t phase, const char *ssid,
                          uint32_t retry_delay_ms)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // The result stays visible on the finished screen until the next search.
    if (phase == DONGLE_UI_SEARCHING && s_phase != ui::Phase::searching) {
        s_media = {0, 0, 0};
    }
    s_phase = to_ui_phase(phase);
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid != NULL ? ssid : "");
    s_retry_deadline_us =
        esp_timer_get_time() + static_cast<int64_t>(retry_delay_ms) * 1000;
    xSemaphoreGive(s_lock);
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

void dongle_ui_set_media_progress(uint32_t done, uint32_t stored,
                                  uint32_t total)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_media = {done, stored, total};
    xSemaphoreGive(s_lock);
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

void dongle_ui_set_web_access(const char *ssid, const char *key,
                              const char *url)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_web_ssid, sizeof(s_web_ssid), "%s", ssid);
    snprintf(s_web_key, sizeof(s_web_key), "%s", key);
    snprintf(s_web_url, sizeof(s_web_url), "%s", url);
    xSemaphoreGive(s_lock);
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}
