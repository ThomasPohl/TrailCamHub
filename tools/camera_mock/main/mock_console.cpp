#include "mock_console.hpp"

#include <cstdio>
#include <cstring>

#include "esp_console.h"
#include "esp_log.h"

#include "mock_ap.hpp"
#include "mock_ble.h"
#include "mock_udp.hpp"

namespace mock_console
{

namespace
{

const char *TAG = "mock_console";

int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    bool subscribed = false;
    const bool connected = mock_ble_connected(&subscribed);
    std::printf("BLE: %s\n",
                connected ? (subscribed ? "connected, notifying" : "connected")
                          : "advertising");
    std::printf("AP: %s \"%s\" (BSSID %s, password %s), %d station(s)\n",
                mock_ap::running() ? "up" : "off", mock_ap::ssid().c_str(),
                mock_ap::bssid().c_str(), mock_ap::password().c_str(),
                mock_ap::station_count());
    std::printf("%s", mock_udp::status().c_str());
    return 0;
}

int cmd_close(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    mock_udp::close_session();
    return 0;
}

int cmd_ap(int argc, char **argv)
{
    if (argc != 2 ||
        (std::strcmp(argv[1], "on") != 0 && std::strcmp(argv[1], "off") != 0)) {
        std::printf("usage: ap on|off\n");
        return 1;
    }
    if (std::strcmp(argv[1], "on") == 0) {
        return mock_ap::start() ? 0 : 1;
    }
    mock_udp::close_session();
    mock_ap::stop();
    return 0;
}

esp_err_t add(const char *name, const char *help, const char *hint,
              esp_console_cmd_func_t func)
{
    esp_console_cmd_t cmd = {};
    cmd.command = name;
    cmd.help = help;
    cmd.hint = hint;
    cmd.func = func;
    return esp_console_cmd_register(&cmd);
}

}  // namespace

esp_err_t start()
{
    esp_console_repl_t *repl = nullptr;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "mock>";
    esp_console_dev_uart_config_t uart_config =
        ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err =
        esp_console_new_repl_uart(&uart_config, &repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Console: %s", esp_err_to_name(err));
        return err;
    }
    esp_console_register_help_command();
    add("status", "BLE, access point and UDP session counters", nullptr,
        cmd_status);
    add("close", "end the session from the camera side (CLOSE)", nullptr,
        cmd_close);
    add("ap", "turn the access point on or off", "on|off", cmd_ap);
    return esp_console_start_repl(repl);
}

}  // namespace mock_console
