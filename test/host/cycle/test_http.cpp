// The HTTP layer of the uplink (http_util) through the host esp_http_client
// (#446): what the firmware sends, how it reads answers, and the data usage
// it counts.
#include <doctest/doctest.h>

#include <algorithm>

#include "data_usage_store.hpp"
#include "host_cycle.hpp"
#include "host_http.hpp"
#include "http_util.hpp"

namespace
{

struct Backend {
    Backend()
    {
        host_cycle::power_on(1780308000);
        host_http::clear_requests();
    }
    ~Backend()
    {
        host_http::set_handler(nullptr);
    }
};

}  // namespace

TEST_CASE("http: a POST reaches the handler with headers and body")
{
    Backend backend;
    host_http::set_handler([](const host_http::Request &request) {
        host_http::Response response;
        response.status =
            request.header("content-type") == "application/json" ? 200 : 415;
        response.body =
            "{\"ok\":true,\"got\":" + std::to_string(request.body.size()) + "}";
        return response;
    });
    uplink::Request request;
    request.url = "http://backend.test/v1/checkin";
    request.method = HTTP_METHOD_POST;
    request.headers = {{"Content-Type", "application/json"},
                       {"Authorization", "Bearer key"}};
    const std::string body(40000, 'x');  // more than one 16 KB chunk
    request.body_size = body.size();
    request.body = uplink::string_body(body);

    const uplink::HttpResult result = uplink::send(request);
    CHECK(result.error == ESP_OK);
    CHECK(result.status == 200);
    CHECK(result.body == "{\"ok\":true,\"got\":40000}");
    REQUIRE(host_http::requests().size() == 1);
    const host_http::Request &sent = host_http::requests()[0];
    CHECK(sent.method == "POST");
    CHECK(sent.url == "http://backend.test/v1/checkin");
    CHECK(sent.header("authorization") == "Bearer key");
    CHECK(sent.body == body);
}

TEST_CASE("http: answers without length, errors and Basic authentication")
{
    Backend backend;
    host_http::set_handler([](const host_http::Request &request) {
        host_http::Response response;
        if (request.url.find("/big") != std::string::npos) {
            response.body.assign(70 * 1024, 'b');
        } else if (request.url.find("/missing") != std::string::npos) {
            response.status = 404;
            response.body = "not found";
        } else {
            response.no_length = true;
            response.body.assign(9000, 'c');
        }
        return response;
    });

    uplink::Request request;
    request.url = "http://backend.test/stream";
    request.user = "hub";
    request.password = "secret";
    uplink::HttpResult result = uplink::send(request);
    CHECK(result.error == ESP_OK);
    CHECK(result.body.size() == 9000);
    CHECK(host_http::requests().back().user == "hub");
    CHECK(host_http::requests().back().password == "secret");

    request.url = "http://backend.test/missing";
    result = uplink::send(request);
    CHECK(result.error == ESP_OK);
    CHECK(result.status == 404);
    CHECK(uplink::describe(result) == "HTTP 404 not found");

    // More than 64 KB is refused, not read into memory.
    request.url = "http://backend.test/big";
    CHECK(uplink::send(request).error == ESP_ERR_INVALID_SIZE);
}

TEST_CASE("http: without a network the request fails, the hub goes on")
{
    Backend backend;
    host_http::set_handler(nullptr);
    uplink::Request request;
    request.url = "http://backend.test/v1/checkin";
    CHECK(uplink::send(request).error != ESP_OK);
    CHECK(host_http::requests().empty());
}

TEST_CASE("http: the data usage counts each request and is stored in NVS")
{
    Backend backend;
    host_http::set_handler([](const host_http::Request &) {
        host_http::Response response;
        response.body = "0123456789";
        return response;
    });
    const auto wifi = [] {
        return uplink::usage::snapshot()
            .by_transport[(std::size_t)uplink::usage::Transport::wifi];
    };
    uplink::usage::set_transport(uplink::usage::Transport::wifi);
    const uplink::usage::Bytes before = wifi();
    uplink::Request request;
    request.url = "http://backend.test/v1/checkin";
    request.method = HTTP_METHOD_POST;
    request.body_size = 100;
    request.body = uplink::string_body(std::string(100, 'x'));
    REQUIRE(uplink::send(request).error == ESP_OK);
    const uplink::usage::Bytes after = wifi();
    CHECK(after.tx - before.tx == 100 + uplink::usage::kRequestOverheadTx);
    CHECK(after.rx - before.rx == 10 + uplink::usage::kRequestOverheadRx);

    uplink::usage::persist();
    const auto keys = host_cycle::nvs_keys();
    CHECK(std::find(keys.begin(), keys.end(), "trailcam_usage/totals") !=
          keys.end());
}
