#pragma once

// Application-level login flow for the camera's ARTEMIS protocol, driven over
// the reliable-UDP session (pppp::Session). It sends the login command
// (cmdId 0), reads the session token from the response, and then sends
// token-authenticated commands. Pure logic (no ESP-IDF), host-testable with a
// fake transport.

#include <cstddef>
#include <cstdint>
#include <string>

#include "artemis_aes128.hpp"
#include "artemis_json.hpp"
#include "artemis_reassembler.hpp"
#include "pppp_session.hpp"

namespace camera::artemis
{

// The login request (cmdId 0). The camera sets its clock from "utcTime"
// (see camera_wall_clock() for what to put there); a `camera_time` of 0 or
// less leaves the field out, and the camera keeps the clock it has.
std::string build_login_json(std::int64_t camera_time);

class LoginSession
{
  public:
    enum class State {
        idle,        // nothing sent yet
        logging_in,  // login request sent, awaiting the response
        logged_in,   // token received; commands can be sent
        failed,      // the camera rejected the login (result != 0)
    };

    LoginSession(pppp::Session &session, const Aes128 &cipher);

    // Sends the login request (cmdId 0) as an RUDP DATA packet. `utc_time` is
    // the time to set on the camera (0: leave it out). Returns false if the
    // send fails; call only from the idle state.
    bool start(std::uint32_t utc_time, std::int64_t now_us);

    // Feeds one UDP packet received from the camera. Drives the RUDP session
    // (ACKs, ALIVE, …) and, when a complete ARTEMIS message arrives, advances
    // the login state.
    void on_packet(const std::uint8_t *bytes, std::size_t size,
                   std::int64_t now_us);

    // Retransmits RUDP DATA packets whose interval has elapsed.
    std::size_t poll(std::int64_t now_us);

    // Sends a token-authenticated command (e.g. 512, 768) once logged in.
    // Returns false if not logged in or the send fails.
    bool send_command(std::uint32_t cmd_id, std::int64_t now_us);

    // Requests the media list (cmdId 768); see send_command().
    bool request_file_list(std::int64_t now_us)
    {
        return send_command(768, now_us);
    }

    // The most recently received, fully reassembled command response.
    bool has_response() const
    {
        return has_response_;
    }
    std::int64_t last_response_cmd() const
    {
        return last_response_cmd_;
    }
    const std::string &last_response_json() const
    {
        return last_response_json_;
    }

    State state() const
    {
        return state_;
    }
    bool logged_in() const
    {
        return state_ == State::logged_in;
    }
    // The session token; valid once logged_in().
    std::int64_t token() const
    {
        return token_;
    }
    // True if the camera closed the session.
    bool closed() const
    {
        return closed_;
    }

  private:
    bool send_json(std::uint32_t app_sequence, const std::string &json,
                   std::int64_t now_us, const char *context);
    void handle_message(const std::uint8_t *payload, std::size_t size);

    pppp::Session &session_;
    const Aes128 &cipher_;
    Reassembler reassembler_;
    State state_ = State::idle;
    std::uint32_t next_app_seq_ = 1;
    std::int64_t token_ = 0;
    bool closed_ = false;
    bool has_response_ = false;
    std::int64_t last_response_cmd_ = -1;
    std::string last_response_json_;
};

}  // namespace camera::artemis
