/*
 * The sync of a cycle: picks the destination (own backend or WebDAV),
 * installs a newer firmware at the end and pings the health check.
 */

#include "uplink.hpp"

#include <stdio.h>
#include <sys/time.h>
#include <utility>
#include <vector>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "backend_internal.hpp"
#include "camera_actions.hpp"
#include "camera_wifi.h"
#include "delete_request.hpp"
#include "destinations.hpp"
#include "device_info.hpp"
#include "cycle_trace.hpp"
#include "hub_battery.hpp"
#include "http_util.hpp"
#include "hub_config.hpp"
#include "media_catalog.hpp"
#include "media_store.hpp"
#include "modem_uplink.hpp"
#include "mp4_body.hpp"
#include "mp4_mux.hpp"
#include "ota_policy.hpp"
#include "ota_update.hpp"
#include "schedule_state.hpp"
#include "sync_cameras.hpp"
#include "system_diagnostics.h"
#include "time_sync.h"
#include "uplink_protocol.hpp"
#include "video_request.hpp"

namespace uplink
{

namespace backend
{

namespace
{

const char *TAG = "trailcamhub_uplink";

}  // namespace

// Looks for a newer release on GitHub and installs it if the battery and the
// uplink allow it; the scheduler restarts the hub after the cycle.
void update_firmware(const hub_config::Settings &settings)
{
    // The battery as read between cycles: at rest, not under the modem's
    // load.
    const schedule::State state = schedule::state();
    ota::Conditions conditions;
    conditions.battery_percent = state.hub_battery;
    conditions.charging = state.hub_charging;
    conditions.power_level = (int)schedule::hub_level(state);
    conditions.over_modem =
        hub_config::uplink_transport(settings) == hub_config::Transport::modem;
    conditions.signal_dbm =
        conditions.over_modem ? modem_uplink::signal_dbm() : 0;
    // A firmware image is ~1.7 MB: no modem sleep while it may download.
    if (!conditions.over_modem) {
        camera_wifi_set_full_power(true);
    }
    ota::check_for_update(conditions);
    if (!conditions.over_modem) {
        camera_wifi_set_full_power(false);
    }
}

}  // namespace backend

namespace
{

const char *TAG = "trailcamhub_uplink";

// Tells the health check service (setting health_url) how the sync went;
// it warns the user when these calls stop.
void ping_health_check(const hub_config::Settings &settings, bool ok)
{
    if (settings.health_url.empty()) {
        return;
    }
    Request request;
    request.url = settings.health_url + (ok ? "" : "/fail");
    const HttpResult result = send(request);
    if (result.error != ESP_OK || result.status >= 300) {
        ESP_LOGW(TAG, "Health check: %s", describe(result).c_str());
    }
}

esp_err_t run_destination(const hub_config::Settings &settings, Report &report)
{
    const hub_config::Destination destination =
        hub_config::destination(settings);
    if (destination == hub_config::Destination::backend) {
        return backend::run_backend(settings, report);
    }
    report = Report{};
    const bool over_modem =
        hub_config::uplink_transport(settings) == hub_config::Transport::modem;
    const esp_err_t error = push_webdav(settings, over_modem, report);
    if (error == ESP_OK) {
        // The firmware reached its destination: a new one is kept. Then a
        // newer firmware, last, as with the backend.
        ota::confirm_running();
        backend::update_firmware(settings);
    }
    return error;
}

}  // namespace

esp_err_t run(const hub_config::Settings &settings, Report &report)
{
    const esp_err_t error = run_destination(settings, report);
    ping_health_check(settings, error == ESP_OK);
    return error;
}

}  // namespace uplink
