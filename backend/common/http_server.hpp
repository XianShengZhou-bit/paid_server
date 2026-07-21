#pragma once

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <functional>
#include <string>
#include <utility>

#include "logger.hpp"
#include "thread_pool.hpp"

namespace http_server {

struct Options {
    uint16_t port;
    int listen_backlog;
    int worker_threads;
    int max_concurrent_connections;
    int read_timeout_seconds;
};

using ClientHandler = std::function<void(int client_fd)>;

class Server {
  public:
    // review
    explicit Server(Options options) : options_(std::move(options)) {}

    // review
    Server(const Server&) = delete;
    // review
    Server& operator=(const Server&) = delete;

    // review
    void run(const ClientHandler& handler, std::atomic<bool>* running = nullptr) {
        const int server_fd = createListenSocket();
        const int epoll_fd = ::epoll_create1(0);
        if (epoll_fd < 0) {
            ::close(server_fd);
            throw std::runtime_error(std::string("epoll_create1 失败: ") + std::strerror(errno));
        }

        epoll_event listen_event{};
        listen_event.events = EPOLLIN;
        listen_event.data.fd = server_fd;
        if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &listen_event) < 0) {
            ::close(epoll_fd);
            ::close(server_fd);
            throw std::runtime_error(std::string("epoll_ctl 失败: ") + std::strerror(errno));
        }

        LOG_INFO("HTTP 服务启动: 0.0.0.0:{}, workers={}, backlog={}, max_conn={}", options_.port,
                 options_.worker_threads, options_.listen_backlog, options_.max_concurrent_connections);

        epoll_event events[64];
        while (running == nullptr || running->load()) {
            const int ready = ::epoll_wait(epoll_fd, events, 64, 500);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                LOG_ERROR("epoll_wait 失败: {}", std::strerror(errno));
                break;
            }

            for (int i = 0; i < ready; ++i) {
                if (events[i].data.fd != server_fd) {
                    continue;
                }

                while (running == nullptr || running->load()) {
                    const int client_fd = ::accept(server_fd, nullptr, nullptr);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        if (errno == EINTR) {
                            continue;
                        }
                        LOG_ERROR("accept 失败: {}", std::strerror(errno));
                        break;
                    }

                    if (!tryAcquireConnectionSlot()) {
                        LOG_WARN("HTTP 并发连接已达上限 {}, 拒绝新连接", options_.max_concurrent_connections);
                        ::close(client_fd);
                        continue;
                    }

                    setReadTimeout(client_fd);

                    payment_http::WorkerPool::instance().submit([this, client_fd, handler]() {
                        try {
                            handler(client_fd);
                        } catch (const std::exception& ex) {
                            LOG_ERROR("HTTP 连接处理异常: error={}", ex.what());
                        } catch (...) {
                            LOG_ERROR("HTTP 连接处理未知异常");
                        }
                        ::close(client_fd);
                        releaseConnectionSlot();
                    });
                }
            }
        }

        ::close(epoll_fd);
        ::close(server_fd);
    }

  private:
    // review
    int createListenSocket() {
        const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            throw std::runtime_error("创建 socket 失败");
        }

        int reuse = 1;
        ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        const int flags = ::fcntl(server_fd, F_GETFL, 0);
        if (flags >= 0) {
            ::fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(options_.port);
        address.sin_addr.s_addr = INADDR_ANY;

        if (::bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            ::close(server_fd);
            throw std::runtime_error(std::string("bind 失败: ") + std::strerror(errno));
        }
        if (::listen(server_fd, options_.listen_backlog) < 0) {
            ::close(server_fd);
            throw std::runtime_error("listen 失败");
        }
        return server_fd;
    }

    // review
    void setReadTimeout(int client_fd) const {
        if (options_.read_timeout_seconds <= 0) {
            LOG_WARN("read_timeout_seconds 小于等于 0, 不设置读取超时时间");
            return;
        }
        timeval timeout{};
        timeout.tv_sec = options_.read_timeout_seconds;
        timeout.tv_usec = 0;
        ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }

    // review
    bool tryAcquireConnectionSlot() {
        const int current = active_connections_.fetch_add(1) + 1;
        if (current > options_.max_concurrent_connections) {
            releaseConnectionSlot();
            return false;
        }
        return true;
    }

    // review
    void releaseConnectionSlot() {
        active_connections_.fetch_sub(1);
    }

    Options options_;
    std::atomic<int> active_connections_{0};
};

} // namespace http_server
