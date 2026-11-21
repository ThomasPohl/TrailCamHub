#pragma once

// The camera mock (tools/camera_mock/core) on a UDP socket of the host, as
// tools/camera_mock/main/mock_udp.cpp runs it on an ESP32: the hub's camera
// session finds it at 127.0.0.1:40611 and runs against it for real, with real
// sockets and the real time. With HOST_VIRTUAL_LINK (session_virtual_tests,
// #354) the mock is attached to the virtual network of virtual_net.hpp
// instead: same interface, no thread, virtual time.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "mock_camera.hpp"

namespace host_session
{

class MockServer
{
  public:
    explicit MockServer(const camera_mock::Options &options);
    ~MockServer();
    MockServer(const MockServer &) = delete;
    MockServer &operator=(const MockServer &) = delete;

    // False if the port could not be bound.
    bool ok() const
    {
#ifdef HOST_VIRTUAL_LINK
        return true;
#else
        return fd_ >= 0;
#endif
    }

    void set_faults(const camera_mock::Faults &faults);
    void set_identity(const camera_mock::Identity &identity);
    void close_session();
    camera_mock::Stats stats();
    // Lines the mock logged.
    std::string log();

  private:
#ifdef HOST_VIRTUAL_LINK
    class Link;
    std::unique_ptr<Link> link_;
#else
    class UdpOutput;
    void run();

    int fd_ = -1;
    std::mutex lock_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::unique_ptr<UdpOutput> output_;
    std::unique_ptr<camera_mock::Camera> camera_;
    std::string log_;
#endif
};

// Runs `action` once `delay_ms` has passed while a session runs: on a thread
// in real time, or on the virtual clock. join() waits for it (real) or
// cancels it if it has not run (virtual).
class Later
{
  public:
    Later(int delay_ms, std::function<void()> action);
    ~Later();
    Later(const Later &) = delete;
    Later &operator=(const Later &) = delete;
    void join();

  private:
#ifdef HOST_VIRTUAL_LINK
    int timer_ = -1;
#else
    std::thread thread_;
#endif
};

}  // namespace host_session
