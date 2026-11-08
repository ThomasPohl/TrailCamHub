/*
 * Stores the hub settings in their own NVS namespace. NVS is initialized by
 * app_main before anything here is used.
 */

#include "hub_config.hpp"

#include "sdkconfig.h"

#include <vector>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"

namespace hub_config
{

namespace
{

const char *TAG = "trailcamhub_config";
const char *kNamespace = "hub_config";
// Not a setting: written by the modem uplink, cleared with sim_pin.
const char *kRefusedPinKey = "pin_refused";

class Handle
{
  public:
    explicit Handle(nvs_open_mode_t mode)
    {
        error_ = nvs_open(kNamespace, mode, &handle_);
    }
    ~Handle()
    {
        if (error_ == ESP_OK) {
            nvs_close(handle_);
        }
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;

    esp_err_t error() const
    {
        return error_;
    }
    nvs_handle_t get() const
    {
        return handle_;
    }

  private:
    nvs_handle_t handle_{};
    esp_err_t error_;
};

esp_err_t read_string(nvs_handle_t handle, const char *key, std::string &out)
{
    out.clear();
    size_t length = 0;
    esp_err_t error = nvs_get_str(handle, key, nullptr, &length);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(error, TAG, "Could not read %s", key);
    std::vector<char> buffer(length);
    ESP_RETURN_ON_ERROR(nvs_get_str(handle, key, buffer.data(), &length), TAG,
                        "Could not read %s", key);
    out.assign(buffer.data());
    return ESP_OK;
}

}  // namespace

esp_err_t load(Settings &out)
{
    out = Settings{};
    Handle handle(NVS_READONLY);
    if (handle.error() == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;  // nothing stored yet
    }
    ESP_RETURN_ON_ERROR(handle.error(), TAG, "Could not open settings");
    for (Key key : kAllKeys) {
        ESP_RETURN_ON_ERROR(
            read_string(handle.get(), key_name(key), field(out, key)), TAG,
            "Could not load settings");
    }
    return ESP_OK;
}

esp_err_t set(Key key, const std::string &value, std::string *reason)
{
    if (!value.empty()) {
        const std::string problem = validate(key, value);
        if (!problem.empty()) {
            if (reason != nullptr) {
                *reason = problem;
            }
            return ESP_ERR_INVALID_ARG;
        }
    }
    Handle handle(NVS_READWRITE);
    ESP_RETURN_ON_ERROR(handle.error(), TAG, "Could not open settings");
    esp_err_t error =
        value.empty() ? nvs_erase_key(handle.get(), key_name(key))
                      : nvs_set_str(handle.get(), key_name(key), value.c_str());
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        error = ESP_OK;  // erasing a setting that was never stored
    }
    ESP_RETURN_ON_ERROR(error, TAG, "Could not store %s", key_name(key));
    if (key == Key::sim_pin) {
        // A new (or the same, entered again on purpose) PIN may be tried.
        error = nvs_erase_key(handle.get(), kRefusedPinKey);
        if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Could not forget the refused SIM PIN: %s",
                     esp_err_to_name(error));
        }
    }
    ESP_RETURN_ON_ERROR(nvs_commit(handle.get()), TAG, "Could not commit");
    ESP_LOGI(TAG, "Setting %s %s", key_name(key),
             value.empty() ? "erased" : "stored");
    return ESP_OK;
}

bool modem_available()
{
#if CONFIG_MODEM_UPLINK
    return true;
#else
    return false;
#endif
}

Transport uplink_transport(const Settings &settings)
{
    return transport(settings, modem_available());
}

bool uplink_ready(const Settings &settings)
{
    return uplink_configured(settings, modem_available());
}

std::uint32_t refused_sim_pin()
{
    Handle handle(NVS_READONLY);
    std::uint32_t fingerprint = 0;
    if (handle.error() != ESP_OK ||
        nvs_get_u32(handle.get(), kRefusedPinKey, &fingerprint) != ESP_OK) {
        return 0;
    }
    return fingerprint;
}

esp_err_t set_refused_sim_pin(std::uint32_t fingerprint)
{
    Handle handle(NVS_READWRITE);
    ESP_RETURN_ON_ERROR(handle.error(), TAG, "Could not open settings");
    ESP_RETURN_ON_ERROR(nvs_set_u32(handle.get(), kRefusedPinKey, fingerprint),
                        TAG, "Could not store the refused SIM PIN");
    return nvs_commit(handle.get());
}

esp_err_t clear()
{
    Handle handle(NVS_READWRITE);
    ESP_RETURN_ON_ERROR(handle.error(), TAG, "Could not open settings");
    ESP_RETURN_ON_ERROR(nvs_erase_all(handle.get()), TAG,
                        "Could not erase settings");
    ESP_RETURN_ON_ERROR(nvs_commit(handle.get()), TAG, "Could not commit");
    ESP_LOGI(TAG, "All settings erased");
    return ESP_OK;
}

}  // namespace hub_config
