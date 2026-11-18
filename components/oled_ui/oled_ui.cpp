/*
 * Status display on an SSD1306 OLED (#224): one task redraws the screen when
 * its content changes. The application reports the phase as for the dongle's
 * display; the texts come from the same code (status_screen.cpp).
 */

#include "oled_ui.h"

#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "hub_battery.hpp"
#include "oled_renderer.hpp"
#include "ssd1306.hpp"
#include "status_screen.hpp"

#define OLED_TASK_STACK 4096
#define OLED_TASK_PRIORITY 2
#define OLED_TICK_MS 1000
#define MAX_SSID_LEN 32

static const char *TAG = "oled_ui";

namespace
{

SemaphoreHandle_t s_lock;
TaskHandle_t s_task;
ui::Phase s_phase = ui::Phase::idle;
char s_ssid[MAX_SSID_LEN + 1];
int64_t s_retry_deadline_us;
ui::MediaProgress s_media;

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
    case DONGLE_UI_LISTED:
        return ui::Phase::listed;
    case DONGLE_UI_PARTIAL:
        return ui::Phase::partial;
    case DONGLE_UI_NOT_FOUND:
        return ui::Phase::not_found;
    case DONGLE_UI_NO_CARD:
        return ui::Phase::no_card;
    case DONGLE_UI_UPLOAD_FAILED:
        return ui::Phase::upload_failed;
    case DONGLE_UI_STOPPED:
        return ui::Phase::stopped;
    case DONGLE_UI_IDLE:
        break;
    }
    return ui::Phase::idle;
}

ui::Screen current_screen()
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const ui::Phase phase = s_phase;
    char ssid[MAX_SSID_LEN + 1];
    memcpy(ssid, s_ssid, sizeof(ssid));
    const int64_t deadline_us = s_retry_deadline_us;
    const ui::MediaProgress media = s_media;
    xSemaphoreGive(s_lock);

    uint32_t retry_in_s = 0;
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us > 0) {
        retry_in_s = static_cast<uint32_t>((remaining_us + 999999) / 1000000);
    }
    return ui::make_screen(phase, ssid, retry_in_s, media);
}

void oled_task(void *arg)
{
    (void)arg;
    uint8_t frame[ui::kOledFrameSize];
    ui::Screen shown = {};
    bool has_shown = false;
    while (true) {
        const ui::Screen screen = current_screen();
        if (!has_shown || !ui::same_content(screen, shown)) {
            ui::render_oled(screen, frame);
            if (ssd1306::present(frame, sizeof(frame)) == ESP_OK) {
                shown = screen;
                has_shown = true;
            } else {
                ESP_LOGW(TAG, "Could not update the OLED");
            }
        }
        // The retry countdown changes the text every second.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(OLED_TICK_MS));
    }
}

}  // namespace

esp_err_t oled_ui_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    void *bus = nullptr;
#if CONFIG_HUB_OLED_SHARE_BATTERY_BUS
    bus = hub_battery::i2c_bus();
    if (bus == nullptr) {
        ESP_LOGE(TAG, "The battery gauge's I2C bus is not available");
        return ESP_ERR_INVALID_STATE;
    }
#else
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = -1;
    bus_config.sda_io_num = (gpio_num_t)CONFIG_HUB_OLED_I2C_SDA_GPIO;
    bus_config.scl_io_num = (gpio_num_t)CONFIG_HUB_OLED_I2C_SCL_GPIO;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t handle = nullptr;
    esp_err_t error = i2c_new_master_bus(&bus_config, &handle);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus: %s", esp_err_to_name(error));
        return error;
    }
    bus = handle;
#endif
    esp_err_t error2 = ssd1306::init(bus, CONFIG_HUB_OLED_I2C_ADDRESS);
    if (error2 != ESP_OK) {
        return error2;
    }
    if (xTaskCreate(oled_task, "oled_ui", OLED_TASK_STACK, NULL,
                    OLED_TASK_PRIORITY, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "OLED at I2C 0x%02x", CONFIG_HUB_OLED_I2C_ADDRESS);
    return ESP_OK;
}

void oled_ui_set_status(dongle_ui_phase_t phase, const char *ssid,
                        uint32_t retry_delay_ms)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
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

void oled_ui_set_media_progress(uint32_t done, uint32_t stored, uint32_t total)
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
