/*
 * Reads commands from the console port (UART or USB-Serial-JTAG) and executes
 * them: settings are stored through hub_config, scans and uploads are handed
 * to the session controller, which reports their progress as "TCH EVENT"
 * lines, and the diagnostic commands look at the memory and the SD card.
 */

#include "hub_console.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_ESP_CONSOLE_UART
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif

#include "command_parser.hpp"
#include "diagnostics.hpp"
#include "hub_config.hpp"
#include "sd_card.h"
#include "session_controller.h"

#define CONSOLE_TASK_STACK 6144
#define CONSOLE_TASK_PRIORITY 4
#define CONSOLE_LINE_MAX 512
#define CONSOLE_RX_BUFFER 1024
// The USB-Serial-JTAG driver's buffers are in internal RAM. It drops output
// when the buffer stays full for 50 ms; "cat" readers find the gaps by the
// offsets. With 1 KB the log lines of a BLE stack start and of a camera found
// right after it (about 2 KB within 100 ms) were partly lost.
#define CONSOLE_USB_TX_BUFFER 2048
#define CONSOLE_USB_RX_BUFFER 256

namespace hub_console
{

void reply(const char *kind, const std::string &message)
{
    printf("TCH %s %s\n", kind, message.c_str());
    fflush(stdout);
}

}  // namespace hub_console

using hub_console::reply;

namespace
{

const char *TAG = "trailcamhub_console";

SemaphoreHandle_t s_lock;
// Shared with the setters under s_lock.
char s_phase[16] = "idle";
char s_camera_ssid[33];
int64_t s_retry_deadline_us;
uint32_t s_media_done;
uint32_t s_media_stored;
uint32_t s_media_total;

// True while a cycle uses the SD card: the camera session writes it, the
// backend sync reads it.
bool card_busy()
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool busy = strcmp(s_phase, "connecting") == 0 ||
                      strcmp(s_phase, "connected") == 0 ||
                      strcmp(s_phase, "uploading") == 0;
    xSemaphoreGive(s_lock);
    return busy;
}

std::string format_line(const char *format, ...)
    __attribute__((format(printf, 1, 2)));
std::string format_line(const char *format, ...)
{
    char text[120];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return text;
}

// The cycle's phase, the download progress, the memory and the SD card.
void print_details()
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    reply("INFO", std::string("phase ") + s_phase);
    reply("INFO", std::string("camera ") + s_camera_ssid);
    if (strcmp(s_phase, "retry_wait") == 0) {
        const int64_t left_us = s_retry_deadline_us - esp_timer_get_time();
        reply("INFO",
              format_line("retry in %lld s",
                          (long long)(left_us > 0 ? left_us / 1000000 : 0)));
    }
    reply("INFO",
          format_line("media %lu of %lu done, %lu stored",
                      (unsigned long)s_media_done, (unsigned long)s_media_total,
                      (unsigned long)s_media_stored));
    xSemaphoreGive(s_lock);
    reply("INFO", format_line("uptime %lld s",
                              (long long)(esp_timer_get_time() / 1000000)));
    reply("INFO",
          format_line(
              "internal RAM %u KB free, %u KB minimum",
              (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
              (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) /
                         1024)));
    reply("INFO",
          format_line(
              "PSRAM %u KB free, %u KB minimum",
              (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
              (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) /
                         1024)));
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(SD_CARD_MOUNT_POINT, &total, &free_bytes) == ESP_OK) {
        reply("INFO", format_line("SD card %llu MB free of %llu MB",
                                  (unsigned long long)(free_bytes >> 20),
                                  (unsigned long long)(total >> 20)));
    } else {
        reply("INFO", "SD card not mounted");
    }
}

void print_settings()
{
    hub_config::Settings settings;
    if (hub_config::load(settings) != ESP_OK) {
        reply("ERR", "could not read settings");
        return;
    }
    for (hub_config::Key key : hub_config::kAllKeys) {
        reply("CONFIG", std::string(hub_config::key_name(key)) + " " +
                            hub_config::display_value(
                                key, hub_config::field(settings, key)));
    }
    reply("CONFIG",
          std::string("uplink ") + (hub_config::uplink_configured(settings)
                                        ? "configured"
                                        : "incomplete"));
}

void print_status()
{
    reply("STATE", session_controller_state_name());
    print_settings();
    print_details();
}

void request_cycle(esp_err_t error, const char *what)
{
    switch (error) {
    case ESP_OK:
        reply("OK", std::string(what) + " started");
        break;
    case ESP_ERR_INVALID_STATE:
        reply("ERR",
              std::string("busy (") + session_controller_state_name() + ")");
        break;
    case ESP_ERR_NOT_FOUND:
        reply("ERR", "uplink settings incomplete (see 'config show')");
        break;
    default:
        reply("ERR", std::string(what) + " failed: " + esp_err_to_name(error));
        break;
    }
}

void set_setting(const hub_console::Command &command)
{
    hub_config::Key key;
    if (!hub_config::key_from_name(command.key, key)) {
        reply("ERR", "unknown setting '" + command.key + "'");
        return;
    }
    std::string reason;
    const esp_err_t error = hub_config::set(key, command.value, &reason);
    if (error == ESP_OK) {
        reply("OK", std::string(hub_config::key_name(key)) +
                        (command.value.empty() ? " erased" : " stored"));
    } else if (error == ESP_ERR_INVALID_ARG) {
        reply("ERR", reason);
    } else {
        reply("ERR", std::string("could not store setting: ") +
                         esp_err_to_name(error));
    }
}

void run_diagnostic(const hub_console::Command &command)
{
#if CONFIG_HUB_CONSOLE_DIAGNOSTICS
    using hub_console::CommandType;
    switch (command.type) {
    case CommandType::heap:
        hub_console::run_heap(command.blocks);
        break;
    case CommandType::ls:
        hub_console::run_ls(command.path);
        break;
    case CommandType::cat:
        hub_console::run_cat(command.path, command.offset, command.length);
        break;
    case CommandType::rm:
        hub_console::run_rm(command.path, command.recursive, card_busy());
        break;
    case CommandType::sdbench:
        hub_console::run_sdbench(command.total_kb, command.chunk_kb,
                                 card_busy());
        break;
    default:
        break;
    }
#else
    (void)command;
    reply("ERR", "diagnostic commands are not in this build");
#endif
}

void execute(const hub_console::Command &command)
{
    using hub_console::CommandType;
    switch (command.type) {
    case CommandType::empty:
        break;
    case CommandType::help: {
        const std::string text = hub_console::help_text();
        std::size_t start = 0;
        std::size_t end;
        while ((end = text.find('\n', start)) != std::string::npos) {
            reply("HELP", text.substr(start, end - start));
            start = end + 1;
        }
        break;
    }
    case CommandType::status:
        print_status();
        break;
    case CommandType::scan:
        request_cycle(session_controller_request_scan(), "scan");
        break;
    case CommandType::upload:
        request_cycle(session_controller_request_upload(), "upload");
        break;
    case CommandType::stop:
        session_controller_request_stop();
        reply("OK", "stop requested");
        break;
    case CommandType::heap:
    case CommandType::ls:
    case CommandType::cat:
    case CommandType::rm:
    case CommandType::sdbench:
        run_diagnostic(command);
        break;
    case CommandType::config_show:
        print_settings();
        break;
    case CommandType::config_clear: {
        const esp_err_t error = hub_config::clear();
        if (error == ESP_OK) {
            reply("OK", "settings erased");
        } else {
            reply("ERR", std::string("could not erase settings: ") +
                             esp_err_to_name(error));
        }
        break;
    }
    case CommandType::config_set:
        set_setting(command);
        break;
    case CommandType::invalid:
        reply("ERR", command.error);
        break;
    }
}

void console_task(void *arg)
{
    (void)arg;
    static char line[CONSOLE_LINE_MAX];
    while (true) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        const size_t length = strlen(line);
        if (length == sizeof(line) - 1 && line[length - 1] != '\n') {
            // Overlong line: drop the rest of it and reject the command.
            int c;
            while ((c = fgetc(stdin)) != '\n' && c != EOF) {
            }
            reply("ERR", "line too long");
            continue;
        }
        const hub_console::Command command = hub_console::parse_command(line);
        // The line may hold a secret (password, device key).
        memset(line, 0, sizeof(line));
        if (command.type == hub_console::CommandType::empty) {
            continue;
        }
        execute(command);
        // Ends every reply, so that a script knows when to stop reading.
        reply("DONE", "");
    }
}

// Makes blocking reads on stdin possible: the console port's driver behind
// the console VFS.
esp_err_t install_console_driver()
{
#if CONFIG_ESP_CONSOLE_UART
    const uart_port_t port = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;
    esp_err_t error =
        uart_driver_install(port, CONSOLE_RX_BUFFER, 0, 0, NULL, 0);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not install the console UART driver: %s",
                 esp_err_to_name(error));
        return error;
    }
    uart_vfs_dev_port_set_rx_line_endings(port, ESP_LINE_ENDINGS_CRLF);
    uart_vfs_dev_use_driver(port);
    return ESP_OK;
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    // The log output then goes through the driver as well; it drops output
    // while no computer reads it instead of blocking.
    usb_serial_jtag_driver_config_t config =
        USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    config.tx_buffer_size = CONSOLE_USB_TX_BUFFER;
    config.rx_buffer_size = CONSOLE_USB_RX_BUFFER;
    esp_err_t error = usb_serial_jtag_driver_install(&config);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not install the USB-Serial-JTAG driver: %s",
                 esp_err_to_name(error));
        return error;
    }
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CRLF);
    usb_serial_jtag_vfs_use_driver();
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

}  // namespace

esp_err_t hub_console_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(install_console_driver(), TAG, "console driver");
    // The stack goes to PSRAM where there is some, to spare internal RAM.
    BaseType_t created = pdFAIL;
#if CONFIG_SPIRAM
    created = xTaskCreateWithCaps(
        console_task, "hub_console", CONSOLE_TASK_STACK, NULL,
        CONSOLE_TASK_PRIORITY, NULL, MALLOC_CAP_SPIRAM);
#endif
    if (created != pdPASS) {
        created = xTaskCreate(console_task, "hub_console", CONSOLE_TASK_STACK,
                              NULL, CONSOLE_TASK_PRIORITY, NULL);
    }
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Serial commands ready; type 'help'");
    reply("READY", session_controller_state_name());
    return ESP_OK;
}

void hub_console_set_status(const char *phase, const char *ssid,
                            uint32_t retry_delay_ms)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_phase, sizeof(s_phase), "%s", phase);
    snprintf(s_camera_ssid, sizeof(s_camera_ssid), "%s", ssid);
    s_retry_deadline_us = esp_timer_get_time() + (int64_t)retry_delay_ms * 1000;
    if (strcmp(phase, "searching") == 0) {
        s_media_done = 0;
        s_media_stored = 0;
        s_media_total = 0;
    }
    xSemaphoreGive(s_lock);
}

void hub_console_set_media_progress(uint32_t done, uint32_t stored,
                                    uint32_t total)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_media_done = done;
    s_media_stored = stored;
    s_media_total = total;
    xSemaphoreGive(s_lock);
}
