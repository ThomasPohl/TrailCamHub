#include "boot_button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "button_detector.hpp"

#define BUTTON_POLL_MS 10
#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_TASK_STACK 3072
#define BUTTON_TASK_PRIORITY 3

static const char *TAG = "boot_button";

namespace
{

boot_button_callbacks_t s_callbacks;

void button_task(void *arg)
{
    (void)arg;
    ui::ButtonDetector detector(BUTTON_DEBOUNCE_MS,
                                CONFIG_HUB_BUTTON_LONG_PRESS_MS);
    const gpio_num_t pin = static_cast<gpio_num_t>(CONFIG_HUB_BUTTON_GPIO);
    while (true) {
        const bool pressed = gpio_get_level(pin) == 0;
        const uint32_t now_ms =
            static_cast<uint32_t>(esp_timer_get_time() / 1000);
        switch (detector.update(pressed, now_ms)) {
        case ui::ButtonDetector::Event::short_press:
            ESP_LOGI(TAG, "Short press: search for the cameras");
            if (s_callbacks.on_search != nullptr) {
                s_callbacks.on_search();
            }
            break;
        case ui::ButtonDetector::Event::long_press:
            ESP_LOGI(TAG, "Long press: stop");
            if (s_callbacks.on_stop != nullptr) {
                s_callbacks.on_stop();
            }
            break;
        case ui::ButtonDetector::Event::none:
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

}  // namespace

esp_err_t boot_button_start(const boot_button_callbacks_t *callbacks)
{
    if (callbacks != nullptr) {
        s_callbacks = *callbacks;
    }
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << CONFIG_HUB_BUTTON_GPIO;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO %d: %s", CONFIG_HUB_BUTTON_GPIO,
                 esp_err_to_name(err));
        return err;
    }
    if (xTaskCreate(button_task, "boot_button", BUTTON_TASK_STACK, nullptr,
                    BUTTON_TASK_PRIORITY, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Button on GPIO %d: short press = search, long press = stop",
             CONFIG_HUB_BUTTON_GPIO);
    return ESP_OK;
}
