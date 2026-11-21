#pragma once

// An in-process stand-in for the network and the clock of the camera session
// (#354). The session's UDP sockets are virtual: what it sends goes straight
// to the camera mock, what the mock sends comes back through a queue, and
// time only passes when the session waits (a receive that times out, a
// delay). Nothing sleeps and nothing runs on another thread, so a session
// takes the same steps and the same virtual time on every run.
//
// Used by session_virtual_tests only. The production code is unchanged: the
// sockets are reached through camera_udp_transport.h (replaced here) and
// socket(), bind(), setsockopt() and close() (wrapped by the linker).

#include <cstddef>
#include <cstdint>
#include <functional>

namespace virtual_net
{

// The camera side. Called from inside the session's own thread.
class Server
{
  public:
    virtual ~Server() = default;
    // A datagram that the session sent to the camera's port.
    virtual void on_packet(const std::uint8_t *data, std::size_t size,
                           std::uint16_t client_port, std::int64_t now_ms) = 0;
    // Time has passed; run the camera's timers.
    virtual void on_tick(std::int64_t now_ms) = 0;
};

// The port the camera listens on (the discovery port of the real camera).
constexpr std::uint16_t kCameraPort = 40611;

// The virtual clock in microseconds. Every read moves it on by 1 us, so a
// loop that only polls the clock still ends.
std::int64_t now_us();
// The clock without moving it.
std::int64_t peek_us();
// Virtual time since the last reset().
std::int64_t elapsed_us();
// Lets `us` microseconds pass: timers that fall due run, then the camera's
// timers.
void advance_us(std::int64_t us);

void attach_server(Server *server);
void detach_server(Server *server);
// A datagram from the camera to the session's socket.
void deliver_to_client(const std::uint8_t *data, std::size_t size);

// Runs `action` once `after_ms` of virtual time has passed. Returns an id
// for cancel_timer().
int add_timer(std::int64_t after_ms, std::function<void()> action);
void cancel_timer(int id);

// Forgets sockets and timers and sets the clock back to its start, so every
// case begins at the same virtual time (what a session does can depend on
// the absolute time, e.g. when the camera mock's timers first fall due).
void reset();

}  // namespace virtual_net
