#include "camera_actions.hpp"

#include <mutex>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "action_store.hpp"
#include "sd_card.h"
#include "time_sync.h"

namespace camera::action::service
{

namespace
{

const char *TAG = "trailcamhub_actions";
constexpr const char *kQueuePath = SD_CARD_MOUNT_POINT "/actions.csv";

std::mutex s_lock;
ActionQueue s_queue;
ConfirmGate s_gate;
// Saved only once the queue was loaded: an empty queue must not replace the
// file of a card that could not be read.
bool s_persistent;

std::int64_t now_s()
{
    return time_sync_clock_valid() ? static_cast<std::int64_t>(time(NULL)) : 0;
}

std::int64_t now_us()
{
    return esp_timer_get_time();
}

// Called with the lock held.
void save()
{
    if (s_persistent && !save_queue(kQueuePath, s_queue)) {
        ESP_LOGW(TAG, "Could not save the action queue to the SD card");
    }
}

}  // namespace

void init()
{
    std::lock_guard<std::mutex> guard(s_lock);
    if (load_queue(kQueuePath, s_queue, now_s())) {
        s_persistent = true;
        ESP_LOGI(TAG, "Action queue: %u entries, %u waiting",
                 (unsigned)s_queue.entries().size(),
                 (unsigned)s_queue.pending_count());
        // An old action may have waited for days: drop it now, and save what
        // load_queue() changed.
        s_queue.expire_stale(now_s());
        save();
    } else {
        ESP_LOGW(TAG, "Could not read %s; the queue is not kept", kQueuePath);
    }
}

ActionQueue::AddResult submit(const Action &action, Source source,
                              const std::string &key)
{
    std::lock_guard<std::mutex> guard(s_lock);
    const ActionQueue::AddResult result =
        s_queue.add(action, source, key, now_s());
    if (result.kind == ActionQueue::AddResult::Kind::added) {
        ESP_LOGI(TAG, "Queued action %u: %s (from %s)", (unsigned)result.id,
                 type_name(action.type), source_name(source));
        save();
    }
    return result;
}

std::string request_confirmation(const std::string &subject)
{
    std::lock_guard<std::mutex> guard(s_lock);
    return s_gate.request(subject, now_us(), esp_random());
}

ConfirmGate::Result confirm(const std::string &subject, const std::string &code)
{
    std::lock_guard<std::mutex> guard(s_lock);
    return s_gate.confirm(subject, code, now_us());
}

std::vector<Entry> snapshot()
{
    std::lock_guard<std::mutex> guard(s_lock);
    return s_queue.entries();
}

std::vector<Entry> take_unreported(Source source)
{
    std::lock_guard<std::mutex> guard(s_lock);
    std::vector<Entry> out;
    for (const Entry *entry : s_queue.unreported_results(source)) {
        out.push_back(*entry);
    }
    for (const Entry &entry : out) {
        s_queue.mark_notified(entry.id);
    }
    if (!out.empty()) {
        save();
    }
    return out;
}

bool has_pending()
{
    std::lock_guard<std::mutex> guard(s_lock);
    return s_queue.pending_count() > 0;
}

bool pending_needs_list()
{
    std::lock_guard<std::mutex> guard(s_lock);
    return s_queue.has_pending_needing_list();
}

bool take_next(bool list_ready, Job &job)
{
    std::lock_guard<std::mutex> guard(s_lock);
    s_queue.expire_stale(now_s());
    for (const Entry &entry : s_queue.entries()) {
        if (entry.state != State::pending ||
            (needs_media_list(entry.action.type) && !list_ready)) {
            continue;
        }
        job.id = entry.id;
        job.action = entry.action;
        s_queue.start(job.id);
        // Saved before the command goes out: if the power fails now, the
        // action is not repeated (it ends as failed on the next start).
        save();
        return true;
    }
    return false;
}

void finish(std::uint32_t id, const Outcome &outcome)
{
    std::lock_guard<std::mutex> guard(s_lock);
    if (s_queue.finish(id, outcome, now_s())) {
        if (outcome.done) {
            ESP_LOGI(TAG, "Action %u done", (unsigned)id);
        } else {
            ESP_LOGW(TAG, "Action %u failed: %s", (unsigned)id,
                     outcome.reason.c_str());
        }
        save();
    }
}

void abandon_running(const std::string &reason)
{
    std::lock_guard<std::mutex> guard(s_lock);
    if (s_queue.abandon_running(reason, now_s()) > 0) {
        save();
    }
}

void fail_pending(const std::string &reason)
{
    std::lock_guard<std::mutex> guard(s_lock);
    if (s_queue.fail_pending(reason, now_s()) > 0) {
        save();
    }
}

}  // namespace camera::action::service
