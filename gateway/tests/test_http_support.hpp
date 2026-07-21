#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace gateway_http_test {

// review
// 返回一个可用的临时端口
inline uint16_t pickEphemeralPort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("创建测试 socket 失败");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error("bind 测试端口失败");
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        ::close(fd);
        throw std::runtime_error("getsockname 失败");
    }
    const uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

// review
// 发送 HTTP 请求并获取响应
inline std::string httpRoundTrip(uint16_t port, const std::string& request) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("创建客户端 socket 失败");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error(std::string("connect 失败: ") + std::strerror(errno));
    }

    if (::write(fd, request.data(), request.size()) < 0) {
        ::close(fd);
        throw std::runtime_error("write 请求失败");
    }

    std::string response;
    char buffer[4096];
    while (true) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!response.empty() && (errno == ECONNRESET || errno == EPIPE)) {
                break;
            }
            ::close(fd);
            throw std::runtime_error("read 响应失败");
        }
        if (n == 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return response;
}

} // namespace gateway_http_test
