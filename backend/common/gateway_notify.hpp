// review
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

#include <nlohmann/json.hpp>

#include "config.hpp"
#include "logger.hpp"

namespace payment_gateway_notify {

// review
inline int connectGatewayHttp(const payment_config::PaymentGatewayConfig& gateway) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(gateway.http_port));

    std::string ip = gateway.public_host;
    if (ip == "localhost") {
        ip = "127.0.0.1";
    }
    if (::inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// review
inline bool sendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::write(fd, data.data() + sent, data.size() - sent);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// review
// 读取网关发来的http响应的状态码
inline int readHttpStatus(int fd) {
    std::string raw;
    char buffer[1024];
    while (raw.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n <= 0) {
            return -1;
        }
        raw.append(buffer, static_cast<std::size_t>(n));
        if (raw.size() > 64 * 1024) {
            return -1;
        }
    }

    const std::size_t line_end = raw.find("\r\n");
    if (line_end == std::string::npos) {
        return -1;
    }

    std::string status_line = raw.substr(0, line_end);
    const std::size_t sp1 = status_line.find(' ');
    if (sp1 == std::string::npos) {
        return -1;
    }
    const std::size_t sp2 = status_line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) {
        return -1;
    }
    try {
        return std::stoi(status_line.substr(sp1 + 1, sp2 - sp1 - 1));
    } catch (...) {
        return -1;
    }
}

// review
// 通知网关推送支付结果
inline bool notifyPaymentResult(const std::string& payment_session_id, const std::string& order_sn) {
    LOG_INFO("开始通知网关支付结果: payment_session_id={}, order_sn={}", payment_session_id, order_sn);
    const auto gateway = payment_config::Config::instance().paymentGateway();
    const int fd = connectGatewayHttp(gateway);
    if (fd < 0) {
        LOG_WARN("通知网关失败: 无法连接 {}:{}", gateway.public_host, gateway.http_port);
        return false;
    }

    const nlohmann::json body = {{"type", "PAYMENT_RESULT"},
                                 {"payment_session_id", payment_session_id},
                                 {"order_sn", order_sn},
                                 {"payment_status", "PAID"},
                                 {"message", "支付成功"}};
    const std::string payload = body.dump();

    std::string request = "POST /internal/payment/result HTTP/1.1\r\n";
    request += "Host: " + gateway.public_host + ":" + std::to_string(gateway.http_port) + "\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(payload.size()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += payload;

    bool ok = sendAll(fd, request);
    int status = -1;
    if (ok) {
        status = readHttpStatus(fd);
        ok = status == 200;
    }
    ::close(fd);

    if (!ok) {
        LOG_WARN("通知网关推送失败: payment_session_id={}, http_status={}", payment_session_id, status);
        return false;
    }

    LOG_INFO("已通知网关推送支付结果: payment_session_id={}, order_sn={}", payment_session_id, order_sn);
    return true;
}

} // namespace payment_gateway_notify
