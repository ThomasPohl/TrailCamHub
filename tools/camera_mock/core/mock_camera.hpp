#pragma once

// The camera side of a UDP session (stage 1 of #122): discovery, PRE_LOGIN,
// the reliable DATA/ACK layer and the ARTEMIS commands login (0), device
// info (512), media list (768) and heartbeat (525). Answers are replayed
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
    void queue_data(const std::uint8_t *payload, std::size_t size,
                    std::int64_t now_ms);
    void flush(std::int64_t now_ms);
    void send(const std::vector<std::uint8_t> &packet);
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
    // ARTEMIS request being reassembled from DATA payloads.
    std::vector<std::uint8_t> request_;
    std::size_t request_size_ = 0;
    Stats stats_;
};

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
