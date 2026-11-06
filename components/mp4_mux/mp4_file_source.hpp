#pragma once

// A stream file on disk as an mp4::Source (stdio only, host-testable). The
// file is opened on the first read and the position is remembered, so the
// sequential scans of Mp4File::open() do not seek.

#include <cstdint>
#include <cstdio>
#include <string>

#include "mp4_mux.hpp"

namespace mp4
{

class FileSource : public Source
{
  public:
    explicit FileSource(std::string path);
    ~FileSource() override;
    FileSource(const FileSource &) = delete;
    FileSource &operator=(const FileSource &) = delete;

    // False if the file does not exist (size() is then 0).
    bool exists() const
    {
        return exists_;
    }
    std::uint64_t size() const override
    {
        return size_;
    }
    bool read(std::uint64_t offset, std::uint8_t *destination,
              std::size_t length) override;
    // Closes the file; the next read opens it again.
    void release();

  private:
    std::string path_;
    std::FILE *file_ = nullptr;
    bool exists_ = false;
    std::uint64_t size_ = 0;
    std::uint64_t position_ = 0;
};

}  // namespace mp4
