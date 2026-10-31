/*
 * This module drives the 0.96" ST7735S panel (80x160) of the LilyGO
 * T-Dongle-S3 and T-Dongle-C5 over SPI with the esp_lcd panel IO layer. Both
 * boards use the same panel; only the wiring differs.
 */

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "sd_card.h"
#include "st7735_display.h"

#define LCD_SPI_HOST SPI2_HOST
#if CONFIG_BOARD_TDONGLE_C5
// T-Dongle-C5 wiring. The microSD card shares the bus (MISO GPIO7, CS
// GPIO23); whichever driver starts first initializes it.
#define LCD_PIN_MOSI GPIO_NUM_2
#define LCD_PIN_MISO GPIO_NUM_7
#define LCD_PIN_SCLK GPIO_NUM_6
#define LCD_PIN_CS GPIO_NUM_10
#define LCD_PIN_DC GPIO_NUM_3
#define LCD_PIN_RST GPIO_NUM_1
// Active low.
#define LCD_PIN_BACKLIGHT GPIO_NUM_0
#else
// T-Dongle-S3 wiring.
#define LCD_PIN_MOSI GPIO_NUM_3
#define LCD_PIN_MISO GPIO_NUM_NC
#define LCD_PIN_SCLK GPIO_NUM_5
#define LCD_PIN_CS GPIO_NUM_4
#define LCD_PIN_DC GPIO_NUM_2
#define LCD_PIN_RST GPIO_NUM_1
// Active low.
#define LCD_PIN_BACKLIGHT GPIO_NUM_38
#endif
#define LCD_SPI_CLOCK_HZ (20 * 1000 * 1000)

// The 80x160 window sits inside the 132x162 RAM of the controller; the offsets
// are those of the landscape orientation.
#define LCD_X_GAP 1
#define LCD_Y_GAP 26
// MY | MV, BGR panel order.
#define LCD_MADCTL 0xA8

#define ST7735_SWRESET 0x01
#define ST7735_SLPOUT 0x11
#define ST7735_NORON 0x13
#define ST7735_INVON 0x21
#define ST7735_DISPON 0x29
#define ST7735_CASET 0x2A
#define ST7735_RASET 0x2B
#define ST7735_RAMWR 0x2C
#define ST7735_MADCTL 0x36
#define ST7735_COLMOD 0x3A

static const char *TAG = "trailcamhub_lcd";
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_transfer_done;

static bool on_transfer_done(esp_lcd_panel_io_handle_t io,
                             esp_lcd_panel_io_event_data_t *event, void *ctx)
{
    (void)io;
    (void)event;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(ctx), &woken);
    return woken == pdTRUE;
}

static esp_err_t command(uint8_t cmd, const uint8_t *data, size_t length)
{
    return esp_lcd_panel_io_tx_param(s_io, cmd, data, length);
}

static esp_err_t send_init_sequence(void)
{
    static const uint8_t frame_rate[] = {0x01, 0x2c, 0x2d};
    static const uint8_t frame_rate_partial[] = {0x01, 0x2c, 0x2d,
                                                 0x01, 0x2c, 0x2d};
    static const uint8_t inversion[] = {0x07};
    static const uint8_t power1[] = {0xa2, 0x02, 0x84};
    static const uint8_t power2[] = {0xc5};
    static const uint8_t power3[] = {0x0a, 0x00};
    static const uint8_t power4[] = {0x8a, 0x2a};
    static const uint8_t power5[] = {0x8a, 0xee};
    static const uint8_t vcom[] = {0x0e};
    static const uint8_t madctl[] = {LCD_MADCTL};
    static const uint8_t colmod[] = {0x05};
    static const uint8_t gamma_positive[] = {0x02, 0x1c, 0x07, 0x12, 0x37, 0x32,
                                             0x29, 0x2d, 0x29, 0x25, 0x2b, 0x39,
                                             0x00, 0x01, 0x03, 0x10};
    static const uint8_t gamma_negative[] = {0x03, 0x1d, 0x07, 0x06, 0x2e, 0x2c,
                                             0x29, 0x2d, 0x2e, 0x2e, 0x37, 0x3f,
                                             0x00, 0x00, 0x02, 0x10};

    ESP_RETURN_ON_ERROR(command(ST7735_SWRESET, NULL, 0), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_RETURN_ON_ERROR(command(ST7735_SLPOUT, NULL, 0), TAG, "sleep out");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(command(0xb1, frame_rate, sizeof(frame_rate)), TAG,
                        "init");
    ESP_RETURN_ON_ERROR(command(0xb2, frame_rate, sizeof(frame_rate)), TAG,
                        "init");
    ESP_RETURN_ON_ERROR(
        command(0xb3, frame_rate_partial, sizeof(frame_rate_partial)), TAG,
        "init");
    ESP_RETURN_ON_ERROR(command(0xb4, inversion, sizeof(inversion)), TAG,
                        "init");
    ESP_RETURN_ON_ERROR(command(0xc0, power1, sizeof(power1)), TAG, "init");
    ESP_RETURN_ON_ERROR(command(0xc1, power2, sizeof(power2)), TAG, "init");
    ESP_RETURN_ON_ERROR(command(0xc2, power3, sizeof(power3)), TAG, "init");
    ESP_RETURN_ON_ERROR(command(0xc3, power4, sizeof(power4)), TAG, "init");
    ESP_RETURN_ON_ERROR(command(0xc4, power5, sizeof(power5)), TAG, "init");
    ESP_RETURN_ON_ERROR(command(0xc5, vcom, sizeof(vcom)), TAG, "init");
    ESP_RETURN_ON_ERROR(command(ST7735_INVON, NULL, 0), TAG, "inversion");
    ESP_RETURN_ON_ERROR(command(ST7735_MADCTL, madctl, sizeof(madctl)), TAG,
                        "orientation");
    ESP_RETURN_ON_ERROR(command(ST7735_COLMOD, colmod, sizeof(colmod)), TAG,
                        "pixel format");
    ESP_RETURN_ON_ERROR(command(0xe0, gamma_positive, sizeof(gamma_positive)),
                        TAG, "gamma");
    ESP_RETURN_ON_ERROR(command(0xe1, gamma_negative, sizeof(gamma_negative)),
                        TAG, "gamma");
    ESP_RETURN_ON_ERROR(command(ST7735_NORON, NULL, 0), TAG, "normal mode");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(command(ST7735_DISPON, NULL, 0), TAG, "display on");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t st7735_init(void)
{
    s_transfer_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_transfer_done != NULL, ESP_ERR_NO_MEM, TAG,
                        "no memory");

    gpio_config_t pins = {};
    pins.pin_bit_mask = (1ULL << LCD_PIN_RST) | (1ULL << LCD_PIN_BACKLIGHT);
    pins.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&pins), TAG, "GPIO setup");
    // Keep the backlight off until the first frame is drawn.
    gpio_set_level(LCD_PIN_BACKLIGHT, 1);

    spi_bus_config_t bus = {};
    bus.mosi_io_num = LCD_PIN_MOSI;
    bus.miso_io_num = LCD_PIN_MISO;
    bus.sclk_io_num = LCD_PIN_SCLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = ST7735_WIDTH * ST7735_HEIGHT * 2;
    // On the T-Dongle-C5 the SD card driver sets up the shared bus (with a
    // large enough maximum transfer size) unless the card is disabled.
    if (!sd_card_spi_bus_ready()) {
        ESP_RETURN_ON_ERROR(
            spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG,
            "SPI bus");
    }

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = LCD_PIN_CS;
    io_config.dc_gpio_num = LCD_PIN_DC;
    io_config.spi_mode = 0;
    io_config.pclk_hz = LCD_SPI_CLOCK_HZ;
    io_config.trans_queue_depth = 1;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.on_color_trans_done = on_transfer_done;
    io_config.user_ctx = s_transfer_done;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
                            static_cast<esp_lcd_spi_bus_handle_t>(LCD_SPI_HOST),
                            &io_config, &s_io),
                        TAG, "panel IO");

    gpio_set_level(LCD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    return send_init_sequence();
}

esp_err_t st7735_present(const uint16_t *frame)
{
    const uint16_t x_end = LCD_X_GAP + ST7735_WIDTH - 1;
    const uint16_t y_end = LCD_Y_GAP + ST7735_HEIGHT - 1;
    const uint8_t columns[] = {LCD_X_GAP >> 8, LCD_X_GAP & 0xff,
                               static_cast<uint8_t>(x_end >> 8),
                               static_cast<uint8_t>(x_end & 0xff)};
    const uint8_t rows[] = {LCD_Y_GAP >> 8, LCD_Y_GAP & 0xff,
                            static_cast<uint8_t>(y_end >> 8),
                            static_cast<uint8_t>(y_end & 0xff)};
    ESP_RETURN_ON_ERROR(command(ST7735_CASET, columns, sizeof(columns)), TAG,
                        "column range");
    ESP_RETURN_ON_ERROR(command(ST7735_RASET, rows, sizeof(rows)), TAG,
                        "row range");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_color(s_io, ST7735_RAMWR, frame,
                                  ST7735_WIDTH * ST7735_HEIGHT * 2),
        TAG, "frame transfer");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_transfer_done, pdMS_TO_TICKS(1000)) ==
                            pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "frame transfer timed out");
    gpio_set_level(LCD_PIN_BACKLIGHT, 0);
    return ESP_OK;
}
