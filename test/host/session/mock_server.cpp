#include "mock_server.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mock_wire.hpp"

namespace host_session
{

namespace
{

constexpr std::uint16_t kCameraPort = 40611;

std::int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}

bool same_endpoint(const sockaddr_in &a, const sockaddr_in &b)
{
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

}  // namespace

class MockServer::UdpOutput final : public camera_mock::Output
{
  public:
    int fd = -1;
    sockaddr_in peer = {};
    bool has_peer = false;

    void send(const std::uint8_t *data, std::size_t size) override
    {
        if (has_peer) {
            sendto(fd, data, size, 0, reinterpret_cast<const sockaddr *>(&peer),
                   sizeof(peer));
        }
    }
};

MockServer::MockServer(const camera_mock::Options &options)
    : output_(new UdpOutput)
{
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        return;
    }
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(kCameraPort);
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd_, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0) {
        ::close(fd_);
        fd_ = -1;
        return;
    }
    timeval timeout = {};
    timeout.tv_usec = 5 * 1000;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    output_->fd = fd_;
    camera_.reset(new camera_mock::Camera(
        *output_,
        [this](camera_mock::LogLevel, const std::string &text) {
            log_ += text + "\n";
        },
        options));
    thread_ = std::thread([this] { run(); });
}

MockServer::~MockServer()
{
    stop_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void MockServer::run()
{
    std::uint8_t buffer[1600];
    while (!stop_) {
        sockaddr_in source = {};
        socklen_t source_size = sizeof(source);
        const ssize_t received =
            recvfrom(fd_, buffer, sizeof(buffer), 0,
                     reinterpret_cast<sockaddr *>(&source), &source_size);
        std::lock_guard<std::mutex> guard(lock_);
        if (received > 0) {
            const bool discovery =
                received >= 2 && buffer[0] == camera_mock::wire::kMagic &&
                buffer[1] == camera_mock::wire::kDiscoveryRequest;
            if (discovery) {
                output_->peer = source;
                output_->has_peer = true;
            }
            if (discovery ||
                (output_->has_peer && same_endpoint(output_->peer, source))) {
                const auto size = static_cast<std::size_t>(received);
                camera_->handle_packet(buffer, size, now_ms());
            }
        }
        camera_->poll(now_ms());
    }
}

void MockServer::set_faults(const camera_mock::Faults &faults)
{
    std::lock_guard<std::mutex> guard(lock_);
    camera_->set_faults(faults);
}

void MockServer::set_identity(const camera_mock::Identity &identity)
{
    std::lock_guard<std::mutex> guard(lock_);
    camera_->set_identity(identity);
}

void MockServer::close_session()
{
    std::lock_guard<std::mutex> guard(lock_);
    camera_->close();
}

camera_mock::Stats MockServer::stats()
{
    std::lock_guard<std::mutex> guard(lock_);
    return camera_->stats();
}

std::string MockServer::log()
{
    std::lock_guard<std::mutex> guard(lock_);
    return log_;
}

Later::Later(int delay_ms, std::function<void()> action)
    : thread_([delay_ms, action] {
          std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
          action();
      })
{
}

Later::~Later()
{
    join();
}

void Later::join()
{
    if (thread_.joinable()) {
        thread_.join();
    }
}

}  // namespace host_session
