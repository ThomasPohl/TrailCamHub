#include "data_usage_store.hpp"

#include <mutex>

#include "esp_log.h"
#include "nvs.h"

namespace uplink::usage
{

namespace
{

const char *TAG = "trailcamhub_usage";
const char *kNamespace = "trailcam_usage";
const char *kKey = "totals";

std::mutex s_mutex;
Counter s_counter;
Transport s_transport = Transport::wifi;
bool s_loaded = false;

void load_locked()
{
    if (s_loaded) {
        return;
    }
    s_loaded = true;
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    char text[128];
    size_t length = sizeof(text);
    if (nvs_get_str(handle, kKey, text, &length) == ESP_OK &&
        !s_counter.parse(text)) {
        ESP_LOGW(TAG, "Stored data usage is malformed; starting at 0");
    }
    nvs_close(handle);
    s_counter.mark_saved();
}

}  // namespace

void init()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    load_locked();
}

void set_transport(Transport transport)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_transport = transport;
}

void record(std::uint64_t body_tx, std::uint64_t body_rx)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    load_locked();
    s_counter.add_request(s_transport, body_tx, body_rx);
}

Totals snapshot()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    load_locked();
    return s_counter.totals();
}

void acknowledge(const Totals &reported)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    load_locked();
    s_counter.acknowledge(reported);
}

void persist()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_loaded || !s_counter.dirty()) {
        return;
    }
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    const std::string text = s_counter.serialize();
    if (nvs_set_str(handle, kKey, text.c_str()) == ESP_OK &&
        nvs_commit(handle) == ESP_OK) {
        s_counter.mark_saved();
    } else {
        ESP_LOGW(TAG, "Could not store the data usage");
    }
    nvs_close(handle);
}

}  // namespace uplink::usage
