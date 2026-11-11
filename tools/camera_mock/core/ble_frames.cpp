#include "ble_frames.hpp"

#include <cstdio>
#include <cstring>

namespace camera_mock::ble
{

namespace
{

constexpr std::size_t kMaxAdvertisingData = 31;
constexpr std::uint8_t kFrameHeader[4] = {0x13, 0x57, 0x01, 0x00};
constexpr char kWakeCommand[] = "BT_KEY_ON";

std::string json_escape(const std::string &text)
{
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

}  // namespace

std::vector<std::uint8_t> advertising_data(const std::uint8_t dev_id[6],
                                           std::uint16_t product_id,
                                           const std::string &name)
{
    // Flags, the 16-bit service UUID (little-endian), manufacturer data.
    std::vector<std::uint8_t> out = {
        0x02, 0x01, 0x06, 0x03, 0x03, kServiceUuid & 0xff, kServiceUuid >> 8,
        0x0b, 0xff, 0x87, 0x19};
    out.insert(out.end(), dev_id, dev_id + 6);
    out.push_back(static_cast<std::uint8_t>(product_id >> 8));
    out.push_back(static_cast<std::uint8_t>(product_id));
    const std::size_t room = kMaxAdvertisingData - out.size() - 2;
    const std::size_t length = name.size() < room ? name.size() : room;
    out.push_back(static_cast<std::uint8_t>(length + 1));
    out.push_back(0x09);  // complete local name
    out.insert(out.end(), name.begin(), name.begin() + length);
    return out;
}

bool is_wake_command(const std::uint8_t *data, std::size_t size)
{
    if (data == nullptr) {
        return false;
    }
    if (size == sizeof(kWakeCommand) - 1 &&
        std::memcmp(data, kWakeCommand, size) == 0) {
        return true;
    }
    return size >= sizeof(kFrameHeader) &&
           std::memcmp(data, kFrameHeader, sizeof(kFrameHeader)) == 0;
}

std::vector<std::uint8_t> credentials_frame(const std::string &ssid,
                                            const std::string &bssid,
                                            const std::string &password)
{
    const std::string json = "{\"ret\":0,\"ssid\":\"" + json_escape(ssid) +
                             "\",\"bssid\":\"" + json_escape(bssid) +
                             "\",\"pwd\":\"" + json_escape(password) + "\"}";
    std::vector<std::uint8_t> out(kFrameHeader,
                                  kFrameHeader + sizeof(kFrameHeader));
    const std::uint32_t length = static_cast<std::uint32_t>(json.size());
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>(length >> shift));
    }
    out.insert(out.end(), json.begin(), json.end());
    return out;
}

std::vector<std::vector<std::uint8_t>>
split_notifications(const std::vector<std::uint8_t> &frame, std::size_t chunk)
{
    std::vector<std::vector<std::uint8_t>> out;
    if (chunk == 0) {
        return out;
    }
    for (std::size_t offset = 0; offset < frame.size(); offset += chunk) {
        const std::size_t size =
            frame.size() - offset < chunk ? frame.size() - offset : chunk;
        out.emplace_back(frame.begin() + offset, frame.begin() + offset + size);
    }
    return out;
}

std::string format_mac(const std::uint8_t mac[6])
{
    char text[18];
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
                  mac[1], mac[2], mac[3], mac[4], mac[5]);
    return text;
}

std::string ap_ssid(const std::uint8_t bssid[6])
{
    char text[16];
    std::snprintf(text, sizeof(text), "TrailCam_%02X%02X", bssid[4], bssid[5]);
    return text;
}

}  // namespace camera_mock::ble
