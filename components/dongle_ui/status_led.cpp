/*
 * This module drives the single APA102 LED of the T-Dongle by bit-banging its
 * clock and data lines; one update is 96 bits.
 */

#include "driver/gpio.h"
#include "esp_check.h"
#include "sdkconfig.h"

#include "status_led.h"

#if CONFIG_BOARD_TDONGLE_C5
#define LED_PIN_CLOCK GPIO_NUM_4
#define LED_PIN_DATA GPIO_NUM_5
#else
#define LED_PIN_CLOCK GPIO_NUM_39
#define LED_PIN_DATA GPIO_NUM_40
#endif

static const char *TAG = "trailcamhub_led";

static void send_byte(uint8_t value)
{
    for (int bit = 7; bit >= 0; bit--) {
        gpio_set_level(LED_PIN_DATA, (value >> bit) & 1);
        gpio_set_level(LED_PIN_CLOCK, 1);
        gpio_set_level(LED_PIN_CLOCK, 0);
    }
}

esp_err_t status_led_init(void)
{
    gpio_config_t pins = {};
    pins.pin_bit_mask = (1ULL << LED_PIN_CLOCK) | (1ULL << LED_PIN_DATA);
    pins.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&pins), TAG, "LED pins");
    gpio_set_level(LED_PIN_CLOCK, 0);
    gpio_set_level(LED_PIN_DATA, 0);
    status_led_set(0, 0, 0, 0);
    return ESP_OK;
}

void status_led_set(uint8_t red, uint8_t green, uint8_t blue,
                    uint8_t brightness)
{
    if (brightness > 31) {
        brightness = 31;
    }
    // Start frame, LED frame (brightness, blue, green, red), end frame.
    for (int i = 0; i < 4; i++) {
        send_byte(0x00);
    }
    send_byte(0xe0 | brightness);
    send_byte(blue);
    send_byte(green);
    send_byte(red);
    for (int i = 0; i < 4; i++) {
        send_byte(0xff);
    }
}
