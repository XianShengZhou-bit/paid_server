// review
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace gateway_ws {

// review
inline std::string trim(const std::string& input) {
    const std::size_t start = input.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    const std::size_t end = input.find_last_not_of(" \t\r\n");
    return input.substr(start, end - start + 1);
}

// review
inline std::string toLower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

// review
// 从raw_headers中获取name的值
inline std::string headerValue(const std::string& raw_headers, const std::string& name) {
    const std::string key = toLower(name);
    std::istringstream stream(raw_headers);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        if (toLower(trim(line.substr(0, colon))) == key) {
            return trim(line.substr(colon + 1));
        }
    }
    return "";
}

// review
// 从query中获取key的值
inline std::string queryParam(const std::string& query, const std::string& key) {
    std::size_t pos = 0;
    while (pos < query.size()) {
        const std::size_t amp = query.find('&', pos);
        const std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key) {
            return pair.substr(eq + 1);
        }
        if (amp == std::string::npos) {
            break;
        }
        pos = amp + 1;
    }
    return "";
}

// review
// 将data指针指向的内存中的len字节数据进行base64编码
inline std::string base64Encode(const unsigned char* data, std::size_t len) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        const unsigned int b0 = data[i];
        const unsigned int b1 = (i + 1 < len) ? data[i + 1] : 0;
        const unsigned int b2 = (i + 2 < len) ? data[i + 2] : 0;
        const unsigned int triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(table[(triple >> 18) & 0x3F]);
        out.push_back(table[(triple >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? table[(triple >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? table[triple & 0x3F] : '=');
    }
    return out;
}

// review
inline std::string makeAcceptKey(const std::string& client_key) {
    const std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const std::string input = client_key + magic;
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
    return base64Encode(digest, SHA_DIGEST_LENGTH);
}

// review
// 发送文本帧到客户端
inline bool sendTextFrame(int fd, const std::string& payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    if (payload.size() < 126) {
        frame.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 0xFFFF) {
        frame.push_back(126);
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        frame.push_back(static_cast<char>(payload.size() & 0xFF));
    } else {
        return false;
    }
    frame += payload;
    return ::write(fd, frame.data(), frame.size()) == static_cast<ssize_t>(frame.size());
}

// review
// 将http请求升级为websocket请求
inline bool performHandshake(int client_fd, const std::string& raw_request, std::string& payment_session_id) {
    const std::size_t line_end = raw_request.find("\r\n");
    if (line_end == std::string::npos) {
        return false;
    }
    const std::string request_line = raw_request.substr(0, line_end);
    std::istringstream line_stream(request_line);
    std::string method;
    std::string target;
    line_stream >> method >> target;
    if (method != "GET" || target.empty()) {
        return false;
    }

    std::string path;
    std::string query;
    const std::size_t query_pos = target.find('?');
    if (query_pos == std::string::npos) {
        path = target;
    } else {
        path = target.substr(0, query_pos);
        query = target.substr(query_pos + 1);
    }
    if (path != "/ws/payment") {
        return false;
    }

    payment_session_id = queryParam(query, "payment_session_id");
    if (payment_session_id.size() < 3 || payment_session_id.rfind("PS", 0) != 0) {
        return false;
    }

    const std::string ws_key = headerValue(raw_request.substr(line_end + 2), "Sec-WebSocket-Key");
    if (ws_key.empty()) {
        return false;
    }

    std::ostringstream oss;
    oss << "HTTP/1.1 101 Switching Protocols\r\n";
    oss << "Upgrade: websocket\r\n";
    oss << "Connection: Upgrade\r\n";
    oss << "Sec-WebSocket-Accept: " << makeAcceptKey(ws_key) << "\r\n\r\n";
    const std::string response = oss.str();
    return ::write(client_fd, response.data(), response.size()) == static_cast<ssize_t>(response.size());
}

class WsRegistry {
  public:
    // review
    static WsRegistry& instance() {
        static WsRegistry registry;
        return registry;
    }

    // review
    // 绑定payment_session_id和client_fd，如果已经存在，则关闭旧的连接
    void bind(const std::string& payment_session_id, int client_fd) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = session_to_fd_.find(payment_session_id);
        if (it != session_to_fd_.end() && it->second != client_fd) {
            ::close(it->second);
        }
        fd_to_session_[client_fd] = payment_session_id;
        session_to_fd_[payment_session_id] = client_fd;
    }

    // review
    // 解绑client_fd和payment_session_id
    void unbind(int client_fd) {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = fd_to_session_.find(client_fd);
        if (it == fd_to_session_.end()) {
            return;
        }
        const std::string session_id = it->second;
        fd_to_session_.erase(it);
        const auto session_it = session_to_fd_.find(session_id);
        if (session_it != session_to_fd_.end() && session_it->second == client_fd) {
            session_to_fd_.erase(session_it);
        }
    }

    // review
    // 推送消息到指定的payment_session_id
    bool push(const std::string& payment_session_id, const std::string& message) {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = session_to_fd_.find(payment_session_id);
        if (it == session_to_fd_.end()) {
            return false;
        }
        return sendTextFrame(it->second, message);
    }

    // review
    std::size_t connectionCount() {
        std::lock_guard<std::mutex> lock(mu_);
        return session_to_fd_.size();
    }

  private:
    WsRegistry() = default;

    std::mutex mu_;
    std::unordered_map<std::string, int> session_to_fd_;
    std::unordered_map<int, std::string> fd_to_session_;
}; // 可以优化为redis上(好处是可以设置生命周期，清理僵尸连接)，但是需要写权限，TODO

} // namespace gateway_ws
