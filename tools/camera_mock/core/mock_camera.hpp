#pragma once

// The camera side of a UDP session (stage 1 of #122): discovery, PRE_LOGIN,
// the reliable DATA/ACK layer and the ARTEMIS commands login (0), device
// info (512), media list (768) and heartbeat (525); since stage 3 also the
// camera actions photo (641), video start/stop (643/644), delete (773) and
// format (518). Answers are replayed
// from captures (capture_fixtures.hpp). Only the low-level primitives are
// shared with the hub: AES-128, Base64 and the parsing of the ARTEMIS
// request header (components/camera_link/artemis_*).
//
// Pure logic without ESP-IDF, driven by handle_packet() and poll() with the
// current time, so it runs in host tests as well. Not thread-safe.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "artemis_aes128.hpp"

namespace camera_mock
{

struct Options {
    // The camera retransmits unacknowledged DATA quickly: ~70 ms in the
    // captures (the hub uses 800 ms).
    std::int64_t retransmit_ms = 100;
    // DATA packets in flight before the next one waits for an ACK. The camera
    // sent up to 7 response fragments at once in the captures.
    std::size_t window = 8;
    // Without a PRE_LOGIN the camera sends its first DATA ("ACK") on its own
    // ~0.3-0.5 s after discovery (captures of the app, #51).
    std::int64_t first_data_delay_ms = 400;
    // The camera sends ALIVE about every 2 s and answers the client's.
    std::int64_t alive_interval_ms = 2000;
    // A session without any packet from the hub for this long is dropped.
    std::int64_t idle_timeout_ms = 30000;
    // Discovery probes this close together belong to one session start (the
    // app sends two); a later one starts a new session.
    std::int64_t discovery_merge_ms = 1000;
};

// Faults the mock injects, switchable at run time (stage 2 of #122). All off
// by default. Random choices come from a fixed-seed generator, so a test
// sees the same sequence every time.
struct Faults {
    // Share of DATA packets to the hub (first sends and retransmissions)
    // that are not sent, in percent.
    unsigned drop_percent = 0;
    // Share of DATA packets sent twice, in percent.
    unsigned duplicate_percent = 0;
    // Acknowledge the hub's DATA this late (the real camera acknowledges up
    // to 1.5-2 s late while streaming thumbnails, #281).
    std::int64_t ack_delay_ms = 0;
    // Answer no ARTEMIS request after the login (as a camera that is
    // commanded before its session has settled).
    bool mute = false;
    // Values the device info (512) reports instead of the captured ones;
    // -1 = as captured (battery 100 %, 20617 MB of 29998 MB free).
    int battery_percent = -1;
    int sd_free_mb = -1;

    bool any() const
    {
        return drop_percent > 0 || duplicate_percent > 0 || ack_delay_ms > 0 ||
               mute || battery_percent >= 0 || sd_free_mb >= 0;
    }
};

// The camera's identity (#122 stage 2): its MAC ("devId" in 512, the BLE
// manufacturer data, the media header) and the BLE name derived from it.
// Default: the captured TC100. A second mock with another MAC stands for a
// second camera (#57).
struct Identity {
    std::uint8_t dev_id[6] = {0xc6, 0x1e, 0x0d, 0xe0, 0x0c, 0xfb};

    bool operator==(const Identity &other) const;
};

// "C6:1E:0D:E0:0C:FB".
std::string mac_text(const Identity &identity);
// "TC100_0CFB": as the camera, the last two bytes of the MAC.
std::string ble_name(const Identity &identity);
// Six hex bytes separated by ':' or '-'; false (and `identity` unchanged)
// otherwise. A multicast MAC (lowest bit of the first byte) is refused.
bool parse_mac(const std::string &text, Identity &identity);
// The device info with "devId" set to the identity's MAC.
std::string patch_identity(const std::string &json, const Identity &identity);

// The console command `fault …` without the word "fault": "off",
// "drop <0-100>", "dup <0-100>", "lateack <ms>", "mute on|off",
// "battery <0-100>|off", "sdfree <MB>|off". Changes `faults`; false with a
// message in `error` (and `faults` unchanged) if the command is not valid.
bool apply_fault_command(Faults &faults, const std::vector<std::string> &args,
                         std::string &error);

// One line for the console: "drop 10 %, battery 15 %" or "none".
std::string describe(const Faults &faults);

// The device info with the faults' battery and SD values put in.
std::string patch_device_info(const std::string &json, const Faults &faults);

// Outgoing UDP datagrams to the hub.
class Output
{
  public:
    virtual ~Output() = default;
    virtual void send(const std::uint8_t *data, std::size_t size) = 0;
};

enum class LogLevel {
    info,
    warn,
};
using Logger = std::function<void(LogLevel, const std::string &)>;

struct Stats {
    unsigned sessions = 0;
    unsigned discoveries = 0;
    unsigned pre_logins = 0;
    unsigned data_received = 0;
    unsigned duplicates = 0;
    unsigned out_of_order = 0;
    unsigned data_sent = 0;
    unsigned retransmissions = 0;
    unsigned alive_sent = 0;
    unsigned alive_acks = 0;
    unsigned responses = 0;
    // Injected faults (#122 stage 2).
    unsigned dropped = 0;
    unsigned duplicated = 0;
    unsigned muted = 0;
    // Camera actions answered (#122 stage 3).
    unsigned photos = 0;
    unsigned deletions = 0;
    unsigned formats = 0;
    // Bulk channel (#122 stage 2/3): thumbnails (772), photos (1285).
    unsigned thumbnails = 0;
    unsigned downloads = 0;
    unsigned bulk_frames_sent = 0;
    unsigned bulk_retransmissions = 0;
    // Requests by cmdId.
    std::map<int, unsigned> requests;
};

class Camera
{
  public:
    enum class State {
        // No session: waiting for discovery.
        idle,
        // Discovery answered; the first DATA ("ACK") is not sent yet.
        discovered,
        // First DATA sent; waiting for and answering ARTEMIS requests.
        session,
    };

    Camera(Output &output, Logger logger, const Options &options = Options());

    // A datagram from the hub. Returns false if it was not a camera packet.
    bool handle_packet(const std::uint8_t *bytes, std::size_t size,
                       std::int64_t now_ms);
    // Timers: first DATA, retransmissions, ALIVE, idle timeout.
    void poll(std::int64_t now_ms);
    // Ends the session from the camera side (CLOSE), as the camera does
    // when it turns its Wi-Fi off.
    void close();

    void set_faults(const Faults &faults)
    {
        faults_ = faults;
    }
    const Faults &faults() const
    {
        return faults_;
    }
    void set_identity(const Identity &identity)
    {
        identity_ = identity;
    }
    const Identity &identity() const
    {
        return identity_;
    }

    State state() const
    {
        return state_;
    }
    bool logged_in() const
    {
        return logged_in_;
    }
    std::size_t outstanding() const
    {
        return tx_.size();
    }
    // A video started with 643 and not stopped yet.
    bool recording() const
    {
        return recording_;
    }
    // The card was erased with 518: the media list is empty from then on.
    bool formatted() const
    {
        return formatted_;
    }
    const Stats &stats() const
    {
        return stats_;
    }

  private:
    struct Outgoing {
        std::uint16_t index = 0;
        std::vector<std::uint8_t> packet;
        bool sent = false;
        std::int64_t last_sent_ms = 0;
    };

    void start_session(std::int64_t now_ms);
    void end_session(const char *reason);
    void send_first_data(std::int64_t now_ms);
    void handle_data(const std::uint8_t *payload, std::size_t size,
                     std::uint16_t index, std::int64_t now_ms);
    void handle_request(const std::uint8_t *bytes, std::size_t size,
                        std::int64_t now_ms);
    void respond(std::uint32_t app_sequence, const std::string &base64,
                 std::size_t padding, std::int64_t now_ms);
    // Encrypts and answers a JSON the mock builds itself (no capture).
    void respond_json(std::uint32_t app_sequence, const std::string &json,
                      std::int64_t now_ms);
    void handle_action(std::int64_t cmd_id, const std::string &request,
                       std::uint32_t app_sequence, std::int64_t now_ms);
    void queue_data(const std::uint8_t *payload, std::size_t size,
                    std::int64_t now_ms);
    // 772 and 1285: the JSON answer, then the media record on the bulk
    // channel.
    void handle_media(std::int64_t cmd_id, const std::string &request,
                      std::uint32_t app_sequence, std::int64_t now_ms);
    void queue_bulk_record(std::uint8_t subtype,
                           const std::vector<std::uint8_t> &record,
                           std::int64_t now_ms);
    void flush_bulk(std::int64_t now_ms);
    void flush(std::int64_t now_ms);
    void send(const std::vector<std::uint8_t> &packet);
    // A DATA packet, through the drop and duplicate faults.
    void send_data(const std::vector<std::uint8_t> &packet);
    void send_ack(std::uint16_t index, std::int64_t now_ms);
    // 0..99 from a fixed-seed generator.
    unsigned roll();
    void log(LogLevel level, const std::string &text);

    Output &output_;
    Logger logger_;
    Options options_;
    camera::artemis::Aes128 cipher_;
    State state_ = State::idle;
    bool logged_in_ = false;
    std::int64_t discovered_ms_ = 0;
    std::int64_t last_rx_ms_ = 0;
    std::int64_t last_alive_ms_ = 0;
    std::uint16_t next_tx_index_ = 0;
    std::uint16_t next_rx_index_ = 0;
    std::deque<Outgoing> tx_;
    // Bulk frames not acknowledged yet; `index` is the frame's sequence.
    struct BulkOutgoing {
        std::uint8_t subtype = 0;
        Outgoing frame;
    };
    std::deque<BulkOutgoing> bulk_tx_;
    std::uint16_t next_bulk_seq_ = 0;
    // ARTEMIS request being reassembled from DATA payloads.
    std::vector<std::uint8_t> request_;
    std::size_t request_size_ = 0;
    Stats stats_;
    Faults faults_;
    Identity identity_;
    std::uint32_t random_ = 0x2545F491u;
    // ACKs held back by Faults::ack_delay_ms: index and when to send.
    std::deque<std::pair<std::uint16_t, std::int64_t>> late_acks_;
    bool recording_ = false;
    bool formatted_ = false;
    // Recordings deleted with 773, as "fileType/dirNum/mediaNum".
    std::vector<std::string> deleted_;
};

// The answer to a camera action (#122 stage 3). No capture holds one: the
// shape follows the other answers (result field, errorMsg, result, cmdId)
// and docs/camera-actions.md (snapRet, recordRet, delRet, formatRet; 644
// has only result). `field` empty = only result.
std::string action_response(int cmd_id, const char *field, int value);

// The bytes a bulk media record carries after its 20-byte ARTEMIS header
// (media_transfer.hpp in the hub, from a dump of a real TC100, #105): a
// 72-byte media header (camera MAC as text, dir and num as u16, data length
// and mediaId as u32, little-endian) and the file. The file is synthetic, a
// valid JPEG frame of SOI, filler and EOI; no capture holds a thumbnail.
std::vector<std::uint8_t> media_record(const Identity &identity,
                                       std::int64_t dir_num,
                                       std::int64_t media_num,
                                       std::int64_t media_id,
                                       std::size_t file_size);

// The mediaId of a file in the captured media list; 0 if it is not there.
std::int64_t captured_media_id(const camera::artemis::Aes128 &cipher,
                               std::int64_t file_type, std::int64_t dir_num,
                               std::int64_t media_num);

// Builds an ARTEMIS response (MsgType 3) the way the camera frames it:
// header, Base64, then `padding` zero bytes, all counted in PayloadLen.
std::vector<std::uint8_t> artemis_response(std::uint32_t app_sequence,
                                           const std::string &base64,
                                           std::size_t padding);

// The media-list page past the captured ones. No capture has one; the shape
// follows docs/udp-protocol.md §5.4 (an empty page with result 0 ends the
// list) and the key order of the captured pages.
std::string empty_media_list_page(int page_no);

// The integer value of `"key":` in `json`, without a JSON parser (and so
// without the hub's artemis_json). False if the key is missing.
bool find_int(const std::string &json, const char *key, std::int64_t &value);

}  // namespace camera_mock
