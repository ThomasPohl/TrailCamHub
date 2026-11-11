#pragma once

// The thumbnails a camera session still has to fetch (cmdId 772), out of
// camera_protocol.cpp (#65): which one is requested next, what the request
// says, and what happens when a thumbnail stops arriving. Pure logic (no
// ESP-IDF), host-tested; camera_protocol.cpp sends the requests and stores
// the answers.

#include <cstddef>
#include <string>
#include <vector>

#include "media_catalog.hpp"

namespace camera::link
{

class ThumbnailQueue
{
  public:
    // What to do with a thumbnail that stopped arriving.
    enum class Stall {
        request_again,  // first stall of this thumbnail: current() unchanged
        skipped,        // second stall: moved on to the next thumbnail
    };

    // Starts over with `items`, fetched in this order.
    void start(std::vector<artemis::CatalogEntry> items);
    // Empties the queue and gives its memory back.
    void release();

    // The thumbnail to request, or nullptr when all were handled.
    const artemis::CatalogEntry *current() const;
    // The fields of the request for current() (",\"thumbnailReqs\":[...]"),
    // empty when all were handled.
    std::string request_fields() const;

    // current() arrived (stored or not usable): moves on to the next one.
    void advance();
    // current() stopped arriving. The first time it is requested again; the
    // second time it is skipped, so that one bad thumbnail cannot hold up
    // the session.
    Stall on_stall();
    // current() was requested again after a stall.
    bool retrying() const
    {
        return retried_;
    }

    std::size_t size() const
    {
        return items_.size();
    }
    // Thumbnails handled so far (arrived or skipped).
    std::size_t handled() const
    {
        return index_;
    }

  private:
    std::vector<artemis::CatalogEntry> items_;
    std::size_t index_ = 0;
    bool retried_ = false;
};

}  // namespace camera::link
