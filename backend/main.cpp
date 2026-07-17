// review
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

#include "config.hpp"
#include "logger.hpp"
#include "mysql_pool.hpp"
#include "payment_impl.hpp"
#include "redis_pool.hpp"

namespace {

// review
std::string trim(const std::string& input) {
    const std::size_t start = input.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    const std::size_t end = input.find_last_not_of(" \t\r\n");
    return input.substr(start, end - start + 1);
}

// review
bool readHttpRequest(int client_fd, payment_impl::HttpRequest& request) {
    std::string raw;
    char buffer[4096];
    while (raw.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = ::read(client_fd, buffer, sizeof(buffer));
        if (n <= 0) {
            return false;
        }
        raw.append(buffer, static_cast<std::size_t>(n));
        if (raw.size() > 1024 * 1024) {
            return false;
        }
    } // 主要是为了获取请求头

    const std::size_t header_end = raw.find("\r\n\r\n");
    std::istringstream header_stream(raw.substr(0, header_end));
    std::string request_line;
    if (!std::getline(header_stream, request_line)) {
        return false;
    }
    if (!request_line.empty() && request_line.back() == '\r') {
        request_line.pop_back();
    }

    std::istringstream line_stream(request_line);
    std::string target;
    line_stream >> request.method >> target;
    if (request.method.empty() || target.empty()) {
        return false;
    } // 为了获取请求方法和路径

    const std::size_t query_pos = target.find('?');
    if (query_pos == std::string::npos) {
        request.path = target;
    } else {
        request.path = target.substr(0, query_pos);
        request.query = target.substr(query_pos + 1);
    } // 为了获取请求路径和对应参数

    std::string header_line;
    std::size_t content_length = 0;
    while (std::getline(header_stream, header_line)) {
        if (!header_line.empty() && header_line.back() == '\r') {
            header_line.pop_back();
        }
        if (header_line.empty()) {
            break;
        }
        const std::size_t colon = header_line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = trim(header_line.substr(0, colon));
        std::string value = trim(header_line.substr(colon + 1));
        request.headers[key] = value;
        if (key == "Content-Length") {
            content_length = static_cast<std::size_t>(std::stoul(value));
        }
    } // 为了获取请求头中的键值对

    const std::string body_prefix = raw.substr(header_end + 4);
    if (body_prefix.size() >= content_length) {
        request.body = body_prefix.substr(0, content_length);
        return true;
    }
    // 为了获取请求体
    // 如果请求体长度小于content_length，则需要继续读取
    request.body = body_prefix;
    while (request.body.size() < content_length) {
        const ssize_t n = ::read(client_fd, buffer, sizeof(buffer));
        if (n <= 0) {
            return false;
        }
        request.body.append(buffer, static_cast<std::size_t>(n));
    }
    request.body.resize(content_length);
    // 为了获取请求体

    return true;
}

// review
void writeHttpResponse(int client_fd, const payment_impl::HttpResponse& response) {
    const std::string body = response.envelope.dump();
    std::ostringstream oss;
    oss << "HTTP/1.1 " << response.http_status << " OK\r\n";
    oss << "Content-Type: application/json; charset=utf-8\r\n";
    for (const auto& header : response.extra_headers) {
        oss << header.first << ": " << header.second << "\r\n";
    }
    oss << "Content-Length: " << body.size() << "\r\n";
    oss << "Connection: close\r\n\r\n";
    oss << body;
    const std::string payload = oss.str();
    ::write(client_fd, payload.data(), payload.size());
}

// review
std::string resolveEnvPath(int argc, char** argv) {
    if (argc < 2) {
        throw std::runtime_error("未指定环境变量文件路径");
    }
    const std::string env_path = argv[1];
    if (env_path.empty()) {
        throw std::runtime_error("环境变量文件路径为空");
    }
    return env_path;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string env_path = resolveEnvPath(argc, argv);
        auto& config = const_cast<payment_config::Config&>(payment_config::Config::instance());
        config.loadFromEnvFile(env_path);

        const auto logger_config = config.logger();

        logger::Logger::getInstance().init(true, logger_config.file, logger_config.max_file_size,
                                           logger_config.max_files, logger_config.console_enabled);
        LOG_INFO("开始启动 payment backend, env={}", env_path);
        LOG_INFO("日志系统初始化完成: file={}", logger_config.file);
        payment_mysql::MySqlPool::instance().initFromConfig();
        LOG_INFO("MySQL 连接池初始化完成");
        payment_redis::RedisPool::instance().initFromConfig();
        LOG_INFO("Redis 读写连接池初始化完成");

        const auto backend = config.paymentBackend();
        const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            throw std::runtime_error("创建 socket 失败");
        }

        int reuse = 1;
        ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(backend.http_port));
        // PAYMENT_BACKEND_IP 是网关/客户端访问地址，不是 bind 地址；监听固定 0.0.0.0
        address.sin_addr.s_addr = INADDR_ANY;

        if (::bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            throw std::runtime_error(std::string("bind 失败: ") + std::strerror(errno));
        }
        if (::listen(server_fd, 16) < 0) {
            throw std::runtime_error("listen 失败");
        }

        LOG_INFO("payment backend 启动: listen=0.0.0.0:{}, public={}:{}", backend.http_port, backend.ip,
                 backend.http_port);

        while (true) {
            const int client_fd = ::accept(server_fd, nullptr, nullptr);
            if (client_fd < 0) {
                LOG_ERROR("accept 失败: {}", std::strerror(errno));
                continue;
            }

            // 后续引入epoll + 线程池提高并发量
            payment_impl::HttpRequest request;
            if (readHttpRequest(client_fd, request)) {
                LOG_INFO("收到请求: {}", request.path);
                const payment_impl::HttpResponse response = payment_impl::handleRequest(request);
                LOG_INFO("响应请求: {}", response.http_status);
                writeHttpResponse(client_fd, response);
            } else {
                LOG_WARN("读取 HTTP 请求失败");
            }
            ::close(client_fd);
        }
    } catch (const std::exception& ex) {
        // logger 可能在 init 之前就失败，此处不能用 LOG_*
        std::cerr << "启动失败: " << ex.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "启动失败: 未知异常" << std::endl;
        return 2;
    }

    return 0;
}
