#include "ssd1306.hpp"

#include "driver/i2c_master.h"
#include "esp_log.h"

namespace ssd1306
{

namespace
{

const char *TAG = "ssd1306";
constexpr int kTimeoutMs = 100;
constexpr std::uint32_t kSpeedHz = 400000;
constexpr int kWidth = 128;
constexpr int kPages = 8;

i2c_master_dev_handle_t s_device = nullptr;

// Control byte 0x00: the bytes that follow are commands.
esp_err_t commands(const std::uint8_t *data, std::size_t size)
{
    std::uint8_t buffer[32];
    if (size + 1 > sizeof(buffer)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buffer[0] = 0x00;
    for (std::size_t i = 0; i < size; i++) {
        buffer[i + 1] = data[i];
    }
    return i2c_master_transmit(s_device, buffer, size + 1, kTimeoutMs);
}

}  // namespace

esp_err_t init(void *bus, std::uint8_t address)
{
    if (s_device != nullptr) {
        return ESP_OK;
    }
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = address;
    config.scl_speed_hz = kSpeedHz;
    esp_err_t error = i2c_master_bus_add_device(
        static_cast<i2c_master_bus_handle_t>(bus), &config, &s_device);
    if (error != ESP_OK) {
        return error;
    }
    // Display off, clock, multiplex 64, offset 0, start line 0, charge pump
    // on, horizontal addressing, segment remap and COM scan reversed (upright
    // with the pins at the top), COM pins, contrast, pre-charge, VCOM level,
    // follow RAM, normal (not inverted), display on.
    static const std::uint8_t init[] = {
        0xae, 0xd5, 0x80, 0xa8, 0x3f, 0xd3, 0x00, 0x40, 0x8d,
        0x14, 0x20, 0x00, 0xa1, 0xc8, 0xda, 0x12, 0x81, 0xcf,
        0xd9, 0xf1, 0xdb, 0x40, 0xa4, 0xa6, 0xaf,
    };
    // Two chunks: the buffer of commands() is small.
    error = commands(init, 12);
    if (error == ESP_OK) {
        error = commands(init + 12, sizeof(init) - 12);
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "No SSD1306 at 0x%02x: %s", address,
                 esp_err_to_name(error));
        i2c_master_bus_rm_device(s_device);
        s_device = nullptr;
    }
    return error;
}

esp_err_t present(const std::uint8_t *frame, std::size_t size)
{
    if (s_device == nullptr ||
        size != static_cast<std::size_t>(kWidth) * kPages) {
        return ESP_ERR_INVALID_STATE;
    }
    // Whole frame: column 0..127, page 0..7, one page per transfer.
    static const std::uint8_t window[] = {0x21, 0x00, 0x7f, 0x22, 0x00, 0x07};
    esp_err_t error = commands(window, sizeof(window));
    for (int page = 0; error == ESP_OK && page < kPages; page++) {
        std::uint8_t buffer[kWidth + 1];
        buffer[0] = 0x40;  // control byte: display data follows
        for (int x = 0; x < kWidth; x++) {
            buffer[x + 1] = frame[page * kWidth + x];
        }
        error =
            i2c_master_transmit(s_device, buffer, sizeof(buffer), kTimeoutMs);
    }
    return error;
}

}  // namespace ssd1306
