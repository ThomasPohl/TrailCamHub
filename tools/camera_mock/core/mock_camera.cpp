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

bool apply_fault_command(Faults &faults, const std::vector<std::string> &args,
                         std::string &error)
{
    const char *usage = "usage: fault off | drop <0-100> | dup <0-100> | "
                        "lateack <ms> | mute on|off | battery <0-100>|off | "
                        "sdfree <MB>|off";
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
            break;  // bulk-stream ACKs: stage 2
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
        if (faults_.battery_percent >= 0 || faults_.sd_free_mb >= 0) {
            std::string json;
            if (camera::artemis::decode(
                    cipher_, fixtures::kDeviceInfoResponse.base64, json)) {
                respond_json(message.app_sequence,
                             patch_device_info(json, faults_), now_ms);
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
            formatted_ ? nullptr : captured_page(static_cast<int>(page_no));
        if (page != nullptr) {
            respond(message.app_sequence, page->base64, page->padding, now_ms);
        } else {
            respond(
                message.app_sequence,
                camera::artemis::encode(
                    cipher_, empty_media_list_page(static_cast<int>(page_no))),
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
