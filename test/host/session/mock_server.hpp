#pragma once

// The camera mock (tools/camera_mock/core) on a UDP socket of the host, as
// tools/camera_mock/main/mock_udp.cpp runs it on an ESP32: the hub's camera
// session finds it at 127.0.0.1:40611 and runs against it for real, with real
// sockets and the real time.

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "mock_camera.hpp"

namespace host_session
{

class MockServer
{
  public:
    explicit MockServer(const camera_mock::Options &options =
                            camera_mock::Options());
    ~MockServer();
    MockServer(const MockServer &) = delete;
    MockServer &operator=(const MockServer &) = delete;

    // False if the port could not be bound.
    bool ok() const
    {
        return fd_ >= 0;
    }

    void set_faults(const camera_mock::Faults &faults);
    void set_identity(const camera_mock::Identity &identity);
    void close_session();
    camera_mock::Stats stats();
    // Lines the mock logged.
    std::string log();

  private:
    class UdpOutput;
    void run();

    int fd_ = -1;
    std::mutex lock_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::unique_ptr<UdpOutput> output_;
    std::unique_ptr<camera_mock::Camera> camera_;
    std::string log_;
};

}  // namespace host_session
