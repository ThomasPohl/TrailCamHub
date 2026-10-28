#include "media_transfer.hpp"

namespace camera::artemis
{

namespace
{

std::string build_media_request(std::int64_t cmd_id, const char *array_key,
                                std::int64_t file_type, std::int64_t dir_num,
                                std::int64_t media_num, std::int64_t token)
{
    return "{\"cmdId\":" + std::to_string(cmd_id) + ",\"" + array_key +
           "\":[{\"fileType\":" + std::to_string(file_type) +
           ",\"dirNum\":" + std::to_string(dir_num) +
           ",\"mediaNum\":" + std::to_string(media_num) +
           "}],\"token\":" + std::to_string(token) + "}";
}

std::int64_t read_u16_le(const std::uint8_t *p)
{
    return static_cast<std::int64_t>(p[0]) |
           (static_cast<std::int64_t>(p[1]) << 8);
}

std::int64_t read_u32_le(const std::uint8_t *p)
{
    return static_cast<std::int64_t>(p[0]) |
           (static_cast<std::int64_t>(p[1]) << 8) |
           (static_cast<std::int64_t>(p[2]) << 16) |
           (static_cast<std::int64_t>(p[3]) << 24);
}

}  // namespace

std::string build_thumbnail_request(std::int64_t file_type,
                                    std::int64_t dir_num,
                                    std::int64_t media_num, std::int64_t token)
{
    return build_media_request(772, "thumbnailReqs", file_type, dir_num,
                               media_num, token);
}

std::string build_thumbnail_request(const MediaItem &item, std::int64_t token)
{
    return build_thumbnail_request(item.file_type, item.dir_num, item.num,
                                   token);
}

std::string build_download_request(std::int64_t file_type, std::int64_t dir_num,
                                   std::int64_t media_num, std::int64_t token)
{
    return build_media_request(1285, "downloadReqs", file_type, dir_num,
                               media_num, token);
}

std::string build_download_request(const MediaItem &item, std::int64_t token)
{
    return build_download_request(item.file_type, item.dir_num, item.num,
                                  token);
}

bool parse_media_record(const std::uint8_t *payload, std::size_t payload_size,
                        MediaRecordHeader &out, std::size_t &jpeg_offset,
                        std::size_t &jpeg_size)
{
    if (payload == nullptr || payload_size < kMediaRecordHeaderSize)
        return false;
    out.dir_num = read_u16_le(payload + 0x20);
    out.media_num = read_u16_le(payload + 0x22);
    out.media_id = read_u32_le(payload + 0x30);
    jpeg_offset = kMediaRecordHeaderSize;
    jpeg_size = payload_size - kMediaRecordHeaderSize;
    return true;
}

}  // namespace camera::artemis
