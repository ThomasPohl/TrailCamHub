#pragma once

// The action queue as a file on the SD card (#194). Written through a
// temporary file like the catalog (catalog_file.hpp), so a power loss leaves
// either the old or the new queue. Pure (stdio only), host-tested.

#include <string>

#include "action_queue.hpp"

namespace camera::action
{

// Loads the queue from `path` (finishing an interrupted replace first). A
// missing file is an empty queue and returns true; an unreadable one returns
// false and leaves the queue empty. Entries that were running when the file
// was written end as failed ("the hub restarted ..."): they are not
// repeated.
bool load_queue(const std::string &path, ActionQueue &queue,
                std::int64_t now_s);

// Writes the queue to `path`. Returns false if that failed; the old file is
// kept then.
bool save_queue(const std::string &path, const ActionQueue &queue);

}  // namespace camera::action
