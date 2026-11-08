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
#include <time.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
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
#include "hub_battery.hpp"
#include "ota_update.hpp"
#include "hub_config.hpp"
#include "schedule_state.hpp"
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
    if (message.empty()) {
        printf("TCH %s\n", kind);
    } else {
        printf("TCH %s %s\n", kind, message.c_str());
    }
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
uint32_t s_cameras_done;
uint32_t s_cameras_read;
uint32_t s_max_cameras;
int64_t s_retry_deadline_us;
uint32_t s_media_done;
uint32_t s_media_stored;
uint32_t s_media_total;

#if CONFIG_HUB_CONSOLE_DIAGNOSTICS
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
#endif

void event(const std::string &message)
{
    reply("EVENT", message);
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

// Last phase printed; only touched by the session controller task.
session_phase_t s_printed_phase = SESSION_PHASE_IDLE;
bool s_printed_stopping = false;

// Status listener: one line per phase change, stop and finished cycle, and
// the state for the status command.
void on_status(const session_status_t *status, void *context)
{
    (void)context;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *phase = status->front_end_phase_name;
    if (strcmp(phase, "searching") == 0 && strcmp(s_phase, "searching") != 0) {
        s_media_done = 0;
        s_media_stored = 0;
        s_media_total = 0;
    }
    snprintf(s_phase, sizeof(s_phase), "%s", phase);
    if (status->camera_ssid[0] != '\0') {
        snprintf(s_camera_ssid, sizeof(s_camera_ssid), "%s",
                 status->camera_ssid);
    }
    s_retry_deadline_us =
        esp_timer_get_time() + (int64_t)status->retry_delay_ms * 1000;
    s_cameras_done = status->cameras_done;
    s_cameras_read = status->cameras_read;
    s_max_cameras = status->max_cameras;
    xSemaphoreGive(s_lock);

    if (status->phase != s_printed_phase) {
        s_printed_phase = status->phase;
        event(std::string("state ") + status->phase_name);
    }
    if (status->stopping && !s_printed_stopping) {
        event("stopping");
    }
    s_printed_stopping = status->stopping;
    // "camera 2 read TC100_A1F0 F4_C1_56_06_A1_F0": a camera of the scan is
    // done (read, partial, no-card or failed); the id is "-" if unknown.
    if (status->camera_finished && status->cameras_done >= 1 &&
        status->cameras_done <= SESSION_MAX_CAMERA_RESULTS) {
        const session_camera_t &camera =
            status->cameras[status->cameras_done - 1];
        event(format_line("camera %u %s %s %s", (unsigned)status->cameras_done,
                          camera.result_name,
                          camera.name[0] != '\0' ? camera.name : "unknown",
                          camera.cam_id[0] != '\0' ? camera.cam_id : "-"));
    }
    if (status->searching_next_camera &&
        status->phase == SESSION_PHASE_CAMERA_WAKE && status->camera_finished) {
        event("searching next camera");
    }
    if (status->cycle_finished) {
        // "done uplink-done no-card": synced, but the camera has no SD card.
        const bool no_card_synced =
            status->camera_no_card &&
            status->last_result != SESSION_RESULT_CAMERA_NO_CARD;
        event(std::string("done ") + status->last_result_name +
              (no_card_synced ? " no-card" : ""));
    }
}

// The hub's own battery: "battery 3987 mV, 82% (MAX17048)"; nothing on
// boards without a measurement.
void print_battery()
{
    if (!hub_battery::supported()) {
        return;
    }
    hub_battery::Reading reading;
    if (hub_battery::read(reading)) {
        reply("INFO",
              format_line("battery %d mV, %d%% (%s)%s", reading.millivolts,
                          reading.percent, hub_battery::source_name(),
                          reading.charging ? ", charging" : ""));
    } else {
        reply("INFO", std::string("battery unknown (") +
                          hub_battery::source_name() + " does not answer)");
    }
}

// The cycle's phase, the download progress, the battery, the memory and the SD
// card.
void print_details()
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    reply("INFO", std::string("phase ") + s_phase);
    reply("INFO", std::string("camera ") + s_camera_ssid);
    reply("INFO", format_line("cameras %lu read, %lu done (at most %lu)",
                              (unsigned long)s_cameras_read,
                              (unsigned long)s_cameras_done,
                              (unsigned long)s_max_cameras));
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
    print_battery();
    reply("INFO", "firmware " + ota::running_version() + " (" + ota::board() +
                      "-" + ota::variant() + "), " + ota::last_check_text());
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
    // Starts with "backend", not a setting name, so that it cannot be read as
    // one of the setting lines above.
    const char *transport =
        hub_config::transport_name(hub_config::uplink_transport(settings));
    reply("CONFIG",
          hub_config::uplink_ready(settings)
              ? std::string("backend configured, over ") + transport
              : std::string("backend incomplete (") + transport + ")");
    if (hub_config::modem_available() && !settings.sim_pin.empty() &&
        hub_config::refused_sim_pin() ==
            hub_config::sim_pin_fingerprint(settings.sim_pin)) {
        reply("INFO", "the SIM card refused sim_pin; it is not sent again "
                      "until sim_pin is set again");
    }
}

// "in 1234 s (2026-11-02 15:20)", "in 1234 s" without a clock, or "never".
std::string when(std::int64_t t, std::int64_t now)
{
    if (t == schedule::kNever) {
        return "never";
    }
    std::string text =
        "in " + std::to_string((long long)(t > now ? t - now : 0)) + " s";
    const std::int64_t unix_time = schedule::to_unix(t);
    if (unix_time > 0) {
        const time_t wall = static_cast<time_t>(unix_time);
        struct tm local = {};
        localtime_r(&wall, &local);
        char date[24];
        strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &local);
        text += std::string(" (") + date + ")";
    }
    return text;
}

// The profile, the schedule that applies and the next cycles.
void print_schedule()
{
    const schedule::Effective effective = schedule::effective();
    const schedule::Times times = schedule::next_times(effective);
    const schedule::State state = schedule::state();
    const std::int64_t now = schedule::now();
#if CONFIG_HUB_PROFILE_AUTONOMOUS
    const char *profile = "autonomous";
#else
    const char *profile = "on-site";
#endif
#if CONFIG_POWER_DEEP_SLEEP
    const char *sleep = "deep sleep between cycles";
#else
    const char *sleep = "stays awake";
#endif
    reply("INFO", std::string("profile ") + profile + ", " + sleep);
    reply("INFO", "camera plan " + effective.camera_plan + " (" +
                      schedule::origin_name(effective.camera_plan_origin) +
                      ")");
    reply("INFO",
          "check-in every " + std::to_string(effective.checkin_min) + " min (" +
              schedule::origin_name(effective.checkin_origin) +
              (effective.settings.uplink ? ")" : ", no uplink settings)"));
    reply("INFO", "next camera read " + when(times.camera, now));
    reply("INFO", "next check-in " + when(times.checkin, now));
    if (state.camera_failures > 0 || state.checkin_failures > 0) {
        reply("INFO", format_line("failed in a row: %u camera reads, %u "
                                  "check-ins",
                                  (unsigned)state.camera_failures,
                                  (unsigned)state.checkin_failures));
    }
    if (state.camera_battery >= 0) {
        reply("INFO",
              format_line("camera battery %d%%", (int)state.camera_battery));
    }
    if (hub_battery::supported()) {
        reply("INFO",
              std::string("hub battery level ") +
                  schedule::power_level_name(schedule::hub_level(state)) +
                  (state.hub_charging ? " (charging)" : ""));
    }
    const char *stretch =
        schedule::stretch_name(schedule::stretch(effective.settings, state));
    if (stretch[0] != '\0') {
        reply("INFO", std::string("schedule stretched: ") + stretch);
    }
}

void print_status()
{
    reply("STATE", session_controller_state_name());
    print_settings();
    print_details();
    print_schedule();
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
    case CommandType::camlist:
        request_cycle(session_controller_request_list(), "camlist");
        break;
    case CommandType::upload:
        request_cycle(session_controller_request_upload(), "upload");
        break;
    case CommandType::update:
        // The release manifest is read at the end of the sync, also if it
        // was read less than a day ago.
        ota::force_check();
        request_cycle(session_controller_request_upload(), "update");
        break;
    case CommandType::stop: {
        const esp_err_t error = session_controller_request_stop();
        if (error == ESP_OK) {
            reply("OK", "stop requested");
        } else if (error == ESP_ERR_INVALID_STATE) {
            reply("ERR", std::string("nothing to stop (") +
                             session_controller_state_name() + ")");
        } else {
            reply("ERR", std::string("stop failed: ") + esp_err_to_name(error));
        }
        break;
    }
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
        // Someone is using the hub: no deep sleep for a while.
        schedule::note_activity();
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
    session_status_t status;
    session_controller_get_status(&status);
    s_printed_phase = status.phase;
    const esp_err_t listener_error =
        session_controller_add_status_listener(on_status, NULL);
    if (listener_error != ESP_OK) {
        ESP_LOGW(TAG, "Could not follow the session status: %s",
                 esp_err_to_name(listener_error));
    }
    // The stack must stay in internal RAM: commands such as "config set"
    // write NVS, and flash writes disable the cache and with it PSRAM.
    // ESP-IDF asserts in spi_flash_disable_interrupts_caches_and_other_cpu()
    // if the calling task's stack lives in PSRAM.
    const BaseType_t created =
        xTaskCreate(console_task, "hub_console", CONSOLE_TASK_STACK, NULL,
                    CONSOLE_TASK_PRIORITY, NULL);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Serial commands ready; type 'help'");
    reply("READY", session_controller_state_name());
    return ESP_OK;
}

void hub_console_list_event(const char *line)
{
    event(line);
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
