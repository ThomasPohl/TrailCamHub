#pragma once

// How the catalog file is replaced and how often it is saved. Pure (stdio
// only), host-tested; the SD paths and the logging are in media_store.cpp.
//
// The catalog is replaced through a temporary file: write "<path>.tmp",
// remove "<path>", rename the temporary file (FAT cannot rename onto an
// existing file). A power loss between the last two steps leaves only the
// complete temporary file; recover_replaced_file() finishes the rename on
// the next load, so the catalog is never lost.

#include <cstdint>
#include <string>

namespace camera::store
{

// Path of the temporary file that replaces `path`.
std::string replacement_path(const std::string &path);

// Replaces `path` with its temporary file, which must be complete and
// closed. Returns false if the rename failed; the temporary file stays then.
bool replace_file(const std::string &path);

// Finishes an interrupted replace_file(): if `path` is missing and its
// temporary file exists, renames it. Returns true if a file was recovered.
// A temporary file next to an existing `path` may be incomplete (the write
// was interrupted) and is left alone; the next save overwrites it.
bool recover_replaced_file(const std::string &path);

// Decides when a catalog that changes often is written. Saving rewrites the
// whole file, so storing a thumbnail does not save each time: a save is due
// after `max_changes` unsaved changes or once the oldest unsaved change is
// `max_age_us` old. What is unsaved when the power fails is only fetched
// again in the next session.
class SaveThrottle
{
  public:
    SaveThrottle(unsigned max_changes, std::int64_t max_age_us)
        : max_changes_(max_changes), max_age_us_(max_age_us)
    {
    }

    // Counts a change made at `now_us`. Returns true if a save is due.
    bool note_change(std::int64_t now_us);
    // There are changes that were not saved.
    bool pending() const
    {
        return pending_ > 0;
    }
    // The catalog was written (or the changes were dropped with it).
    void saved()
    {
        pending_ = 0;
    }

  private:
    unsigned max_changes_;
    std::int64_t max_age_us_;
    unsigned pending_ = 0;
    std::int64_t oldest_us_ = 0;
};

}  // namespace camera::store
