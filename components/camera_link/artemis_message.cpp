#include "artemis_message.hpp"

#include <cstring>

#include "artemis_crypto.hpp"
#include "camera_udp_packets.hpp"

namespace camera::artemis
{
namespace
{

constexpr std::size_t kHeaderSize = 20;
constexpr std::uint8_t kSignature[8] = {'A', 'R', 'T', 'E', 'M', 'I', 'S', 0};

std::uint32_t read_u32_le(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

std::vector<std::uint8_t> build_request(const Aes128 &cipher,
                                        std::uint32_t app_sequence,
                                        const std::string &json)
{
    const std::string body = encode(cipher, json);
    camera::ArtemisCommandRequest request;
    if (!request.build(app_sequence, body.c_str(), body.size()))
        return {};
    return std::vector<std::uint8_t>(request.data(),
                                     request.data() + request.size());
}

std::vector<std::uint8_t> Heartbeat::next(const Aes128 &cipher)
{
    count_ = static_cast<std::uint16_t>(count_ == 0xffff ? 1 : count_ + 1);
    return build_request(cipher, kHeartbeatSequenceBase + count_,
                         kHeartbeatJson);
}

bool parse(const Aes128 &cipher, const std::uint8_t *bytes, std::size_t size,
           Message &out)
{
    if (bytes == nullptr || size < kHeaderSize)
        return false;
    if (std::memcmp(bytes, kSignature, sizeof(kSignature)) != 0)
        return false;

    const std::uint32_t payload_len = read_u32_le(bytes + 16);
    if (payload_len == 0 || payload_len > size - kHeaderSize)
        return false;

    // PayloadLen covers the Base64 string plus its NUL terminator and any
    // trailing zero padding, so the Base64 ends at the first NUL.
    const std::uint8_t *body = bytes + kHeaderSize;
    std::size_t base64_len = 0;
    while (base64_len < payload_len && body[base64_len] != 0)
        ++base64_len;

    std::string json;
    if (!decode(cipher,
                std::string(reinterpret_cast<const char *>(body), base64_len),
                json))
        return false;

    out.msg_type = read_u32_le(bytes + 8);
    out.app_sequence = read_u32_le(bytes + 12);
    out.json = std::move(json);
    return true;
}

}  // namespace camera::artemis
