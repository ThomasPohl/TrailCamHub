/*
 * Reads the hub's battery from a MAX17048 fuel gauge over I2C (Waveshare
 * ESP32-S3-A7670E-4G). The gauge is powered by the cell, so without a cell it
 * does not answer and the level stays unknown.
 */

#include "hub_battery.hpp"

#include <cstdint>
#include <mutex>

#include "esp_log.h"
#include "sdkconfig.h"

#include "battery_level.hpp"

#if CONFIG_HUB_BATTERY_MAX17048
#include "driver/i2c_master.h"
#endif

namespace hub_battery
{

namespace
{

const char *TAG = "trailcamhub_battery";

#if CONFIG_HUB_BATTERY_MAX17048

constexpr int kTimeoutMs = 50;
constexpr std::uint32_t kSpeedHz = 100000;

std::mutex s_lock;
bool s_bus_tried = false;
i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_device = nullptr;
// Whether the last read found the gauge, so that a change is logged once.
bool s_last_ok = true;
bool s_version_checked = false;

bool open_bus()
{
    if (s_device != nullptr) {
        return true;
    }
    if (s_bus_tried) {
        return false;
    }
    s_bus_tried = true;

    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = -1;  // any free controller
    bus_config.sda_io_num = (gpio_num_t)CONFIG_HUB_BATTERY_I2C_SDA_GPIO;
    bus_config.scl_io_num = (gpio_num_t)CONFIG_HUB_BATTERY_I2C_SCL_GPIO;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;
    esp_err_t error = i2c_new_master_bus(&bus_config, &s_bus);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not open the I2C bus for the battery gauge: %s",
                 esp_err_to_name(error));
        return false;
    }

    i2c_device_config_t device_config = {};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = kMax17048Address;
    device_config.scl_speed_hz = kSpeedHz;
    error = i2c_master_bus_add_device(s_bus, &device_config, &s_device);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not add the battery gauge: %s",
                 esp_err_to_name(error));
        i2c_del_master_bus(s_bus);
        s_bus = nullptr;
        return false;
    }
    return true;
}

bool read_register(std::uint8_t reg, std::uint16_t &value)
{
    std::uint8_t data[2] = {};
    if (i2c_master_transmit_receive(s_device, &reg, 1, data, sizeof(data),
                                    kTimeoutMs) != ESP_OK) {
        return false;
    }
    value = (std::uint16_t)((data[0] << 8) | data[1]);
    return true;
}

// Reads the gauge; the lock is held.
bool read_gauge(Reading &out)
{
    if (!open_bus()) {
        return false;
    }
    // Probe first: a transfer to a missing device logs an I2C error each time.
    if (i2c_master_probe(s_bus, kMax17048Address, kTimeoutMs) != ESP_OK) {
        return false;
    }
    if (!s_version_checked) {
        std::uint16_t version = 0;
        if (!read_register(kRegVersion, version)) {
            return false;
        }
        if (!is_max17048_version(version)) {
            ESP_LOGW(TAG,
                     "Device at I2C 0x%02x reports version 0x%04x, not a "
                     "MAX17048",
                     kMax17048Address, version);
            return false;
        }
        ESP_LOGI(TAG, "MAX17048 battery gauge found (version 0x%04x)", version);
        s_version_checked = true;
    }
    std::uint16_t vcell = 0;
    std::uint16_t soc = 0;
    if (!read_register(kRegVcell, vcell) || !read_register(kRegSoc, soc)) {
        return false;
    }
    const int millivolts = vcell_to_millivolts(vcell);
    if (!plausible_cell_millivolts(millivolts)) {
        return false;
    }
    out.millivolts = millivolts;
    out.gauge_percent = soc_to_percent(soc);
#if CONFIG_HUB_BATTERY_PERCENT_FROM_VOLTAGE
    out.percent = voltage_to_percent(millivolts, CONFIG_HUB_BATTERY_EMPTY_MV,
                                     CONFIG_HUB_BATTERY_FULL_MV);
#else
    out.percent = out.gauge_percent;
#endif
    return true;
}

#endif  // CONFIG_HUB_BATTERY_MAX17048

}  // namespace

bool supported()
{
#if CONFIG_HUB_BATTERY_MAX17048
    return true;
#else
    return false;
#endif
}

const char *source_name()
{
    return supported() ? "MAX17048" : "none";
}

bool read(Reading &out)
{
    out = Reading{};
#if CONFIG_HUB_BATTERY_MAX17048
    std::lock_guard<std::mutex> guard(s_lock);
    Reading reading;
    const bool ok = read_gauge(reading);
    if (ok != s_last_ok) {
        s_last_ok = ok;
        if (!ok) {
            ESP_LOGW(TAG, "Battery gauge does not answer (no cell inserted?)");
        }
    }
    if (ok) {
        out = reading;
    }
    return ok;
#else
    return false;
#endif
}

int percent()
{
    Reading reading;
    return read(reading) ? reading.percent : -1;
}

void log_reading(const char *stage)
{
    if (!supported()) {
        return;
    }
    Reading reading;
    if (read(reading)) {
        ESP_LOGI(TAG, "Battery [%s]: %d mV, %d %% (gauge %d %%)", stage,
                 reading.millivolts, reading.percent, reading.gauge_percent);
    } else {
        ESP_LOGI(TAG, "Battery [%s]: unknown", stage);
    }
}

}  // namespace hub_battery
