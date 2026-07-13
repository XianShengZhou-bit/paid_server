// review
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <sstream>
#include <string>

#include "config.hpp"
#include "logger.hpp"

namespace gateway_forward {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status = 500;
    std::string body;
};

// review
inline int connectBackend(const payment_config::PaymentBackendConfig& backend) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(backend.http_port));

    std::string ip = backend.ip;
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
inline std::string buildTarget(const HttpRequest& request) {
    if (request.query.empty()) {
        return request.path;
    }
    return request.path + "?" + request.query;
}

// review
inline bool sendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::write(fd, data.data() + sent, data.size() - sent);
        if (n <= 0) {
            return false; // 没发完
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// review
// 这就是为了获取http响应body
inline bool readHttpResponse(int fd, HttpResponse& response) {
    std::string raw;
    char buffer[4096];
    while (raw.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n <= 0) {
            return false;
        }
        raw.append(buffer, static_cast<std::size_t>(n));
        if (raw.size() > 4 * 1024 * 1024) {
            return false;
        }
    } // 主要是为了取报头

    const std::size_t header_end = raw.find("\r\n\r\n");
    const std::string header_part = raw.substr(0, header_end);
    std::istringstream header_stream(header_part);
    std::string status_line; // status_line是状态行
    if (!std::getline(header_stream, status_line)) {
        return false;
    }
    if (!status_line.empty() && status_line.back() == '\r') {
        status_line.pop_back();
    }

    std::istringstream status_stream(status_line);
    std::string http_version;
    status_stream >> http_version >> response.status;

    std::size_t content_length = 0;
    std::string header_line;
    while (std::getline(header_stream, header_line)) {
        if (!header_line.empty() && header_line.back() == '\r') {
            header_line.pop_back();
        }
        const std::size_t colon = header_line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = header_line.substr(0, colon);
        std::string value = header_line.substr(colon + 1);
        while (!key.empty() && (key.front() == ' ' || key.back() == ' ')) {
            if (key.front() == ' ') {
                key.erase(key.begin());
            } else {
                key.pop_back();
            }
        }
        while (!value.empty() && (value.front() == ' ' || value.back() == ' ')) {
            if (value.front() == ' ') {
                value.erase(value.begin());
            } else {
                value.pop_back();
            }
        }
        if (key == "Content-Length") {
            content_length = static_cast<std::size_t>(std::stoul(value));
        }
    }

    response.body = raw.substr(header_end + 4);
    while (response.body.size() < content_length) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n <= 0) {
            return false;
        }
        response.body.append(buffer, static_cast<std::size_t>(n));
    }
    response.body.resize(content_length);
    return true;
}

inline HttpResponse forwardToBackend(const HttpRequest& request, const std::string& user_id) {
    LOG_INFO("开始转发请求到后端: method={}, path={}, query={}, user_id={}", request.method, request.path,
             request.query, user_id);
    const auto backend = payment_config::Config::instance().paymentBackend();
    const int fd = connectBackend(backend);
    if (fd < 0) {
        LOG_ERROR("连接后端失败: {}:{}", backend.ip, backend.http_port);
        return HttpResponse{502, R"({"code":50001,"message":"后端不可达","data":{}})"};
    }

    std::ostringstream oss;
    oss << request.method << " " << buildTarget(request) << " HTTP/1.1\r\n";
    oss << "Host: " << backend.ip << ":" << backend.http_port << "\r\n";
    oss << "Connection: close\r\n";
    if (!user_id.empty()) {
        oss << "X-User-Id: " << user_id << "\r\n";
    }

    const auto content_type_it = request.headers.find("Content-Type");
    if (content_type_it != request.headers.end()) {
        oss << "Content-Type: " << content_type_it->second << "\r\n";
    } else if (!request.body.empty()) {
        oss << "Content-Type: application/json\r\n"; // 兜底，用JSON解析
    }

    if (!request.body.empty()) {
        oss << "Content-Length: " << request.body.size() << "\r\n";
    }
    oss << "\r\n";
    if (!request.body.empty()) {
        oss << request.body;
    }

    HttpResponse response;
    if (!sendAll(fd, oss.str()) || !readHttpResponse(fd, response)) { // 当前是同步阻塞等待，先实现效果
        ::close(fd);
        LOG_ERROR("后端响应异常: {} {}", request.method, request.path);
        return HttpResponse{502, R"({"code":50001,"message":"后端响应异常","data":{}})"};
    }
    ::close(fd);
    LOG_INFO("转发请求到后端完成: method={}, path={}, query={}, user_id={}, status={}", request.method, request.path,
             request.query, user_id, response.status);
    return response;
}

} // namespace gateway_forward
