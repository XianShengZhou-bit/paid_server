// review
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "http_server.hpp"
#include "test_http_support.hpp"
#include "thread_pool.hpp"

/*
测试：gateway http_server::Server + WorkerPool 协作
1. accept 后在线程池里执行 handler
2. running=false 时 run 能退出
*/

namespace {

// review
http_server::Options makeOptions(uint16_t port) {
    http_server::Options options;
    options.port = port;
    options.listen_backlog = 16;
    options.worker_threads = 4;
    options.max_concurrent_connections = 8;
    options.read_timeout_seconds = 5;
    return options;
}

class RunningServer {
  public:
    // review
    RunningServer(uint16_t port, http_server::ClientHandler handler) : running_(true) {
        thread_ = std::thread([this, port, handler = std::move(handler)]() mutable {
            http_server::Server server(makeOptions(port));
            server.run(handler, &running_);
        });
    }

    // review
    ~RunningServer() {
        running_.store(false);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // review
    RunningServer(const RunningServer&) = delete;
    // review
    RunningServer& operator=(const RunningServer&) = delete;

  private:
    std::atomic<bool> running_{false};
    std::thread thread_;
};

// review
std::string requestWithRetry(uint16_t port, const std::string& request) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            return gateway_http_test::httpRoundTrip(port, request);
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    return gateway_http_test::httpRoundTrip(port, request);
}

} // namespace

// review
TEST(GatewayHttpServerTest, HandlerReceivesClientIpInWorkerPool) {
    gateway_http::WorkerPool::instance().init(4);

    const uint16_t port = gateway_http_test::pickEphemeralPort();
    std::string observed_ip;

    RunningServer server(port, [&](int client_fd, const std::string& client_ip) {
        observed_ip = client_ip;
        const std::string body = "OK";
        const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                                     "\r\nConnection: close\r\n\r\n" + body;
        ::write(client_fd, response.data(), response.size());
    });

    const std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    const std::string response = requestWithRetry(port, request);
    EXPECT_NE(response.find("200 OK"), std::string::npos);

    EXPECT_FALSE(observed_ip.empty());
    EXPECT_TRUE(observed_ip == "127.0.0.1" || observed_ip == "::ffff:127.0.0.1");
}
