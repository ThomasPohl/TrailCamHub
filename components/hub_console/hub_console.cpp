/*
 * Reads commands from the console (UART or USB-Serial-JTAG) and executes
 * them: settings are stored through hub_config, scans, uploads and stops are
 * handed to the session controller. The controller's status changes are
 * printed as "TCH EVENT" lines for the web installer and other console
 * clients.
 */

#include "hub_console.h"

#include <stdio.h>
#include <string.h>
#include <string>

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "command_parser.hpp"
#include "hub_config.hpp"
#include "session_controller.h"

#define CONSOLE_TASK_STACK 6144
#define CONSOLE_TASK_PRIORITY 4
#define CONSOLE_LINE_MAX 512
#define CONSOLE_RX_BUFFER 1024

namespace
{

// Last phase printed; only touched by the session controller task.
session_phase_t s_printed_phase = SESSION_PHASE_IDLE;
bool s_printed_stopping = false;

const char *TAG = "trailcamhub_console";

void reply(const char *kind, const std::string &message)
{
    printf("TCH %s %s\n", kind, message.c_str());
    fflush(stdout);
}

void event(const std::string &message)
{
    reply("EVENT", message);
}

// Status listener: one line per phase change, stop and finished cycle.
void on_status(const session_status_t *status, void *context)
{
    (void)context;
    if (status->phase != s_printed_phase) {
        s_printed_phase = status->phase;
        event(std::string("state ") + status->phase_name);
    }
    if (status->stopping && !s_printed_stopping) {
        event("stopping");
    }
    s_printed_stopping = status->stopping;
    if (status->cycle_finished) {
        event(std::string("done ") + status->last_result_name);
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
        execute(hub_console::parse_command(line));
        // The line may hold a secret (password, device key).
        memset(line, 0, sizeof(line));
    }
}

}  // namespace

// Blocking reads on stdin need the driver behind the console VFS.
static esp_err_t install_console_driver(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t config =
        USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    config.rx_buffer_size = CONSOLE_RX_BUFFER;
    esp_err_t error = usb_serial_jtag_driver_install(&config);
    if (error == ESP_OK) {
        usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CRLF);
        usb_serial_jtag_vfs_use_driver();
    }
    return error;
#else
    const uart_port_t port = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;
    esp_err_t error =
        uart_driver_install(port, CONSOLE_RX_BUFFER, 0, 0, NULL, 0);
    if (error == ESP_OK) {
        uart_vfs_dev_port_set_rx_line_endings(port, ESP_LINE_ENDINGS_CRLF);
        uart_vfs_dev_use_driver(port);
    }
    return error;
#endif
}

esp_err_t hub_console_start(void)
{
    esp_err_t error = install_console_driver();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not install the console driver: %s",
                 esp_err_to_name(error));
        return error;
    }

    session_status_t status;
    session_controller_get_status(&status);
    s_printed_phase = status.phase;
    error = session_controller_add_status_listener(on_status, NULL);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not follow the session status: %s",
                 esp_err_to_name(error));
    }

    if (xTaskCreate(console_task, "hub_console", CONSOLE_TASK_STACK, NULL,
                    CONSOLE_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Serial commands ready; type 'help'");
    reply("READY", session_controller_state_name());
    return ESP_OK;
}
