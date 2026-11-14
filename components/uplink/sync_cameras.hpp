#pragma once

// The cameras on the SD card during one sync: each camera's catalog, loaded
// once, changed by the destination (what it knows, which videos were asked
// for) and saved when it changed. Shared by the backend, WebDAV and Telegram
// destinations (#218); owned by the uplink task for the length of a sync.

#include <string>
#include <vector>

#include "media_catalog.hpp"

namespace uplink
{

struct SyncCamera {
    // The directory name on the SD card (the sanitized camera id).
    std::string id;
    camera::artemis::MediaCatalog catalog;
    // The catalog differs from the file on the card.
    bool changed = false;
};

// Loads the catalog of camera `id`; a missing or unreadable file gives an
// empty catalog.
SyncCamera load_camera(const std::string &id);

// Every camera that has a directory on the SD card.
std::vector<SyncCamera> load_cameras();

// The camera with this id, or null.
SyncCamera *find_camera(std::vector<SyncCamera> &cameras,
                        const std::string &id);

// Saves the catalogs that changed (and logs one that could not be saved; it
// counts as saved, so that it is not written again in this sync).
void save_changed(std::vector<SyncCamera> &cameras);

}  // namespace uplink
