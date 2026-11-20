// The C interface to the camera session (camera_session.hpp): the settings
// that other tasks make before a session, the session itself, and the result
// of the last one, which they read after it.

#include <atomic>
#include <mutex>
#include <stdio.h>
#include <string>

#include "esp_attr.h"
#include "esp_log.h"

#include "artemis_login.hpp"
#include "camera_link_io.hpp"
#include "camera_protocol.h"
#include "camera_session.hpp"
#include "pppp_session.hpp"
#include "sd_writer.hpp"

static const char *TAG = "trailcamhub_protocol";

static camera::link::UdpTransport s_transport;
// Static because it holds the DATA packets awaiting acknowledgement.
// In PSRAM where the build allows it (about 8 KB; internal RAM is short).
EXT_RAM_BSS_ATTR static camera::pppp::Session s_session(s_transport);
// Static, in internal RAM: its buffers are big and it is used again for every
// session (see camera_session.hpp).
static camera::link::CameraSession s_camera_session(s_transport, s_session);

// What the other tasks set before a session; a session works with a copy.
static std::mutex s_settings_lock;
static camera::link::SessionSettings s_settings;

// How the last session ended, for the other tasks.
static std::mutex s_result_lock;
static camera::link::SessionResult s_result;
// The battery of the last camera that reported one, since boot (-1: none).
static std::atomic<int> s_last_battery_percent{-1};

esp_err_t camera_protocol_run(const esp_ip4_addr_t *gateway)
{
    if (gateway == NULL || gateway->addr == 0) {
        ESP_LOGE(TAG, "Cannot start camera protocol without a valid gateway");
        return ESP_ERR_INVALID_ARG;
    }
    camera::link::SessionSettings settings;
    {
        std::lock_guard<std::mutex> lock(s_settings_lock);
        settings = s_settings;
    }
    {
        // What the last session did is gone as soon as the next one starts.
        std::lock_guard<std::mutex> lock(s_result_lock);
        s_result = camera::link::SessionResult();
    }
    const camera::link::SessionResult result =
        s_camera_session.run(*gateway, settings);
    if (result.battery_percent >= 0) {
        s_last_battery_percent = result.battery_percent;
    }
    {
        std::lock_guard<std::mutex> lock(s_result_lock);
        s_result = result;
    }
    return result.error;
}

void camera_protocol_request_stop(void)
{
    camera::link::request_stop();
}

bool camera_protocol_work_completed(void)
{
    std::lock_guard<std::mutex> lock(s_result_lock);
    return s_result.work_completed;
}

bool camera_protocol_media_list_error(void)
{
    std::lock_guard<std::mutex> lock(s_result_lock);
    return s_result.media_list_error;
}

uint32_t camera_protocol_stored_items(void)
{
    std::lock_guard<std::mutex> lock(s_result_lock);
    return s_result.stored_items;
}

uint32_t camera_protocol_listed_files(void)
{
    std::lock_guard<std::mutex> lock(s_result_lock);
    return s_result.listed_files;
}

void camera_protocol_last_camera_id(char *out, size_t size)
{
    if (out != NULL && size > 0) {
        std::lock_guard<std::mutex> lock(s_result_lock);
        snprintf(out, size, "%s", s_result.camera_id.c_str());
    }
}

int camera_protocol_last_battery_percent(void)
{
    return s_last_battery_percent.load();
}

void camera_protocol_init_storage(void)
{
    camera::store::sd_writer_start();
}

void camera_protocol_set_login(const char *user, const char *password)
{
    std::lock_guard<std::mutex> lock(s_settings_lock);
    s_settings.login_user =
        user != NULL && user[0] != '\0' ? user : camera::artemis::kDefaultUser;
    s_settings.login_password = password != NULL && password[0] != '\0'
                                    ? password
                                    : camera::artemis::kDefaultPassword;
}

void camera_protocol_set_all_videos(bool all_videos)
{
    std::lock_guard<std::mutex> lock(s_settings_lock);
    s_settings.all_videos = all_videos;
}

void camera_protocol_set_actions_only(bool actions_only)
{
    std::lock_guard<std::mutex> lock(s_settings_lock);
    s_settings.actions_only = actions_only;
}

void camera_protocol_set_list_only(bool list_only)
{
    std::lock_guard<std::mutex> lock(s_settings_lock);
    s_settings.list_only = list_only;
}

void camera_protocol_set_list_callback(camera_protocol_list_cb_t cb)
{
    std::lock_guard<std::mutex> lock(s_settings_lock);
    s_settings.list_callback = cb;
}

void camera_protocol_set_progress_callback(camera_protocol_progress_cb_t cb)
{
    std::lock_guard<std::mutex> lock(s_settings_lock);
    s_settings.progress_callback = cb;
}
