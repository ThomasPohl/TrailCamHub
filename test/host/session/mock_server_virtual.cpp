#include "mock_server.hpp"

#include "mock_wire.hpp"
#include "virtual_net.hpp"

namespace host_session
{

// The camera mock on the virtual network: it is driven by the session's own
// thread, from inside its sends and its waits.
class MockServer::Link final : public virtual_net::Server,
                               public camera_mock::Output
{
  public:
    explicit Link(const camera_mock::Options &options)
        : camera_(
              *this,
              [this](camera_mock::LogLevel, const std::string &text) {
                  log_ += text + "\n";
              },
              options)
    {
        virtual_net::attach_server(this);
    }

    ~Link() override
    {
        virtual_net::detach_server(this);
    }

    // camera_mock::Output
    void send(const std::uint8_t *data, std::size_t size) override
    {
        if (has_peer_) {
            virtual_net::deliver_to_client(data, size);
        }
    }

    // virtual_net::Server
    void on_packet(const std::uint8_t *data, std::size_t size,
                   std::uint16_t client_port, std::int64_t now_ms) override
    {
        const bool discovery = size >= 2 &&
                               data[0] == camera_mock::wire::kMagic &&
                               data[1] == camera_mock::wire::kDiscoveryRequest;
        if (discovery) {
            peer_port_ = client_port;
            has_peer_ = true;
        }
        if (discovery || (has_peer_ && peer_port_ == client_port)) {
            camera_.handle_packet(data, size, now_ms);
        }
    }

    void on_tick(std::int64_t now_ms) override
    {
        camera_.poll(now_ms);
    }

    camera_mock::Camera &camera()
    {
        return camera_;
    }
    const std::string &log() const
    {
        return log_;
    }

  private:
    bool has_peer_ = false;
    std::uint16_t peer_port_ = 0;
    std::string log_;
    camera_mock::Camera camera_;
};

MockServer::MockServer(const camera_mock::Options &options)
    : link_(new Link(options))
{
}

MockServer::~MockServer() = default;

void MockServer::set_faults(const camera_mock::Faults &faults)
{
    link_->camera().set_faults(faults);
}

void MockServer::set_identity(const camera_mock::Identity &identity)
{
    link_->camera().set_identity(identity);
}

void MockServer::close_session()
{
    link_->camera().close();
}

camera_mock::Stats MockServer::stats()
{
    return link_->camera().stats();
}

std::string MockServer::log()
{
    return link_->log();
}

Later::Later(int delay_ms, std::function<void()> action)
    : timer_(virtual_net::add_timer(delay_ms, std::move(action)))
{
}

Later::~Later()
{
    join();
}

void Later::join()
{
    virtual_net::cancel_timer(timer_);
}

}  // namespace host_session
