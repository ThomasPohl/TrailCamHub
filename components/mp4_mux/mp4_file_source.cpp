#include "mp4_file_source.hpp"

#include <sys/stat.h>
#include <utility>

namespace mp4
{

FileSource::FileSource(std::string path) : path_(std::move(path))
{
    struct stat info = {};
    if (stat(path_.c_str(), &info) == 0 && S_ISREG(info.st_mode)) {
        size_ = static_cast<std::uint64_t>(info.st_size);
        exists_ = true;
    }
}

FileSource::~FileSource()
{
    release();
}

void FileSource::release()
{
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    position_ = 0;
}

bool FileSource::read(std::uint64_t offset, std::uint8_t *destination,
                      std::size_t length)
{
    if (!exists_ || offset > size_ || length > size_ - offset) {
        return false;
    }
    if (file_ == nullptr) {
        file_ = std::fopen(path_.c_str(), "rb");
        position_ = 0;
        if (file_ == nullptr) {
            return false;
        }
    }
    bool ok = true;
    if (offset != position_ &&
        std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0) {
        ok = false;
    }
    if (ok && length > 0 &&
        std::fread(destination, 1, length, file_) != length) {
        ok = false;
    }
    // After a failure the position is unknown: seek next time.
    position_ = ok ? offset + length : UINT64_MAX;
    return ok;
}

}  // namespace mp4
