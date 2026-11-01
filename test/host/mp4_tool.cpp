// Builds an MP4 from a raw .h264 and a .aac file with the firmware's muxer,
// for checking its output with other tools (ffmpeg, VLC):
//   mp4_tool video.h264 audio.aac out.mp4 [video_duration_ms]
// Reads the output in pieces of odd sizes, as a web server would.

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "mp4_mux.hpp"

namespace
{
class FileSource : public mp4::Source
{
  public:
    explicit FileSource(const char *path)
    {
        m_file = std::fopen(path, "rb");
    }
    ~FileSource() override
    {
        if (m_file != nullptr) {
            std::fclose(m_file);
        }
    }
    bool ok() const
    {
        return m_file != nullptr;
    }
    std::uint64_t size() const override
    {
        std::fseek(m_file, 0, SEEK_END);
        return static_cast<std::uint64_t>(std::ftell(m_file));
    }
    bool read(std::uint64_t offset, std::uint8_t *destination,
              std::size_t length) override
    {
        return std::fseek(m_file, static_cast<long>(offset), SEEK_SET) == 0 &&
               std::fread(destination, 1, length, m_file) == length;
    }

  private:
    std::FILE *m_file = nullptr;
};
}  // namespace

int main(int argc, char **argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s video.h264 audio.aac out.mp4 [ms]\n",
                     argv[0]);
        return 2;
    }
    FileSource video(argv[1]);
    FileSource audio(argv[2]);
    if (!video.ok() || !audio.ok()) {
        std::fprintf(stderr, "cannot open the input files\n");
        return 1;
    }
    mp4::Options options;
    if (argc > 4) {
        options.video_duration_ms = static_cast<std::uint32_t>(atoi(argv[4]));
    }
    mp4::Mp4File file;
    if (!file.open(video, &audio, options)) {
        std::fprintf(stderr, "open failed: %s\n", file.error().c_str());
        return 1;
    }
    std::fprintf(stderr,
                 "%ux%u, %u video frames (%u skipped before the first key "
                 "frame), %u audio frames, %u ms, %llu bytes\n",
                 file.width(), file.height(), file.video_frames(),
                 file.skipped_frames(), file.audio_frames(), file.duration_ms(),
                 (unsigned long long)file.size());
    std::FILE *out = std::fopen(argv[3], "wb");
    if (out == nullptr) {
        return 1;
    }
    std::vector<std::uint8_t> buffer(4093);
    for (std::uint64_t position = 0; position < file.size();) {
        const std::int64_t n =
            file.read(position, buffer.data(), buffer.size());
        if (n <= 0) {
            std::fprintf(stderr, "read failed at %llu\n",
                         (unsigned long long)position);
            return 1;
        }
        std::fwrite(buffer.data(), 1, static_cast<std::size_t>(n), out);
        position += static_cast<std::uint64_t>(n);
    }
    std::fclose(out);
    return 0;
}
