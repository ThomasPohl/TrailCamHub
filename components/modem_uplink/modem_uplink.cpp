/*
 * Brings the A7670E cellular modem up for one backend sync and down again
 * (see modem_uplink.hpp and docs/modem.md). The esp-iot-solution driver
 * (iot_usbh_modem) handles USB, the AT channel and PPP; this module powers
 * the modem, turns its radio on and off, enters the SIM PIN, waits for the
 * network and starts and ends the data call.
 */

#include "modem_uplink.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_MODEM_UPLINK
#include "at_3gpp_ts_27_007.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hub_config.hpp"
#include "iot_usbh_cdc.h"
#include "iot_usbh_modem.h"
#include "modem_ids.h"
#endif

namespace modem_uplink
{

#if CONFIG_MODEM_UPLINK

namespace
{

const char *TAG = "trailcamhub_modem";

constexpr EventBits_t kGotIp = BIT0;
constexpr EventBits_t kLostIp = BIT1;
constexpr std::int64_t kSecondUs = 1000 * 1000;

enum class Command : std::uint8_t { connect, disconnect, radio_off };

Callbacks s_callbacks = {};
QueueHandle_t s_commands = nullptr;
EventGroupHandle_t s_events = nullptr;
// The settings of the next connect(), handed to the task.
Settings s_next_settings;
SemaphoreHandle_t s_settings_lock = nullptr;
std::atomic<bool> s_cancel{false};
std::atomic<bool> s_active{false};
// The start-up switch-off is queued or running (deep sleep waits for it).
std::atomic<bool> s_switching_off{false};
// Signal of the last attach in dBm; 0: unknown.
std::atomic<int> s_signal_dbm{0};
// Owned by the task.
bool s_cdc_installed = false;
bool s_modem_installed = false;
bool s_ppp_started = false;

void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == IP_EVENT_PPP_GOT_IP) {
        xEventGroupSetBits(s_events, kGotIp);
    } else if (id == IP_EVENT_PPP_LOST_IP) {
        xEventGroupSetBits(s_events, kLostIp);
    }
}

void set_power(bool on)
{
#if CONFIG_MODEM_POWER_GPIO >= 0
    gpio_set_level((gpio_num_t)CONFIG_MODEM_POWER_GPIO, on ? 1 : 0);
    ESP_LOGI(TAG, "Modem power %s (GPIO %d)", on ? "on" : "off",
             CONFIG_MODEM_POWER_GPIO);
#else
    (void)on;
#endif
}

// Waits up to `ms`, in steps, unless the connect is cancelled.
bool pause_ms(std::uint32_t ms)
{
    while (ms > 0 && !s_cancel.load()) {
        const std::uint32_t step = ms < 250 ? ms : 250;
        vTaskDelay(pdMS_TO_TICKS(step));
        ms -= step;
    }
    return !s_cancel.load();
}

struct Deadline {
    std::int64_t end_us;
    explicit Deadline(std::uint32_t seconds)
        : end_us(esp_timer_get_time() + (std::int64_t)seconds * kSecondUs)
    {
    }
    bool passed() const
    {
        return esp_timer_get_time() >= end_us;
    }
    TickType_t ticks_left() const
    {
        const std::int64_t left_us = end_us - esp_timer_get_time();
        return left_us > 0 ? pdMS_TO_TICKS(left_us / 1000) : 0;
    }
};

// Log tag of the driver's AT parser. Until the modem is on USB it is
// stopped, and each command logs "AT parser is stopped" as an error.
const char *kAtParserTag = "Modem AT";

// Waits until the modem answers AT commands over USB.
const char *wait_for_modem(const Deadline &deadline, at_handle_t &at)
{
    ESP_LOGI(TAG, "Waiting for the modem on USB");
    const esp_log_level_t level = esp_log_level_get(kAtParserTag);
    esp_log_level_set(kAtParserTag, ESP_LOG_NONE);
    const char *problem = nullptr;
    while (true) {
        if (s_cancel.load()) {
            problem = "stopped";
            break;
        }
        at = usbh_modem_get_atparser();
        if (at != nullptr && at_cmd_at(at) == ESP_OK) {
            break;
        }
        if (deadline.passed()) {
            problem = "the modem does not answer over USB (DIP switches: USB "
                      "OFF, 4G ON)";
            break;
        }
        pause_ms(1000);
    }
    esp_log_level_set(kAtParserTag, level);
    return problem;
}

// Collects the whole answer of a command up to its final result code.
bool collect_answer(at_handle_t at, const char *buffer)
{
    const std::size_t length = std::strlen(buffer);
    const bool ok =
        length >= 4 && std::strcmp(buffer + length - 4, "OK\r\n") == 0;
    if (!ok && std::strstr(buffer, "ERROR") == nullptr) {
        return false;
    }
    auto *answer = static_cast<std::string *>(modem_at_get_handle_line_ctx(at));
    answer->assign(buffer);
    return true;
}

// Sends `command`; ESP_OK with the answer if it ended in OK, ESP_FAIL if it
// ended in an error, ESP_ERR_TIMEOUT without an answer.
esp_err_t at_query(at_handle_t at, const char *command, std::string &answer)
{
    answer.clear();
    const esp_err_t error =
        modem_at_send_command(at, command, CONFIG_MODEM_COMMAND_TIMEOUT_DEFAULT,
                              collect_answer, &answer);
    if (error != ESP_OK) {
        return error;
    }
    if (answer.empty()) {
        return ESP_ERR_TIMEOUT;
    }
    return answer.find("ERROR") == std::string::npos ? ESP_OK : ESP_FAIL;
}

// PIN attempts the card has left (SIMCom AT+SPIC); -1 if unknown.
int pin_attempts_left(at_handle_t at)
{
    std::string answer;
    if (at_query(at, "AT+SPIC", answer) != ESP_OK) {
        return -1;
    }
    return hub_config::parse_spic_pin_attempts(answer);
}

enum class Sim : std::uint8_t { ready, pin, puk, missing, unknown };

const char *sim_name(Sim sim)
{
    switch (sim) {
    case Sim::ready:
        return "ready";
    case Sim::pin:
        return "PIN required";
    case Sim::puk:
        return "PUK required";
    case Sim::missing:
        return "not inserted";
    case Sim::unknown:
        break;
    }
    return "unknown";
}

// AT+CPIN?, with "SIM not inserted" (+CME ERROR: 10) told apart from an
// answer that is not there yet.
Sim read_sim(at_handle_t at)
{
    std::string answer;
    const esp_err_t error = at_query(at, "AT+CPIN?", answer);
    if (answer.find("+CME ERROR: 10") != std::string::npos ||
        answer.find("SIM not inserted") != std::string::npos ||
        answer.find("NOT INSERTED") != std::string::npos) {
        return Sim::missing;
    }
    if (error != ESP_OK) {
        return Sim::unknown;
    }
    if (answer.find("READY") != std::string::npos) {
        return Sim::ready;
    }
    if (answer.find("SIM PUK") != std::string::npos) {
        return Sim::puk;
    }
    if (answer.find("SIM PIN") != std::string::npos) {
        return Sim::pin;
    }
    return Sim::unknown;
}

const char *enter_pin(at_handle_t at, const std::string &sim_pin)
{
    if (sim_pin.empty()) {
        return "the SIM card needs a PIN (config set sim_pin ...)";
    }
    if (!hub_config::validate(hub_config::Key::sim_pin, sim_pin).empty()) {
        // Stored by an older firmware, which allowed 4-8 digits.
        return "sim_pin must be 4 digits; set it again";
    }
    const int attempts = pin_attempts_left(at);
    if (attempts >= 0) {
        ESP_LOGI(TAG, "SIM PIN attempts left: %d", attempts);
    }
    switch (hub_config::sim_pin_decision(hub_config::refused_sim_pin(), sim_pin,
                                         attempts)) {
    case hub_config::PinDecision::send:
        break;
    case hub_config::PinDecision::refused_before:
        return "the SIM card refused this PIN before; set sim_pin again";
    case hub_config::PinDecision::too_few_attempts:
        return "only one PIN attempt left; unlock the SIM card in a phone";
    }
    const esp_err_t error = at_cmd_set_pin(at, sim_pin.c_str());
    if (error == ESP_FAIL) {
        // The card answered with an error: remembered, so that the next
        // cycles do not use up the attempts and lock the card (PUK).
        hub_config::set_refused_sim_pin(
            hub_config::sim_pin_fingerprint(sim_pin));
        return "the SIM card refused the PIN; set sim_pin again";
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "SIM PIN: %s", esp_err_to_name(error));
        return "the SIM PIN could not be entered";
    }
    ESP_LOGI(TAG, "SIM PIN entered");
    return nullptr;
}

// Without a card the modem says so at once; an unknown state that lasts
// this long ends the attempt as well instead of waiting for the deadline.
constexpr std::int64_t kSimMissingAfterUs = 10 * kSecondUs;

const char *unlock_sim(const Deadline &deadline, at_handle_t at,
                       const std::string &sim_pin)
{
    const std::int64_t started_us = esp_timer_get_time();
    bool pin_entered = false;
    Sim last = Sim::unknown;
    // The card takes a moment after the radio came on and after the PIN.
    while (true) {
        const Sim sim = read_sim(at);
        if (sim != last) {
            ESP_LOGI(TAG, "SIM %s", sim_name(sim));
            last = sim;
        }
        if (sim == Sim::ready) {
            return nullptr;
        }
        if (sim == Sim::puk) {
            return "the SIM card is locked (PUK required)";
        }
        if (sim == Sim::pin && !pin_entered) {
            const char *problem = enter_pin(at, sim_pin);
            if (problem != nullptr) {
                return problem;
            }
            pin_entered = true;
            continue;
        }
        const bool slow =
            esp_timer_get_time() - started_us > kSimMissingAfterUs;
        if (sim == Sim::missing && slow) {
            return "no SIM card";
        }
        if (sim == Sim::unknown && slow && !pin_entered) {
            return "no SIM card (or it does not answer)";
        }
        if (s_cancel.load()) {
            return "stopped";
        }
        if (deadline.passed()) {
            return "the SIM card is not ready";
        }
        pause_ms(1000);
    }
}

const char *wait_for_network(const Deadline &deadline, at_handle_t at)
{
    int last_stat = -1;
    while (true) {
        esp_modem_at_cereg_t reg = {};
        if (at_cmd_get_network_reg_status(at, &reg) == ESP_OK) {
            if (reg.stat == 1 || reg.stat == 5) {
                ESP_LOGI(TAG, "Registered (%s)",
                         reg.stat == 1 ? "home network" : "roaming");
                return nullptr;
            }
            if (reg.stat != last_stat) {
                last_stat = reg.stat;
                ESP_LOGI(TAG, "Network registration: %d", reg.stat);
            }
            if (reg.stat == 3) {
                return "the network denied the registration";
            }
        }
        if (s_cancel.load()) {
            return "stopped";
        }
        if (deadline.passed()) {
            return "no network (registration timed out)";
        }
        pause_ms(2000);
    }
}

void log_network(at_handle_t at)
{
    esp_modem_at_csq_t csq = {};
    char op[64] = {};
    at_cmd_get_signal_quality(at, &csq);
    at_cmd_get_operator_name(at, op, sizeof(op));
    // rssi 0..31 is -113..-51 dBm; 99 unknown.
    s_signal_dbm = csq.rssi >= 0 && csq.rssi <= 31 ? -113 + 2 * csq.rssi : 0;
    if (csq.rssi >= 0 && csq.rssi <= 31) {
        ESP_LOGI(TAG, "Network %s, signal %d dBm (CSQ %d)", op,
                 -113 + 2 * csq.rssi, csq.rssi);
    } else {
        ESP_LOGI(TAG, "Network %s, signal unknown", op);
    }
}

// USB host and modem driver, without starting a data call. Returns null on
// success, else the reason.
const char *install_drivers(const std::string &apn)
{
    if (!s_cdc_installed) {
        usbh_cdc_driver_config_t cdc = {};
        cdc.task_stack_size = 4096;
        cdc.task_priority = 5;
        cdc.task_coreid = 0;
        cdc.skip_init_usb_host_driver = false;
        const esp_err_t error = usbh_cdc_driver_install(&cdc);
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "USB host: %s", esp_err_to_name(error));
            return "the USB host could not be started";
        }
        s_cdc_installed = true;
    }

    usbh_modem_config_t config = {};
    config.modem_id_list = trailcamhub_modem_ids;
    config.at_tx_buffer_size = 256;
    // Room for unsolicited lines arriving together with an answer (with 256
    // bytes the driver truncated them).
    config.at_rx_buffer_size = 1024;
    if (!apn.empty()) {
        config.pdp.enable = true;
        config.pdp.cid = 1;
        config.pdp.type = "IP";
        config.pdp.apn = apn.c_str();
    }
    xEventGroupClearBits(s_events, kGotIp | kLostIp);
    const esp_err_t error = usbh_modem_install(&config);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Modem driver: %s", esp_err_to_name(error));
        return "the modem driver could not be started";
    }
    s_modem_installed = true;
    // A data call is started by bring_up() once the SIM and the network are
    // ready, not as soon as the modem appears on USB.
    usbh_modem_ppp_auto_connect(false);
    return nullptr;
}

// Returns null on success, else the reason.
const char *bring_up(const Settings &settings)
{
    const Deadline deadline(CONFIG_MODEM_ATTACH_TIMEOUT_S);
    const std::int64_t started_us = esp_timer_get_time();
    set_power(true);
    const char *installed = install_drivers(settings.apn);
    if (installed != nullptr) {
        return installed;
    }
    at_handle_t at = nullptr;
    const char *problem = wait_for_modem(deadline, at);
    if (problem != nullptr) {
        return problem;
    }
    ESP_LOGI(TAG, "Modem answers after %lld ms",
             (long long)((esp_timer_get_time() - started_us) / 1000));
    // Back from the flight mode of the last sync. Not an error if the radio
    // was on already.
    at_send_command_response_ok(at, "AT+CFUN=1");
    problem = unlock_sim(deadline, at, settings.sim_pin);
    if (problem == nullptr) {
        problem = wait_for_network(deadline, at);
    }
    if (problem != nullptr) {
        return problem;
    }
    log_network(at);

    s_ppp_started = true;
    const esp_err_t error = usbh_modem_ppp_start(deadline.ticks_left());
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "PPP: %s", esp_err_to_name(error));
        return "the data call could not be started";
    }
    // Waits in steps so that a disconnect() can cancel it.
    while (true) {
        const EventBits_t bits = xEventGroupWaitBits(
            s_events, kGotIp, pdFALSE, pdFALSE, pdMS_TO_TICKS(500));
        if ((bits & kGotIp) != 0) {
            break;
        }
        if (s_cancel.load()) {
            return "stopped";
        }
        if (deadline.passed()) {
            return "the data call got no IP address";
        }
    }
    esp_netif_t *netif = usbh_modem_get_netif();
    if (netif != nullptr) {
        esp_netif_set_default_netif(netif);
    }
    ESP_LOGI(TAG, "Online over the modem after %lld ms",
             (long long)((esp_timer_get_time() - started_us) / 1000));
    return nullptr;
}

void take_down()
{
    if (s_modem_installed) {
        // Ends the data call, if one was started: without one the driver
        // logs an error.
        if (s_ppp_started) {
            usbh_modem_ppp_stop();
            s_ppp_started = false;
        }
#if CONFIG_MODEM_POWER_GPIO < 0
        // The modem keeps its power (DIP switch): flight mode until the
        // next sync, which saves most of its idle current.
        at_handle_t at = usbh_modem_get_atparser();
        if (at != nullptr &&
            at_send_command_response_ok(at, "AT+CFUN=4") == ESP_OK) {
            ESP_LOGI(TAG, "Modem radio off");
        }
#endif
        usbh_modem_uninstall();
        s_modem_installed = false;
    }
    if (s_cdc_installed) {
        const esp_err_t error = usbh_cdc_driver_uninstall();
        if (error == ESP_OK) {
            s_cdc_installed = false;
        } else {
            // Kept for the next sync.
            ESP_LOGW(TAG, "USB host stays installed: %s",
                     esp_err_to_name(error));
        }
    }
    set_power(false);
}

// How long the start-up switch-off waits for the modem on USB. Without
// power (4G DIP switch OFF) it never appears, and nothing needs switching
// off.
constexpr std::uint32_t kRadioOffTimeoutS = 20;

// After a power-on or reset the modem (powered by the DIP switch) starts with
// its radio on and registers with the network on its own, whatever the
// transport: put it into flight mode until a sync needs it (#155).
void radio_off_at_start()
{
#if CONFIG_MODEM_POWER_GPIO >= 0
    // Off by its power GPIO until a sync switches it on.
#else
    const Deadline deadline(kRadioOffTimeoutS);
    const char *problem = install_drivers("");
    at_handle_t at = nullptr;
    if (problem == nullptr) {
        problem = wait_for_modem(deadline, at);
    }
    if (problem != nullptr) {
        ESP_LOGI(TAG, "Modem radio left as it is at start-up: %s", problem);
    }
    // Flight mode (AT+CFUN=4) if the modem answers, drivers removed.
    take_down();
#endif
}

void modem_task(void *arg)
{
    (void)arg;
    Command command;
    while (true) {
        xQueueReceive(s_commands, &command, portMAX_DELAY);
        if (command == Command::connect) {
            Settings settings;
            xSemaphoreTake(s_settings_lock, portMAX_DELAY);
            settings = s_next_settings;
            xSemaphoreGive(s_settings_lock);
            const char *problem = bring_up(settings);
            if (problem == nullptr) {
                if (s_callbacks.connected != nullptr) {
                    s_callbacks.connected();
                }
            } else {
                ESP_LOGW(TAG, "Modem uplink failed: %s", problem);
                if (s_callbacks.failed != nullptr) {
                    s_callbacks.failed(problem);
                }
            }
        } else if (command == Command::radio_off) {
            radio_off_at_start();
            s_switching_off = false;
        } else {
            take_down();
            s_active = false;
        }
    }
}

}  // namespace

bool available()
{
    return true;
}

esp_err_t init(const Callbacks &callbacks)
{
    if (s_commands != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    s_callbacks = callbacks;
    // The PPP glue asks the modem driver for a MAC address, which it does not
    // have; the warning about it on every attach means nothing.
    esp_log_level_set("iot_eth", ESP_LOG_ERROR);
#if CONFIG_MODEM_POWER_GPIO >= 0
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << CONFIG_MODEM_POWER_GPIO;
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    // Off until a sync needs it.
    gpio_set_level((gpio_num_t)CONFIG_MODEM_POWER_GPIO, 0);
#endif
    s_commands = xQueueCreate(4, sizeof(Command));
    s_events = xEventGroupCreate();
    s_settings_lock = xSemaphoreCreateMutex();
    if (s_commands == nullptr || s_events == nullptr ||
        s_settings_lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error = esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP,
                                                 on_ip_event, nullptr);
    if (error == ESP_OK) {
        error = esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP,
                                           on_ip_event, nullptr);
    }
    if (error != ESP_OK) {
        return error;
    }
    if (xTaskCreate(modem_task, "modem", 4096, nullptr, 5, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    // A wake-up from deep sleep finds the modem as the last cycle left it.
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        s_switching_off = true;
        const Command command = Command::radio_off;
        xQueueSend(s_commands, &command, 0);
    }
    return ESP_OK;
}

esp_err_t connect(const Settings &settings)
{
    if (s_commands == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_settings_lock, portMAX_DELAY);
    s_next_settings = settings;
    xSemaphoreGive(s_settings_lock);
    s_cancel = false;
    s_signal_dbm = 0;
    s_active = true;
    const Command command = Command::connect;
    return xQueueSend(s_commands, &command, 0) == pdTRUE ? ESP_OK
                                                         : ESP_ERR_NO_MEM;
}

void disconnect()
{
    if (s_commands == nullptr || !s_active.load()) {
        return;
    }
    s_cancel = true;
    const Command command = Command::disconnect;
    xQueueSend(s_commands, &command, portMAX_DELAY);
}

bool active()
{
    return s_active.load() || s_switching_off.load();
}

int signal_dbm()
{
    return s_signal_dbm.load();
}

#else  // !CONFIG_MODEM_UPLINK

bool available()
{
    return false;
}

esp_err_t init(const Callbacks &callbacks)
{
    (void)callbacks;
    return ESP_OK;
}

esp_err_t connect(const Settings &settings)
{
    (void)settings;
    return ESP_ERR_NOT_SUPPORTED;
}

void disconnect()
{
}

bool active()
{
    return false;
}

int signal_dbm()
{
    return 0;
}

#endif  // CONFIG_MODEM_UPLINK

}  // namespace modem_uplink
