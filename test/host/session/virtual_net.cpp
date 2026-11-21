#include "virtual_net.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <deque>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <vector>

#include "camera_udp_transport.h"

// The linker redirects these calls of the camera session (--wrap) to the
// functions below; other file descriptors go to the real ones.
extern "C" {
int __real_close(int fd);
int __wrap_socket(int domain, int type, int protocol);
int __wrap_bind(int fd, const struct sockaddr *address, socklen_t length);
int __wrap_setsockopt(int fd, int level, int name, const void *value,
                      socklen_t length);
int __wrap_close(int fd);
}

namespace virtual_net
{

namespace
{

// Virtual descriptors start here, above any real one of a test.
constexpr int kFirstFd = 100000;
// A receive without a timeout waits at most this long in virtual time.
constexpr std::int64_t kWaitForeverUs = 3600LL * 1000 * 1000;
// How often the camera's timers run while the session waits; the real mock
// polls every 5 ms.
constexpr std::int64_t kTickUs = 5000;
// Time a datagram from the camera takes on the air: ~1.4 MB/s, about what
// the camera's Wi-Fi delivers. Without it the link would have no bandwidth
// limit and a session would do far more work per second of virtual time than
// on the real camera.
constexpr std::int64_t kWireUs = 1000;

struct Socket {
    bool open = false;
    std::uint16_t port = 0;  // network order
    std::int64_t timeout_us = 0;
    std::deque<std::vector<std::uint8_t>> inbox;
};

struct Timer {
    int id;
    std::int64_t due_us;
    std::function<void()> action;
    bool active;
};

constexpr std::int64_t kStartUs = 10LL * 1000 * 1000;
std::int64_t s_now_us = kStartUs;
std::vector<Socket> s_sockets;
std::vector<Timer> s_timers;
int s_next_timer = 1;
Server *s_server = nullptr;

Socket *socket_of(int fd)
{
    const int index = fd - kFirstFd;
    if (index < 0 || index >= static_cast<int>(s_sockets.size()) ||
        !s_sockets[static_cast<std::size_t>(index)].open) {
        return nullptr;
    }
    return &s_sockets[static_cast<std::size_t>(index)];
}

bool is_virtual(int fd)
{
    return fd >= kFirstFd;
}

void run_due_timers()
{
    // An action may add timers, so go by index.
    for (std::size_t i = 0; i < s_timers.size(); ++i) {
        if (s_timers[i].active && s_timers[i].due_us <= s_now_us) {
            s_timers[i].active = false;
            const std::function<void()> action = s_timers[i].action;
            action();
        }
    }
}

}  // namespace

std::int64_t now_us()
{
    return ++s_now_us;
}

std::int64_t peek_us()
{
    return s_now_us;
}

std::int64_t elapsed_us()
{
    return s_now_us - kStartUs;
}

void advance_us(std::int64_t us)
{
    s_now_us += us;
    run_due_timers();
    if (s_server != nullptr) {
        s_server->on_tick(s_now_us / 1000);
    }
}

void attach_server(Server *server)
{
    s_server = server;
}

void detach_server(Server *server)
{
    if (s_server == server) {
        s_server = nullptr;
    }
}

void deliver_to_client(const std::uint8_t *data, std::size_t size)
{
    for (Socket &socket : s_sockets) {
        if (socket.open && socket.port != 0) {
            socket.inbox.emplace_back(data, data + size);
            s_now_us += kWireUs;
            return;
        }
    }
    // No socket bound: the datagram is lost, as on a real network.
}

int add_timer(std::int64_t after_ms, std::function<void()> action)
{
    const int id = s_next_timer++;
    Timer timer{id, s_now_us + after_ms * 1000, std::move(action), true};
    s_timers.push_back(std::move(timer));
    return id;
}

void cancel_timer(int id)
{
    for (Timer &timer : s_timers) {
        if (timer.id == id) {
            timer.active = false;
        }
    }
}

void reset()
{
    s_sockets.clear();
    s_timers.clear();
    s_now_us = kStartUs;
}

}  // namespace virtual_net

extern "C" {

int __wrap_socket(int domain, int type, int protocol)
{
    (void)protocol;
    if (domain != AF_INET || type != SOCK_DGRAM) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    virtual_net::s_sockets.emplace_back();
    virtual_net::s_sockets.back().open = true;
    return virtual_net::kFirstFd +
           static_cast<int>(virtual_net::s_sockets.size()) - 1;
}

int __wrap_bind(int fd, const struct sockaddr *address, socklen_t length)
{
    virtual_net::Socket *socket = virtual_net::socket_of(fd);
    if (socket == nullptr || length < sizeof(sockaddr_in)) {
        errno = EBADF;
        return -1;
    }
    sockaddr_in local = {};
    std::memcpy(&local, address, sizeof(local));
    socket->port = local.sin_port;
    return 0;
}

int __wrap_setsockopt(int fd, int level, int name, const void *value,
                      socklen_t length)
{
    virtual_net::Socket *socket = virtual_net::socket_of(fd);
    if (socket == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (level == SOL_SOCKET && name == SO_RCVTIMEO &&
        length >= sizeof(timeval)) {
        timeval timeout = {};
        std::memcpy(&timeout, value, sizeof(timeout));
        const std::int64_t seconds = timeout.tv_sec;
        const std::int64_t micros = timeout.tv_usec;
        socket->timeout_us = seconds * 1000000 + micros;
    }
    return 0;
}

int __wrap_close(int fd)
{
    if (!virtual_net::is_virtual(fd)) {
        return __real_close(fd);
    }
    virtual_net::Socket *socket = virtual_net::socket_of(fd);
    if (socket == nullptr) {
        errno = EBADF;
        return -1;
    }
    socket->open = false;
    socket->inbox.clear();
    return 0;
}

}  // extern "C"

// The packet I/O of camera_udp_transport.cpp, on the virtual sockets.
ssize_t camera_udp_send_packet(int socket_fd, const uint8_t *packet,
                               size_t packet_len,
                               const struct sockaddr_in *destination,
                               const char *context)
{
    (void)context;
    virtual_net::Socket *socket = virtual_net::socket_of(socket_fd);
    if (socket == nullptr) {
        errno = EBADF;
        return -1;
    }
    // Only the camera's port is listening; the probe to the other discovery
    // port is lost.
    if (ntohs(destination->sin_port) == virtual_net::kCameraPort &&
        virtual_net::s_server != nullptr) {
        virtual_net::s_server->on_packet(packet, packet_len,
                                         ntohs(socket->port),
                                         virtual_net::s_now_us / 1000);
        // The mock answers a packet at once and runs its timers right after
        // it, as the real server loop does.
        virtual_net::s_server->on_tick(virtual_net::s_now_us / 1000);
    }
    return static_cast<ssize_t>(packet_len);
}

ssize_t camera_udp_receive_packet(int socket_fd, uint8_t *packet,
                                  size_t packet_capacity,
                                  struct sockaddr_in *source,
                                  const char *context)
{
    (void)context;
    virtual_net::Socket *socket = virtual_net::socket_of(socket_fd);
    if (socket == nullptr) {
        errno = EBADF;
        return -1;
    }
    const std::int64_t wait_us = socket->timeout_us > 0
                                     ? socket->timeout_us
                                     : virtual_net::kWaitForeverUs;
    const std::int64_t deadline_us = virtual_net::s_now_us + wait_us;
    for (;;) {
        // The socket may have been closed by a timer.
        socket = virtual_net::socket_of(socket_fd);
        if (socket == nullptr) {
            errno = EBADF;
            return -1;
        }
        if (!socket->inbox.empty()) {
            const std::vector<std::uint8_t> datagram =
                std::move(socket->inbox.front());
            socket->inbox.pop_front();
            const std::size_t size = std::min(datagram.size(), packet_capacity);
            std::memcpy(packet, datagram.data(), size);
            *source = {};
            source->sin_family = AF_INET;
            source->sin_port = htons(virtual_net::kCameraPort);
            source->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            return static_cast<ssize_t>(size);
        }
        if (virtual_net::s_now_us >= deadline_us) {
            errno = EAGAIN;
            return -1;
        }
        virtual_net::advance_us(std::min(virtual_net::kTickUs,
                                         deadline_us - virtual_net::s_now_us));
    }
}
