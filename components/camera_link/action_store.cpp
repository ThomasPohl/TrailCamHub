#include "action_store.hpp"

#include <stdio.h>

#include "catalog_file.hpp"

namespace camera::action
{

bool load_queue(const std::string &path, ActionQueue &queue, std::int64_t now_s)
{
    camera::store::recover_replaced_file(path);
    FILE *file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        queue.parse("v1,1\n");
        return true;
    }
    std::string text;
    char buffer[512];
    size_t got = 0;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        text.append(buffer, got);
    }
    fclose(file);
    if (!queue.parse(text)) {
        return false;
    }
    queue.abandon_running(
        "the hub restarted before the camera answered; not repeated", now_s);
    return true;
}

bool save_queue(const std::string &path, const ActionQueue &queue)
{
    const std::string tmp = camera::store::replacement_path(path);
    FILE *file = fopen(tmp.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const std::string text = queue.serialize();
    const bool wrote = fwrite(text.data(), 1, text.size(), file) == text.size();
    const bool closed = fclose(file) == 0;
    if (!wrote || !closed) {
        remove(tmp.c_str());
        return false;
    }
    return camera::store::replace_file(path);
}

}  // namespace camera::action
