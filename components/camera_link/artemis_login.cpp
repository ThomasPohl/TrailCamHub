#include "artemis_login.hpp"

#include <string>
#include <vector>

#include "artemis_message.hpp"

namespace camera::artemis
{

LoginSession::LoginSession(pppp::Session &session, const Aes128 &cipher)
    : session_(session), cipher_(cipher)
{
}

bool LoginSession::send_json(std::uint32_t app_sequence,
                             const std::string &json, std::int64_t now_us,
                             const char *context)
{
    const std::vector<std::uint8_t> msg =
        build_request(cipher_, app_sequence, json);
    if (msg.empty())
        return false;
    return session_.send_data(msg.data(), msg.size(), now_us, context);
}

std::string build_login_json(std::int64_t camera_time)
{
    std::string json =
        "{\"cmdId\":0,\"usrName\":\"admin\",\"password\":\"admin\","
        "\"needVideo\":0,\"needAudio\":0,";
    if (camera_time > 0) {
        json += "\"utcTime\":" + std::to_string(camera_time) + ",";
    }
    return json + "\"supportHeartBeat\":true}";
}

bool LoginSession::start(std::uint32_t utc_time, std::int64_t now_us)
{
    if (state_ != State::idle)
        return false;
    const std::string json = build_login_json(utc_time);
    if (!send_json(next_app_seq_, json, now_us, "ARTEMIS login"))
        return false;
    ++next_app_seq_;
    state_ = State::logging_in;
    return true;
}

void LoginSession::on_packet(const std::uint8_t *bytes, std::size_t size,
                             std::int64_t now_us)
{
    (void)now_us;
    const pppp::Session::Event event = session_.handle_packet(bytes, size);
    using Type = pppp::Session::Event::Type;
    if (event.type == Type::close) {
        closed_ = true;
        return;
    }
    if (event.type == Type::data) {
        handle_message(event.payload, event.payload_size);
    }
}

void LoginSession::handle_message(const std::uint8_t *payload, std::size_t size)
{
    const Reassembler::Status status = reassembler_.feed(payload, size);
    if (status == Reassembler::Status::incomplete)
        return;
    if (status == Reassembler::Status::invalid) {
        reassembler_.reset();
        return;
    }

    Message msg;
    const bool ok = parse(cipher_, reassembler_.message().data(),
                          reassembler_.message().size(), msg);
    reassembler_.reset();
    if (!ok)
        return;
    if (msg.msg_type != kMsgResponse)
        return;

    std::int64_t cmd_id = 0;
    if (!json_find_int(msg.json, "cmdId", cmd_id))
        return;

    has_response_ = true;
    last_response_cmd_ = cmd_id;
    last_response_json_ = msg.json;

    if (cmd_id == 0 && state_ == State::logging_in) {
        std::int64_t result = 0;
        std::int64_t token = 0;
        if (json_find_int(msg.json, "result", result) && result == 0 &&
            json_find_int(msg.json, "token", token)) {
            token_ = token;
            state_ = State::logged_in;
        } else {
            state_ = State::failed;
        }
    }
}

std::size_t LoginSession::poll(std::int64_t now_us)
{
    return session_.poll(now_us);
}

bool LoginSession::send_command(std::uint32_t cmd_id, std::int64_t now_us)
{
    if (state_ != State::logged_in)
        return false;
    const std::string json = "{\"cmdId\":" + std::to_string(cmd_id) +
                             ",\"token\":" + std::to_string(token_) + "}";
    if (!send_json(next_app_seq_, json, now_us, "ARTEMIS command"))
        return false;
    ++next_app_seq_;
    return true;
}

}  // namespace camera::artemis
