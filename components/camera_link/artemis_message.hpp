#pragma once

// ARTEMIS application-layer framing: a 20-byte header
// ("ARTEMIS\0" + MsgType + AppSeq + PayloadLen, little-endian) followed by a
// NUL-terminated Base64 string that carries the encrypted JSON body. This
// module ties the header to the command-channel crypto (artemis_crypto) so
// callers work with plain JSON. Pure logic, host-testable. The resulting
// bytes are carried as the payload of an RUDP DATA packet (pppp::Session).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "artemis_aes128.hpp"

namespace camera::artemis
{

constexpr std::uint32_t kMsgRequest = 2;
constexpr std::uint32_t kMsgResponse = 3;

struct Message {
    std::uint32_t msg_type = 0;      // kMsgRequest / kMsgResponse
    std::uint32_t app_sequence = 0;  // application sequence counter
    std::string json;                // decrypted JSON body
};

// Builds a request message (MsgType 2) for the given JSON command and
// application sequence: ARTEMIS header + Base64(AES-128-CBC(JSON)) + NUL.
// Returns an empty vector if the encoded body does not fit a single packet.
std::vector<std::uint8_t> build_request(const Aes128 &cipher,
                                        std::uint32_t app_sequence,
                                        const std::string &json);

// Heartbeat (cmdId 525). The TrailCam Go app numbers heartbeats in a range of
// their own: application sequence 0x00010000 + a counter that starts at 1 in
// each session and counts each new heartbeat (retransmissions keep it). Seen
// in all captures of pwnb0t/trailcam-automation (up to 0x1002e); the app
// counts the NUL in PayloadLen and leaves stale bytes after it in the padding,
// which the camera does not need (our other commands send neither).
constexpr std::uint32_t kHeartbeatSequenceBase = 0x00010000;
constexpr const char *kHeartbeatJson = "{\"cmdId\":525}";

class Heartbeat
{
  public:
    // The next heartbeat request; the counter wraps from 0xffff to 1.
    std::vector<std::uint8_t> next(const Aes128 &cipher);
    // Counter of the last heartbeat built (0 before the first).
    std::uint16_t count() const
    {
        return count_;
    }

  private:
    std::uint16_t count_ = 0;
};

// Parses an ARTEMIS message carried in an RUDP DATA payload. Returns false if
// the signature or length fields are invalid or the body cannot be decoded.
bool parse(const Aes128 &cipher, const std::uint8_t *bytes, std::size_t size,
           Message &out);

}  // namespace camera::artemis
