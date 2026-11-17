#include "mock_identity.hpp"

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

namespace mock_identity
{
namespace
{
const char *TAG = "mock_identity";
const char *kNamespace = "mock";
const char *kKey = "dev_id";
}  // namespace

camera_mock::Identity load()
{
    camera_mock::Identity identity;
    if (!camera_mock::parse_mac(CONFIG_CAMERA_MOCK_DEV_ID, identity)) {
        ESP_LOGW(TAG,
                 "CONFIG_CAMERA_MOCK_DEV_ID \"%s\" is not a MAC; using "
                 "the captured camera's",
                 CONFIG_CAMERA_MOCK_DEV_ID);
    }
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return identity;
    }
    char text[24] = {};
    size_t length = sizeof(text);
    if (nvs_get_str(handle, kKey, text, &length) == ESP_OK &&
        !camera_mock::parse_mac(text, identity)) {
        ESP_LOGW(TAG, "Stored MAC \"%s\" is not valid; ignored", text);
    }
    nvs_close(handle);
    return identity;
}

bool save(const std::string &mac)
{
    camera_mock::Identity identity;
    if (!camera_mock::parse_mac(mac, identity)) {
        return false;
    }
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    const bool ok =
        nvs_set_str(handle, kKey, camera_mock::mac_text(identity).c_str()) ==
            ESP_OK &&
        nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

void clear()
{
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_erase_key(handle, kKey);
    nvs_commit(handle);
    nvs_close(handle);
}

}  // namespace mock_identity
