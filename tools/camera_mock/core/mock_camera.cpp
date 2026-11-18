#include "mock_camera.hpp"

#include <cstdio>
#include <cstring>

#include "artemis_crypto.hpp"
#include "artemis_message.hpp"
#include "capture_fixtures.hpp"
#include "mock_wire.hpp"

namespace camera_mock
{

namespace
{

constexpr std::size_t kArtemisHeaderSize = 20;
constexpr std::uint8_t kArtemisSignature[8] = {'A', 'R', 'T', 'E',
                                               'M', 'I', 'S', 0};
constexpr std::uint32_t kMsgResponse = 3;

// Bootstrap key of the PRE_LOGIN packet, published by pi_trailcam
// (docs/protocol-analysis.md).
constexpr std::uint8_t kPreLoginKey[16] = {'a', '0', '1', 'b', 'c', '2',
                                           '3', 'e', 'd', '4', '5', 'f',
                                           'F', '5', '6', 'A'};
// Bytes before the ciphertext in the hub's PRE_LOGIN. Not checked: the app
// never sends a PRE_LOGIN to the camera, so nothing is known about what a
// camera would expect there (#51).
constexpr std::size_t kPreLoginHeaderSize = 28;

std::uint32_t read_u32_le(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

void append_u32_le(std::vector<std::uint8_t> &out, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

// Decrypts the PRE_LOGIN body (AES-128-ECB, PKCS#7) for the log. Returns an
// empty string if it does not decrypt to something that looks like JSON.
std::string decrypt_pre_login(const std::uint8_t *payload, std::size_t size)
{
    if (size <= kPreLoginHeaderSize ||
        (size - kPreLoginHeaderSize) % camera::artemis::Aes128::kBlockSize !=
            0) {
        return {};
    }
    const camera::artemis::Aes128 cipher(kPreLoginKey);
    std::string plain;
    for (std::size_t offset = kPreLoginHeaderSize; offset < size;
         offset += camera::artemis::Aes128::kBlockSize) {
        std::uint8_t block[camera::artemis::Aes128::kBlockSize];
        cipher.decrypt_block(payload + offset, block);
        plain.append(reinterpret_cast<const char *>(block), sizeof(block));
    }
    const std::size_t pad = static_cast<std::uint8_t>(plain.back());
    if (pad == 0 || pad > camera::artemis::Aes128::kBlockSize ||
        pad > plain.size()) {
        return {};
    }
    plain.resize(plain.size() - pad);
    if (plain.empty() || plain.front() != '{') {
        return {};
    }
    return plain;
}

const fixtures::CapturedResponse *captured_page(int page_no)
{
    switch (page_no) {
    case 0:
        return &fixtures::kMediaListPage0Response;
    case 1:
        return &fixtures::kMediaListPage1Response;
    default:
        return nullptr;
    }
}

std::string format(const char *fmt, long long a, long long b = 0,
                   long long c = 0)
{
    char text[160];
    std::snprintf(text, sizeof(text), fmt, a, b, c);
    return text;
}

}  // namespace

bool find_int(const std::string &json, const char *key, std::int64_t &value)
{
    const std::string needle = std::string("\"") + key + "\":";
    const std::size_t at = json.find(needle);
    if (at == std::string::npos) {
        return false;
    }
    std::size_t pos = at + needle.size();
    while (pos < json.size() && json[pos] == ' ') {
        pos++;
    }
    bool negative = false;
    if (pos < json.size() && json[pos] == '-') {
        negative = true;
        pos++;
    }
    if (pos >= json.size() || json[pos] < '0' || json[pos] > '9') {
        return false;
    }
    std::int64_t result = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        result = result * 10 + (json[pos] - '0');
        pos++;
    }
    value = negative ? -result : result;
    return true;
}

std::vector<std::uint8_t> artemis_response(std::uint32_t app_sequence,
                                           const std::string &base64,
                                           std::size_t padding)
{
    std::vector<std::uint8_t> out(
        kArtemisSignature, kArtemisSignature + sizeof(kArtemisSignature));
    append_u32_le(out, kMsgResponse);
    append_u32_le(out, app_sequence);
    append_u32_le(out, static_cast<std::uint32_t>(base64.size() + padding));
    out.insert(out.end(), base64.begin(), base64.end());
    out.insert(out.end(), padding, 0);
    return out;
}

std::vector<std::uint8_t>
media_record_for_file(const Identity &identity, std::int64_t dir_num,
                      std::int64_t media_num, std::int64_t media_id,
                      const std::uint8_t *file, std::size_t file_size)
{
    std::vector<std::uint8_t> header(72, 0);
    const std::string mac = mac_text(identity);
    std::memcpy(header.data(), mac.data(), 17);
    const auto put = [&header](std::size_t at, std::uint32_t value, int bytes) {
        for (int i = 0; i < bytes; i++) {
            header[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
        }
    };
    put(0x20, static_cast<std::uint32_t>(dir_num), 2);
    put(0x22, static_cast<std::uint32_t>(media_num), 2);
    put(0x24, static_cast<std::uint32_t>(file_size), 4);
    put(0x28, static_cast<std::uint32_t>(media_id), 4);

    std::vector<std::uint8_t> out(
        kArtemisSignature, kArtemisSignature + sizeof(kArtemisSignature));
    append_u32_le(out, kMsgResponse);
    append_u32_le(out, 0);
    append_u32_le(out, static_cast<std::uint32_t>(header.size() + file_size));
    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), file, file + file_size);
    return out;
}

std::vector<std::uint8_t> media_record(const Identity &identity,
                                       std::int64_t dir_num,
                                       std::int64_t media_num,
                                       std::int64_t media_id,
                                       std::size_t file_size)
{
    std::vector<std::uint8_t> file(file_size < 4 ? 4 : file_size, 0x5a);
    file[0] = 0xff;
    file[1] = 0xd8;
    file[file.size() - 2] = 0xff;
    file[file.size() - 1] = 0xd9;
    for (std::size_t i = 2; i + 2 < file.size(); i++) {
        file[i] = static_cast<std::uint8_t>((i * 31 + media_num) & 0x7f);
    }
    return media_record_for_file(identity, dir_num, media_num, media_id,
                                 file.data(), file.size());
}

namespace
{

// The entries of both captured pages as JSON objects.
std::vector<std::string> captured_entries(const camera::artemis::Aes128 &cipher)
{
    std::vector<std::string> entries;
    const fixtures::CapturedResponse *pages[] = {
        &fixtures::kMediaListPage0Response, &fixtures::kMediaListPage1Response};
    for (const fixtures::CapturedResponse *page : pages) {
        std::string json;
        if (!camera::artemis::decode(cipher, page->base64, json)) {
            continue;
        }
        std::size_t at = 0;
        while ((at = json.find('{', at + 1)) != std::string::npos) {
            const std::size_t end = json.find('}', at);
            if (end == std::string::npos) {
                break;
            }
            entries.push_back(json.substr(at, end - at + 1));
            at = end;
        }
    }
    return entries;
}

constexpr std::int64_t kFirstDir = 102;
constexpr std::int64_t kFirstNum = 436;
constexpr std::int64_t kPerDir = 300;
constexpr std::int64_t kFirstTime = 1770385736;

struct Entry {
    std::int64_t file_type = 0;
    std::int64_t dir_num = 0;
    std::int64_t media_num = 0;
    std::int64_t duration_ms = 0;
    std::int64_t media_id = 0;
    std::int64_t media_time = 0;
};

// The invented entry `index` (>= 90): continues the captured numbering.
Entry invented_entry(std::size_t index)
{
    const std::int64_t i = static_cast<std::int64_t>(index);
    Entry e;
    e.file_type = i % 2;
    e.dir_num = kFirstDir - i / kPerDir;
    e.media_num = kFirstNum - i % kPerDir;
    e.duration_ms = e.file_type == 1 ? 8000 + (i % 7) * 500 : 0;
    // Never 0 (the hub reads 0 as "not there"), spread over 32 bits.
    e.media_id = static_cast<std::int64_t>(
        (static_cast<std::uint32_t>(i) * 2654435761u) | 1u);
    e.media_time = kFirstTime - i * 300;
    return e;
}

std::string entry_json(const Entry &e)
{
    return "{\"fileType\":" + std::to_string(e.file_type) +
           ",\"mediaDirNum\":" + std::to_string(e.dir_num) +
           ",\"mediaNum\":" + std::to_string(e.media_num) +
           ",\"durationMs\":" + std::to_string(e.duration_ms) +
           ",\"mediaId\":" + std::to_string(e.media_id) +
           ",\"mediaTime\":" + std::to_string(e.media_time) + "}";
}

}  // namespace

namespace
{

// The entry of a file in the media list of `count` entries.
bool find_entry(const camera::artemis::Aes128 &cipher, std::int64_t file_type,
                std::int64_t dir_num, std::int64_t media_num, std::size_t count,
                Entry &out)
{
    const std::vector<std::string> captured = captured_entries(cipher);
    for (std::size_t i = 0; i < captured.size() && i < count; i++) {
        Entry e;
        if (find_int(captured[i], "fileType", e.file_type) &&
            e.file_type == file_type &&
            find_int(captured[i], "mediaDirNum", e.dir_num) &&
            e.dir_num == dir_num &&
            find_int(captured[i], "mediaNum", e.media_num) &&
            e.media_num == media_num &&
            find_int(captured[i], "mediaId", e.media_id)) {
            find_int(captured[i], "durationMs", e.duration_ms);
            out = e;
            return true;
        }
    }
    for (std::size_t i = captured.size(); i < count; i++) {
        const Entry e = invented_entry(i);
        if (e.file_type == file_type && e.dir_num == dir_num &&
            e.media_num == media_num) {
            out = e;
            return true;
        }
    }
    return false;
}

}  // namespace

std::int64_t captured_media_id(const camera::artemis::Aes128 &cipher,
                               std::int64_t file_type, std::int64_t dir_num,
                               std::int64_t media_num, std::size_t count)
{
    Entry e;
    return find_entry(cipher, file_type, dir_num, media_num, count, e)
               ? e.media_id
               : 0;
}

std::string media_list_page(const camera::artemis::Aes128 &cipher,
                            std::size_t count, int page_no, int per_page)
{
    const std::vector<std::string> captured = captured_entries(cipher);
    std::string items;
    int in_page = 0;
    if (page_no >= 0 && per_page > 0) {
        const std::size_t first = static_cast<std::size_t>(page_no) *
                                  static_cast<std::size_t>(per_page);
        for (std::size_t i = first; i < count && in_page < per_page; i++) {
            if (!items.empty()) {
                items += ",";
            }
            items += i < captured.size() ? captured[i]
                                         : entry_json(invented_entry(i));
            in_page++;
        }
    }
    return "{\"mediaFiles\":[" + items +
           "],\"cnt\":" + std::to_string(in_page) +
           ",\"pageNo\":" + std::to_string(page_no) +
           ",\"getMediaListRet\":0,\"errorMsg\":\"Success\",\"result\":0,"
           "\"cmdId\":768}";
}

bool Identity::operator==(const Identity &other) const
{
    return std::memcmp(dev_id, other.dev_id, sizeof(dev_id)) == 0;
}

std::string mac_text(const Identity &identity)
{
    char text[18];
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                  identity.dev_id[0], identity.dev_id[1], identity.dev_id[2],
                  identity.dev_id[3], identity.dev_id[4], identity.dev_id[5]);
    return text;
}

std::string ble_name(const Identity &identity)
{
    char text[11];
    std::snprintf(text, sizeof(text), "TC100_%02X%02X", identity.dev_id[4],
                  identity.dev_id[5]);
    return text;
}

bool parse_mac(const std::string &text, Identity &identity)
{
    if (text.size() != 17) {
        return false;
    }
    Identity parsed;
    for (int i = 0; i < 6; i++) {
        const char sep = i < 5 ? text[i * 3 + 2] : ':';
        if (sep != ':' && sep != '-') {
            return false;
        }
        unsigned value = 0;
        for (int k = 0; k < 2; k++) {
            const char c = text[i * 3 + k];
            unsigned digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<unsigned>(c - 'A' + 10);
            } else {
                return false;
            }
            value = value * 16 + digit;
        }
        parsed.dev_id[i] = static_cast<std::uint8_t>(value);
    }
    if ((parsed.dev_id[0] & 0x01) != 0) {
        return false;
    }
    identity = parsed;
    return true;
}

std::string patch_identity(const std::string &json, const Identity &identity)
{
    const std::string key = "\"devId\":\"";
    const std::size_t at = json.find(key);
    if (at == std::string::npos) {
        return json;
    }
    const std::size_t start = at + key.size();
    const std::size_t end = json.find('"', start);
    if (end == std::string::npos) {
        return json;
    }
    return json.substr(0, start) + mac_text(identity) + json.substr(end);
}

bool apply_fault_command(Faults &faults, const std::vector<std::string> &args,
                         std::string &error)
{
    const char *usage = "usage: fault off | drop <0-100> | dup <0-100> | "
                        "lateack <ms> | mute on|off | battery <0-100>|off | "
                        "sdfree <MB>|off | abort <frames>|off";
    if (args.empty()) {
        error = usage;
        return false;
    }
    const std::string &what = args[0];
    if (what == "off" && args.size() == 1) {
        faults = Faults();
        return true;
    }
    if (args.size() != 2) {
        error = usage;
        return false;
    }
    const std::string &arg = args[1];
    long long number = -1;
    bool is_number = !arg.empty();
    for (char c : arg) {
        is_number = is_number && c >= '0' && c <= '9';
    }
    if (is_number && arg.size() <= 9) {
        number = std::stoll(arg);
    }
    Faults next = faults;
    if (what == "drop" || what == "dup") {
        if (number < 0 || number > 100) {
            error = what + " takes a share in percent, 0-100";
            return false;
        }
        (what == "drop" ? next.drop_percent : next.duplicate_percent) =
            static_cast<unsigned>(number);
    } else if (what == "abort") {
        if (arg == "off") {
            next.abort_after_frames = 0;
        } else if (number < 1 || number > 10000) {
            error = "abort takes a number of frames (1-10000) or off";
            return false;
        } else {
            next.abort_after_frames = static_cast<unsigned>(number);
        }
    } else if (what == "lateack") {
        if (number < 0 || number > 60000) {
            error = "lateack takes a delay in ms, 0-60000";
            return false;
        }
        next.ack_delay_ms = number;
    } else if (what == "mute") {
        if (arg != "on" && arg != "off") {
            error = "usage: fault mute on|off";
            return false;
        }
        next.mute = arg == "on";
    } else if (what == "battery" || what == "sdfree") {
        int &field = what == "battery" ? next.battery_percent : next.sd_free_mb;
        if (arg == "off") {
            field = -1;
        } else if (number < 0 || (what == "battery" && number > 100) ||
                   number > 10000000) {
            error = what == "battery" ? "battery takes 0-100 or off"
                                      : "sdfree takes MB or off";
            return false;
        } else {
            field = static_cast<int>(number);
        }
    } else {
        error = usage;
        return false;
    }
    faults = next;
    return true;
}

std::string describe(const Faults &faults)
{
    if (!faults.any()) {
        return "none";
    }
    std::string out;
    auto add = [&out](const std::string &part) {
        out += (out.empty() ? "" : ", ") + part;
    };
    if (faults.drop_percent > 0) {
        add("drop " + std::to_string(faults.drop_percent) + " %");
    }
    if (faults.duplicate_percent > 0) {
        add("duplicate " + std::to_string(faults.duplicate_percent) + " %");
    }
    if (faults.ack_delay_ms > 0) {
        add("ACKs " + std::to_string(faults.ack_delay_ms) + " ms late");
    }
    if (faults.mute) {
        add("mute after login");
    }
    if (faults.battery_percent >= 0) {
        add("battery " + std::to_string(faults.battery_percent) + " %");
    }
    if (faults.sd_free_mb >= 0) {
        add("SD free " + std::to_string(faults.sd_free_mb) + " MB");
    }
    if (faults.abort_after_frames > 0) {
        add("bulk records stop after " +
            std::to_string(faults.abort_after_frames) + " frames");
    }
    return out;
}

namespace
{
// Replaces the number after `"key":` in `json`; unchanged if it is missing.
void replace_int(std::string &json, const char *key, int value)
{
    const std::string needle = std::string("\"") + key + "\":";
    const std::size_t at = json.find(needle);
    if (at == std::string::npos) {
        return;
    }
    const std::size_t begin = at + needle.size();
    std::size_t end = begin;
    while (end < json.size() &&
           (json[end] == '-' || (json[end] >= '0' && json[end] <= '9'))) {
        end++;
    }
    json.replace(begin, end - begin, std::to_string(value));
}
}  // namespace

std::string patch_device_info(const std::string &json, const Faults &faults)
{
    std::string out = json;
    if (faults.battery_percent >= 0) {
        replace_int(out, "batPercent", faults.battery_percent);
    }
    if (faults.sd_free_mb >= 0) {
        replace_int(out, "sdFreeMB", faults.sd_free_mb);
    }
    return out;
}

std::string action_response(int cmd_id, const char *field, int value)
{
    std::string json = "{";
    if (field != nullptr && field[0] != '\0') {
        json += std::string("\"") + field + "\":" + std::to_string(value) + ",";
    }
    json += value == 0 ? "\"errorMsg\":\"Success\"" : "\"errorMsg\":\"Failed\"";
    json += ",\"result\":0,\"cmdId\":" + std::to_string(cmd_id) + "}";
    return json;
}

std::size_t playback_frame_count(std::int64_t duration_ms)
{
    return duration_ms > 0
               ? static_cast<std::size_t>(duration_ms) / kPlaybackFrameMs
               : 100;
}

std::vector<std::uint8_t> playback_record(const camera::artemis::Aes128 &cipher,
                                          bool video, bool key_frame,
                                          std::uint32_t pts_ms,
                                          std::uint32_t session_no,
                                          std::size_t data_size)
{
    // Filler without zero bytes, so that it never forms a start code.
    std::uint32_t seed = pts_ms + 1;
    const auto filler = [&seed]() {
        seed = seed * 1103515245u + 12345u;
        return static_cast<std::uint8_t>(0x10 + (seed >> 16) % 0x70);
    };
    std::vector<std::uint8_t> data;
    const auto start_code = [&data](std::uint8_t nal) {
        const std::uint8_t code[5] = {0, 0, 0, 1, nal};
        data.insert(data.end(), code, code + sizeof(code));
    };
    const auto fill = [&data, &filler](std::size_t until) {
        while (data.size() < until) {
            data.push_back(filler());
        }
    };
    if (video) {
        if (data_size < 64) {
            data_size = 64;
        }
        if (key_frame) {
            start_code(0x67);  // SPS
            fill(5 + 19);
            start_code(0x68);  // PPS
            fill(5 + 19 + 5 + 7);
            start_code(0x65);  // IDR slice
        } else {
            start_code(0x41);  // P slice
        }
        fill(data_size);
    } else {
        if (data_size < 16) {
            data_size = 16;
        }
        // ADTS: AAC LC, 16 kHz, mono; the frame length counts the header.
        const std::uint8_t header[7] = {
            0xff,
            0xf1,
            0x60,
            static_cast<std::uint8_t>(0x40 | ((data_size >> 11) & 3)),
            static_cast<std::uint8_t>((data_size >> 3) & 0xff),
            static_cast<std::uint8_t>(((data_size & 7) << 5) | 0x1f),
            0xfc};
        data.assign(header, header + sizeof(header));
        fill(data_size);
    }

    // The app's encryption: the first 0x60 bytes of each 0x1000-byte page
    // (only when more than 0x5f remain), CBC with a zero IV.
    constexpr std::size_t kBlock = camera::artemis::Aes128::kBlockSize;
    for (std::size_t page = 0; page < data.size(); page += 0x1000) {
        if (data.size() - page < 0x60) {
            continue;
        }
        std::uint8_t previous[kBlock] = {};
        for (std::size_t block = 0; block < 0x60; block += kBlock) {
            std::uint8_t *at = data.data() + page + block;
            for (std::size_t i = 0; i < kBlock; i++) {
                at[i] ^= previous[i];
            }
            cipher.encrypt_block(at, at);
            std::memcpy(previous, at, kBlock);
        }
    }

    std::vector<std::uint8_t> header(108, 0);
    const auto put = [&header](std::size_t at, std::uint32_t value) {
        for (int i = 0; i < 4; i++) {
            header[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
        }
    };
    put(8, pts_ms);
    put(video ? 16 : 20, static_cast<std::uint32_t>(data.size()));
    if (video) {
        put(28, 1920);
        put(32, 1080);
    }
    put(48, session_no);

    std::vector<std::uint8_t> out(
        kArtemisSignature, kArtemisSignature + sizeof(kArtemisSignature));
    append_u32_le(out, 4);  // version
    append_u32_le(out, 0);
    append_u32_le(out, static_cast<std::uint32_t>(header.size() + data.size()));
    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::string empty_media_list_page(int page_no)
{
    return "{\"mediaFiles\":[],\"cnt\":0,\"pageNo\":" +
           std::to_string(page_no) +
           ",\"getMediaListRet\":0,\"errorMsg\":\"Success\",\"result\":0,"
           "\"cmdId\":768}";
}

Camera::Camera(Output &output, Logger logger, const Options &options)
    : output_(output), logger_(std::move(logger)), options_(options),
      cipher_(camera::artemis::command_cipher())
{
}

bool Camera::handle_packet(const std::uint8_t *bytes, std::size_t size,
                           std::int64_t now_ms)
{
    using Kind = wire::Packet::Kind;
    const wire::Packet packet = wire::parse(bytes, size);
    if (packet.kind == Kind::invalid) {
        return false;
    }

    if (packet.kind == Kind::discovery_request) {
        stats_.discoveries++;
        send(std::vector<std::uint8_t>(fixtures::kDiscoveryReply,
                                       fixtures::kDiscoveryReply +
                                           sizeof(fixtures::kDiscoveryReply)));
        if (state_ == State::idle ||
            now_ms - discovered_ms_ > options_.discovery_merge_ms) {
            start_session(now_ms);
        }
        last_rx_ms_ = now_ms;
        return true;
    }
    if (state_ == State::idle) {
        log(LogLevel::warn,
            format("Packet type 0x%02llx without a session; ignored",
                   packet.type));
        return true;
    }
    last_rx_ms_ = now_ms;

    switch (packet.kind) {
    case Kind::pre_login: {
        stats_.pre_logins++;
        const std::string json =
            decrypt_pre_login(packet.payload, packet.payload_size);
        if (json.empty()) {
            log(LogLevel::warn,
                "PRE_LOGIN does not decrypt with the bootstrap key");
        } else {
            log(LogLevel::info, "PRE_LOGIN " + json);
        }
        // The real camera's first DATA came without a PRE_LOGIN in the
        // app's captures; after the hub's PRE_LOGIN it is sent at once.
        if (state_ == State::discovered) {
            send_first_data(now_ms);
        }
        break;
    }
    case Kind::data:
        if (packet.channel != 0) {
            log(LogLevel::warn,
                format("DATA on channel %lld ignored", packet.channel));
            break;
        }
        handle_data(packet.payload, packet.payload_size, packet.index, now_ms);
        break;
    case Kind::ack:
        if (packet.channel != 0) {
            for (std::uint16_t seq : packet.acked) {
                for (auto it = bulk_tx_.begin(); it != bulk_tx_.end(); ++it) {
                    if (it->frame.sent && it->subtype == packet.channel &&
                        it->frame.index == seq) {
                        bulk_tx_.erase(it);
                        break;
                    }
                }
            }
            refill_playback(now_ms);
            flush_bulk(now_ms);
            break;
        }
        for (std::uint16_t index : packet.acked) {
            for (auto it = tx_.begin(); it != tx_.end(); ++it) {
                if (it->sent && it->index == index) {
                    tx_.erase(it);
                    break;
                }
            }
        }
        flush(now_ms);
        break;
    case Kind::alive:
        send(wire::control(wire::kAliveAck));
        break;
    case Kind::alive_ack:
        stats_.alive_acks++;
        break;
    case Kind::close:
        end_session("the hub closed the session (CLOSE)");
        break;
    default:
        log(LogLevel::warn,
            format("Unknown packet type 0x%02llx (%lld bytes) ignored",
                   packet.type, static_cast<long long>(size)));
        break;
    }
    return true;
}

void Camera::poll(std::int64_t now_ms)
{
    if (state_ == State::idle) {
        return;
    }
    if (now_ms - last_rx_ms_ > options_.idle_timeout_ms) {
        end_session("no packet from the hub; session dropped");
        return;
    }
    if (state_ == State::discovered &&
        now_ms - discovered_ms_ >= options_.first_data_delay_ms) {
        send_first_data(now_ms);
    }
    if (state_ != State::session) {
        return;
    }
    while (!late_acks_.empty() && late_acks_.front().second <= now_ms) {
        send(wire::ack(late_acks_.front().first));
        late_acks_.pop_front();
    }
    for (Outgoing &out : tx_) {
        if (out.sent && now_ms - out.last_sent_ms >= options_.retransmit_ms) {
            send_data(out.packet);
            out.last_sent_ms = now_ms;
            stats_.retransmissions++;
        }
    }
    flush(now_ms);
    for (BulkOutgoing &out : bulk_tx_) {
        if (out.frame.sent &&
            now_ms - out.frame.last_sent_ms >= options_.retransmit_ms) {
            send_data(out.frame.packet);
            out.frame.last_sent_ms = now_ms;
            stats_.bulk_retransmissions++;
        }
    }
    refill_playback(now_ms);
    flush_bulk(now_ms);
    if (now_ms - last_alive_ms_ >= options_.alive_interval_ms) {
        send(wire::control(wire::kAlive));
        last_alive_ms_ = now_ms;
        stats_.alive_sent++;
    }
}

void Camera::close()
{
    if (state_ == State::idle) {
        return;
    }
    send(wire::control(wire::kClose));
    end_session("closed by the mock (CLOSE sent)");
}

void Camera::start_session(std::int64_t now_ms)
{
    if (state_ != State::idle) {
        log(LogLevel::info, "New discovery; the previous session is dropped");
    }
    stats_.sessions++;
    state_ = State::discovered;
    logged_in_ = false;
    discovered_ms_ = now_ms;
    last_alive_ms_ = now_ms;
    next_tx_index_ = 0;
    next_rx_index_ = 0;
    tx_.clear();
    bulk_tx_.clear();
    playing_ = false;
    play_stream_.clear();
    late_acks_.clear();
    request_.clear();
    request_size_ = 0;
    log(LogLevel::info, "Discovery answered; session " +
                            std::to_string(stats_.sessions) + " started");
}

void Camera::end_session(const char *reason)
{
    log(LogLevel::info, std::string("Session ended: ") + reason);
    state_ = State::idle;
    logged_in_ = false;
    tx_.clear();
    bulk_tx_.clear();
    playing_ = false;
    play_stream_.clear();
    late_acks_.clear();
    request_.clear();
    request_size_ = 0;
}

void Camera::send_first_data(std::int64_t now_ms)
{
    state_ = State::session;
    last_alive_ms_ = now_ms;
    queue_data(fixtures::kFirstDataPayload, sizeof(fixtures::kFirstDataPayload),
               now_ms);
}

void Camera::handle_data(const std::uint8_t *payload, std::size_t size,
                         std::uint16_t index, std::int64_t now_ms)
{
    const std::uint16_t ahead =
        static_cast<std::uint16_t>(index - next_rx_index_);
    if (ahead >= 0x8000) {
        // Already received: the ACK was lost, so acknowledge it again.
        stats_.duplicates++;
        send(wire::ack(index));
        return;
    }
    if (ahead != 0) {
        // A gap: not acknowledged, so the hub sends it again after the
        // missing packet. The real camera's behaviour here is unknown.
        stats_.out_of_order++;
        log(LogLevel::warn, format("DATA index %lld before %lld; dropped",
                                   index, next_rx_index_));
        return;
    }
    stats_.data_received++;
    next_rx_index_ = static_cast<std::uint16_t>(index + 1);
    send_ack(index, now_ms);

    if (size >= kArtemisHeaderSize &&
        std::memcmp(payload, kArtemisSignature, sizeof(kArtemisSignature)) ==
            0) {
        if (!request_.empty()) {
            log(LogLevel::warn, "Incomplete ARTEMIS request dropped");
        }
        request_.assign(payload, payload + size);
        request_size_ = kArtemisHeaderSize + read_u32_le(payload + 16);
    } else if (!request_.empty()) {
        request_.insert(request_.end(), payload, payload + size);
    } else {
        log(LogLevel::warn,
            format("DATA index %lld is not an ARTEMIS message", index));
        return;
    }
    if (request_.size() >= request_size_) {
        std::vector<std::uint8_t> request;
        request.swap(request_);
        request_size_ = 0;
        handle_request(request.data(), request.size(), now_ms);
    }
}

void Camera::handle_request(const std::uint8_t *bytes, std::size_t size,
                            std::int64_t now_ms)
{
    camera::artemis::Message message;
    if (!camera::artemis::parse(cipher_, bytes, size, message)) {
        log(LogLevel::warn, "ARTEMIS request does not decode");
        return;
    }
    std::int64_t cmd_id = -1;
    if (!find_int(message.json, "cmdId", cmd_id)) {
        log(LogLevel::warn, "ARTEMIS request without cmdId: " + message.json);
        return;
    }
    stats_.requests[static_cast<int>(cmd_id)]++;
    if (cmd_id != 525) {
        log(LogLevel::info,
            format("Request cmdId %lld, AppSeq 0x%llx", cmd_id,
                   message.app_sequence) +
                (cmd_id == 0 ? std::string() : ": " + message.json));
    }

    if (cmd_id == 0) {
        // The login carries the camera user's password; not logged.
        logged_in_ = true;
        respond(message.app_sequence, fixtures::kLoginResponse.base64,
                fixtures::kLoginResponse.padding, now_ms);
        return;
    }
    if (cmd_id == 525) {
        return;  // heartbeat: only acknowledged, never answered
    }
    if (faults_.mute) {
        stats_.muted++;
        log(LogLevel::info, "Fault: mute; request not answered");
        return;
    }
    std::int64_t token = 0;
    if (!logged_in_ || !find_int(message.json, "token", token) ||
        token != fixtures::kCapturedToken) {
        // What a real camera does here is unknown; no answer makes the
        // mistake visible on the hub.
        log(LogLevel::warn, "Command without login or with a wrong token; "
                            "not answered");
        return;
    }

    switch (cmd_id) {
    case 512:
        if (faults_.battery_percent >= 0 || faults_.sd_free_mb >= 0 ||
            !(identity_ == Identity())) {
            std::string json;
            if (camera::artemis::decode(
                    cipher_, fixtures::kDeviceInfoResponse.base64, json)) {
                respond_json(
                    message.app_sequence,
                    patch_identity(patch_device_info(json, faults_), identity_),
                    now_ms);
                return;
            }
        }
        respond(message.app_sequence, fixtures::kDeviceInfoResponse.base64,
                fixtures::kDeviceInfoResponse.padding, now_ms);
        return;
    case 768: {
        std::int64_t per_page = 0;
        std::int64_t page_no = 0;
        if (!find_int(message.json, "itemCntPerPage", per_page) ||
            !find_int(message.json, "pageNo", page_no)) {
            // Verified on hardware: without them the camera does not answer.
            log(LogLevel::warn,
                "768 without itemCntPerPage/pageNo; not answered (as the "
                "camera)");
            return;
        }
        if (per_page != fixtures::kCapturedItemsPerPage) {
            log(LogLevel::warn,
                format("itemCntPerPage %lld; the captured pages hold %lld",
                       per_page, fixtures::kCapturedItemsPerPage));
        }
        // After a format the card is empty.
        const fixtures::CapturedResponse *page =
            formatted_ || options_.media_count != kCapturedMediaCount
                ? nullptr
                : captured_page(static_cast<int>(page_no));
        if (page != nullptr) {
            respond(message.app_sequence, page->base64, page->padding, now_ms);
        } else {
            const std::string json =
                formatted_ || options_.media_count == 0
                    ? empty_media_list_page(static_cast<int>(page_no))
                    : media_list_page(cipher_, options_.media_count,
                                      static_cast<int>(page_no),
                                      static_cast<int>(per_page));
            respond(message.app_sequence,
                    camera::artemis::encode(cipher_, json),
                    fixtures::kCapturedPadding, now_ms);
        }
        return;
    }
    case 641:
    case 643:
    case 644:
    case 773:
    case 518:
        handle_action(cmd_id, message.json, message.app_sequence, now_ms);
        return;
    case 772:
    case 1285:
        handle_media(cmd_id, message.json, message.app_sequence, now_ms);
        return;
    case 769:
    case 770:
        handle_playback(cmd_id, message.json, message.app_sequence, now_ms);
        return;
    default:
        log(LogLevel::warn,
            format("cmdId %lld is not supported by the mock yet; not "
                   "answered",
                   cmd_id));
        return;
    }
}

void Camera::handle_action(std::int64_t cmd_id, const std::string &request,
                           std::uint32_t app_sequence, std::int64_t now_ms)
{
    switch (cmd_id) {
    case 641:
        stats_.photos++;
        respond_json(app_sequence, action_response(641, "snapRet", 0), now_ms);
        return;
    case 643:
        recording_ = true;
        respond_json(app_sequence, action_response(643, "recordRet", 0),
                     now_ms);
        return;
    case 644:
        recording_ = false;
        respond_json(app_sequence, action_response(644, "", 0), now_ms);
        return;
    case 773: {
        std::int64_t file_type = 0;
        std::int64_t dir_num = 0;
        std::int64_t media_num = 0;
        if (!find_int(request, "fileType", file_type) ||
            !find_int(request, "dirNum", dir_num) ||
            !find_int(request, "mediaNum", media_num)) {
            respond_json(app_sequence, action_response(773, "delRet", -1),
                         now_ms);
            return;
        }
        const std::string key = std::to_string(file_type) + "/" +
                                std::to_string(dir_num) + "/" +
                                std::to_string(media_num);
        // A second deletion of the same file: "no such file" (-3). The
        // replayed media list still shows deleted files (it is a capture).
        for (const std::string &done : deleted_) {
            if (done == key) {
                respond_json(app_sequence, action_response(773, "delRet", -3),
                             now_ms);
                return;
            }
        }
        deleted_.push_back(key);
        stats_.deletions++;
        respond_json(app_sequence, action_response(773, "delRet", 0), now_ms);
        return;
    }
    case 518:
        formatted_ = true;
        deleted_.clear();
        stats_.formats++;
        respond_json(app_sequence, action_response(518, "formatRet", 0),
                     now_ms);
        return;
    default:
        return;
    }
}

void Camera::handle_playback(std::int64_t cmd_id, const std::string &request,
                             std::uint32_t app_sequence, std::int64_t now_ms)
{
    if (cmd_id == 770) {
        // The camera does not answer; the stream just stops.
        log(LogLevel::info, "770: playback stopped");
        stop_playback();
        return;
    }
    std::int64_t file_type = 0;
    std::int64_t dir_num = 0;
    std::int64_t media_num = 0;
    std::int64_t session_no = 0;
    if (!find_int(request, "fileType", file_type) ||
        !find_int(request, "dirNum", dir_num) ||
        !find_int(request, "mediaNum", media_num) ||
        !find_int(request, "sessionNo", session_no)) {
        log(LogLevel::warn,
            "769 without fileType/dirNum/mediaNum/sessionNo; not answered");
        return;
    }
    // A new request replaces a running stream.
    stop_playback();
    const std::string key = std::to_string(file_type) + "/" +
                            std::to_string(dir_num) + "/" +
                            std::to_string(media_num);
    Entry entry;
    bool gone = formatted_;
    for (const std::string &done : deleted_) {
        gone = gone || done == key;
    }
    // Only a recording under its list entry's own file type, and not a photo
    // (0), can be played; the camera answers startPbRet -3 otherwise (as it
    // does to the app's fileType 1 when its list says 2,
    // docs/video-download.md).
    const bool found = !gone && file_type != 0 &&
                       find_entry(cipher_, file_type, dir_num, media_num,
                                  options_.media_count, entry);
    if (!found) {
        log(LogLevel::info, "769 for " + key + ": refused (startPbRet -3)");
        respond_json(app_sequence,
                     "{\"startPbRet\":-3,\"errorMsg\":\"Failed\",\"result\":0,"
                     "\"cmdId\":769}",
                     now_ms);
        return;
    }
    play_frames_ = playback_frame_count(entry.duration_ms);
    play_frame_ = 0;
    play_bulk_frames_ = 0;
    play_stream_.clear();
    play_session_ = static_cast<std::uint32_t>(session_no);
    playing_ = true;
    stats_.playbacks++;
    respond_json(app_sequence,
                 "{\"startPbRet\":0,\"videoWidth\":1920,\"videoHeight\":1080,"
                 "\"totalFrame\":" +
                     std::to_string(play_frames_) + ",\"totalTime\":" +
                     std::to_string(play_frames_ * kPlaybackFrameMs) +
                     ",\"errorMsg\":\"Success\",\"result\":0,\"cmdId\":769}",
                 now_ms);
    refill_playback(now_ms);
}

void Camera::stop_playback()
{
    playing_ = false;
    play_stream_.clear();
    // Frames not sent yet are never sent; those in flight are not repeated.
    bulk_tx_.clear();
}

void Camera::refill_playback(std::int64_t now_ms)
{
    // Frames waiting in the bulk queue; enough to keep the window full. The
    // records are made as the queue empties, so a long video never sits in
    // RAM at once.
    constexpr std::size_t kQueued = 32;
    while ((playing_ || !play_stream_.empty()) && bulk_tx_.size() < kQueued) {
        if (play_stream_.size() < wire::kMaxDataPayload) {
            if (play_frame_ >= play_frames_) {
                // The last bytes, then the camera just stops sending.
                playing_ = false;
                if (play_stream_.empty()) {
                    break;
                }
            } else {
                const std::size_t i = play_frame_++;
                const std::uint32_t pts =
                    static_cast<std::uint32_t>(i) * kPlaybackFrameMs;
                const bool key = i % 25 == 0;
                const std::size_t video_size =
                    key ? 9000 : 1500 + (i % 5) * 400;
                const auto video = playback_record(cipher_, true, key, pts,
                                                   play_session_, video_size);
                play_stream_.insert(play_stream_.end(), video.begin(),
                                    video.end());
                stats_.playback_records++;
                if (i % 2 == 0) {
                    const auto audio = playback_record(cipher_, false, false,
                                                       pts, play_session_, 200);
                    play_stream_.insert(play_stream_.end(), audio.begin(),
                                        audio.end());
                    stats_.playback_records++;
                }
                continue;
            }
        }
        if (faults_.abort_after_frames > 0 &&
            play_bulk_frames_ >= faults_.abort_after_frames) {
            stats_.aborted_records++;
            log(LogLevel::info,
                format("Fault: abort; playback stopped after %lld frames",
                       faults_.abort_after_frames));
            playing_ = false;
            play_stream_.clear();
            break;
        }
        const std::size_t chunk = play_stream_.size() < wire::kMaxDataPayload
                                      ? play_stream_.size()
                                      : wire::kMaxDataPayload;
        BulkOutgoing out;
        out.subtype = 0x02;
        out.frame.index = next_bulk_seq_++;
        out.frame.packet =
            wire::bulk(0x02, out.frame.index, play_stream_.data(), chunk);
        bulk_tx_.push_back(std::move(out));
        play_stream_.erase(play_stream_.begin(), play_stream_.begin() + chunk);
        play_bulk_frames_++;
    }
    flush_bulk(now_ms);
}

void Camera::handle_media(std::int64_t cmd_id, const std::string &request,
                          std::uint32_t app_sequence, std::int64_t now_ms)
{
    // The answer to 772 and 1285 seen on hardware, also for a file that is
    // gone (docs/camera-media-delete.md): no data follows then.
    respond_json(app_sequence,
                 "{\"cmdRet\":0,\"result\":0,\"cmdId\":" +
                     std::to_string((long long)cmd_id) + "}",
                 now_ms);
    std::int64_t file_type = 0;
    std::int64_t dir_num = 0;
    std::int64_t media_num = 0;
    if (!find_int(request, "fileType", file_type) ||
        !find_int(request, "dirNum", dir_num) ||
        !find_int(request, "mediaNum", media_num)) {
        log(LogLevel::warn, format("%lld without fileType/dirNum/mediaNum; no "
                                   "data",
                                   cmd_id));
        return;
    }
    const std::string key = std::to_string(file_type) + "/" +
                            std::to_string(dir_num) + "/" +
                            std::to_string(media_num);
    bool gone = formatted_;
    for (const std::string &done : deleted_) {
        gone = gone || done == key;
    }
    const std::int64_t media_id = captured_media_id(
        cipher_, file_type, dir_num, media_num, options_.media_count);
    if (gone || media_id == 0) {
        log(LogLevel::info, std::to_string((long long)cmd_id) + " for " + key +
                                ": no such file; no data");
        return;
    }
    // Thumbnails are a few KB; a photo is bigger. Sizes vary with the
    // number so that records end at different frame offsets.
    const bool thumbnail = cmd_id == 772;
    const std::size_t size =
        thumbnail ? 3000 + static_cast<std::size_t>(media_num % 7) * 700
                  : 40000 + static_cast<std::size_t>(media_num % 5) * 3000;
    if (thumbnail) {
        stats_.thumbnails++;
    } else {
        stats_.downloads++;
    }
    const std::uint8_t *jpeg =
        thumbnail ? options_.thumbnail_jpeg : options_.photo_jpeg;
    const std::size_t jpeg_size =
        thumbnail ? options_.thumbnail_jpeg_size : options_.photo_jpeg_size;
    queue_bulk_record(
        thumbnail ? 0x04 : 0x03,
        jpeg != nullptr && jpeg_size > 0
            ? media_record_for_file(identity_, dir_num, media_num, media_id,
                                    jpeg, jpeg_size)
            : media_record(identity_, dir_num, media_num, media_id, size),
        now_ms);
}

void Camera::queue_bulk_record(std::uint8_t subtype,
                               const std::vector<std::uint8_t> &record,
                               std::int64_t now_ms)
{
    for (std::size_t offset = 0; offset < record.size();
         offset += wire::kMaxDataPayload) {
        const std::size_t chunk = record.size() - offset < wire::kMaxDataPayload
                                      ? record.size() - offset
                                      : wire::kMaxDataPayload;
        if (faults_.abort_after_frames > 0 &&
            offset / wire::kMaxDataPayload >= faults_.abort_after_frames) {
            stats_.aborted_records++;
            log(LogLevel::info,
                format("Fault: abort; record stopped after %lld frames",
                       faults_.abort_after_frames));
            break;
        }
        BulkOutgoing out;
        out.subtype = subtype;
        out.frame.index = next_bulk_seq_++;
        out.frame.packet =
            wire::bulk(subtype, out.frame.index, record.data() + offset, chunk);
        bulk_tx_.push_back(std::move(out));
    }
    flush_bulk(now_ms);
}

void Camera::flush_bulk(std::int64_t now_ms)
{
    std::size_t in_flight = 0;
    for (BulkOutgoing &out : bulk_tx_) {
        if (in_flight >= options_.window) {
            break;
        }
        if (!out.frame.sent) {
            send_data(out.frame.packet);
            out.frame.sent = true;
            out.frame.last_sent_ms = now_ms;
            stats_.bulk_frames_sent++;
        }
        in_flight++;
    }
}

void Camera::respond_json(std::uint32_t app_sequence, const std::string &json,
                          std::int64_t now_ms)
{
    respond(app_sequence, camera::artemis::encode(cipher_, json),
            fixtures::kCapturedPadding, now_ms);
}

void Camera::respond(std::uint32_t app_sequence, const std::string &base64,
                     std::size_t padding, std::int64_t now_ms)
{
    const std::vector<std::uint8_t> message =
        artemis_response(app_sequence, base64, padding);
    stats_.responses++;
    // Split into DATA packets of at most 1024 payload bytes with consecutive
    // indices; only the first carries the ARTEMIS header.
    for (std::size_t offset = 0; offset < message.size();
         offset += wire::kMaxDataPayload) {
        const std::size_t chunk =
            message.size() - offset < wire::kMaxDataPayload
                ? message.size() - offset
                : wire::kMaxDataPayload;
        queue_data(message.data() + offset, chunk, now_ms);
    }
}

void Camera::queue_data(const std::uint8_t *payload, std::size_t size,
                        std::int64_t now_ms)
{
    Outgoing out;
    out.index = next_tx_index_++;
    out.packet = wire::data(out.index, payload, size);
    tx_.push_back(std::move(out));
    flush(now_ms);
}

void Camera::flush(std::int64_t now_ms)
{
    std::size_t in_flight = 0;
    for (Outgoing &out : tx_) {
        if (in_flight >= options_.window) {
            break;
        }
        if (!out.sent) {
            send_data(out.packet);
            out.sent = true;
            out.last_sent_ms = now_ms;
            stats_.data_sent++;
        }
        in_flight++;
    }
}

void Camera::send_data(const std::vector<std::uint8_t> &packet)
{
    if (faults_.drop_percent > 0 && roll() < faults_.drop_percent) {
        stats_.dropped++;
        return;
    }
    send(packet);
    if (faults_.duplicate_percent > 0 && roll() < faults_.duplicate_percent) {
        stats_.duplicated++;
        send(packet);
    }
}

void Camera::send_ack(std::uint16_t index, std::int64_t now_ms)
{
    if (faults_.ack_delay_ms > 0) {
        late_acks_.emplace_back(index, now_ms + faults_.ack_delay_ms);
        return;
    }
    send(wire::ack(index));
}

unsigned Camera::roll()
{
    // xorshift32
    random_ ^= random_ << 13;
    random_ ^= random_ >> 17;
    random_ ^= random_ << 5;
    return random_ % 100;
}

void Camera::send(const std::vector<std::uint8_t> &packet)
{
    output_.send(packet.data(), packet.size());
}

void Camera::log(LogLevel level, const std::string &text)
{
    if (logger_) {
        logger_(level, text);
    }
}

}  // namespace camera_mock
