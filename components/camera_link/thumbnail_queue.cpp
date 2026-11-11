#include "thumbnail_queue.hpp"

#include <utility>

namespace camera::link
{

void ThumbnailQueue::start(std::vector<artemis::CatalogEntry> items)
{
    items_ = std::move(items);
    index_ = 0;
    retried_ = false;
}

void ThumbnailQueue::release()
{
    std::vector<artemis::CatalogEntry>().swap(items_);
    index_ = 0;
    retried_ = false;
}

const artemis::CatalogEntry *ThumbnailQueue::current() const
{
    return index_ < items_.size() ? &items_[index_] : nullptr;
}

std::string ThumbnailQueue::request_fields() const
{
    const artemis::CatalogEntry *e = current();
    if (e == nullptr)
        return std::string();
    return ",\"thumbnailReqs\":[{\"fileType\":" + std::to_string(e->file_type) +
           ",\"dirNum\":" + std::to_string(e->dir_num) +
           ",\"mediaNum\":" + std::to_string(e->media_num) + "}]";
}

void ThumbnailQueue::advance()
{
    if (index_ < items_.size())
        ++index_;
    retried_ = false;
}

ThumbnailQueue::Stall ThumbnailQueue::on_stall()
{
    if (!retried_) {
        retried_ = true;
        return Stall::request_again;
    }
    advance();
    return Stall::skipped;
}

}  // namespace camera::link
