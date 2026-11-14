#include "sync_cameras.hpp"

#include <utility>

#include "esp_log.h"

#include "media_store.hpp"

namespace uplink
{

namespace
{

const char *TAG = "trailcamhub_sync";

}  // namespace

SyncCamera load_camera(const std::string &id)
{
    SyncCamera camera;
    camera.id = id;
    camera::store::load_catalog(id, camera.catalog);
    return camera;
}

std::vector<SyncCamera> load_cameras()
{
    std::vector<SyncCamera> cameras;
    for (const std::string &id : camera::store::list_cameras()) {
        cameras.push_back(load_camera(id));
    }
    return cameras;
}

SyncCamera *find_camera(std::vector<SyncCamera> &cameras, const std::string &id)
{
    for (SyncCamera &camera : cameras) {
        if (camera.id == id) {
            return &camera;
        }
    }
    return nullptr;
}

void save_changed(std::vector<SyncCamera> &cameras)
{
    for (SyncCamera &camera : cameras) {
        if (camera.changed &&
            !camera::store::save_catalog(camera.id, camera.catalog)) {
            ESP_LOGW(TAG, "Could not save the catalog of %s",
                     camera.id.c_str());
        }
        camera.changed = false;
    }
}

}  // namespace uplink
