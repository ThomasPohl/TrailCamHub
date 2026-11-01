#include "time_sync.h"

#include <atomic>

#include <sys/time.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "sdkconfig.h"

using timepolicy::Source;

static const char *TAG = "trailcamhub_time";

// A clock before 2024 has not been set.
#define CLOCK_VALID_FROM 1704067200

static std::atomic<int> s_source{static_cast<int>(Source::none)};

bool time_sync_clock_valid(void)
{
    return time(NULL) >= CLOCK_VALID_FROM;
}

Source time_sync_source(void)
{
    return static_cast<Source>(s_source.load());
}

bool time_sync_trusted(void)
{
    return time_sync_clock_valid() && timepolicy::trusted(time_sync_source());
}

// When this firmware was built; a time before that cannot be right.
static int64_t build_time(void)
{
    static int64_t build = -1;
    if (build < 0) {
        const esp_app_desc_t *description = esp_app_get_description();
        build = description == NULL ? 0
                                    : timepolicy::parse_build_time(
                                          description->date, description->time);
    }
    return build;
}

bool time_sync_set(int64_t unix_seconds, Source source)
{
#if !CONFIG_TIME_SYNC_CAMERA
    if (source == Source::camera) {
        return false;
    }
#endif
#if !CONFIG_TIME_SYNC_BROWSER
    if (source == Source::browser) {
        return false;
    }
#endif
    if (!timepolicy::plausible(unix_seconds, build_time())) {
        ESP_LOGW(TAG, "Ignoring %lld from the %s: not a plausible time",
                 (long long)unix_seconds, timepolicy::source_name(source));
        return false;
    }
    if (!timepolicy::may_set(time_sync_source(), source,
                             time_sync_clock_valid())) {
        return false;
    }
    struct timeval now = {};
    now.tv_sec = static_cast<time_t>(unix_seconds);
    if (settimeofday(&now, NULL) != 0) {
        return false;
    }
    s_source = static_cast<int>(source);
    ESP_LOGI(TAG, "Clock set from the %s: %lld",
             timepolicy::source_name(source), (long long)unix_seconds);
    return true;
}

#if CONFIG_TIME_SYNC_SNTP
// The SNTP client has set the clock itself; this records where it came from.
static void on_time_synced(struct timeval *tv)
{
    s_source = static_cast<int>(Source::sntp);
    ESP_LOGI(TAG, "Clock set by SNTP: %lld", (long long)tv->tv_sec);
}

// The first requests may have run before a network was up, and the client
// then waits a long time before it asks again: ask as soon as one has an
// address.
static void on_ip_event(void *arg, esp_event_base_t base, int32_t id,
                        void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    if (time_sync_source() == Source::sntp) {
        return;
    }
    esp_sntp_stop();
    esp_sntp_init();
}

esp_err_t time_sync_start(void)
{
    esp_sntp_config_t config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_TIME_SYNC_SERVER);
    config.wait_for_sync = false;
    config.sync_cb = on_time_synced;
    esp_err_t error = esp_netif_sntp_init(&config);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not start SNTP: %s", esp_err_to_name(error));
        return error;
    }
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip_event,
                               NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event,
                               NULL);
    ESP_LOGI(TAG, "Setting the clock from %s", CONFIG_TIME_SYNC_SERVER);
    return ESP_OK;
}
#else
esp_err_t time_sync_start(void)
{
    return ESP_OK;
}
#endif
