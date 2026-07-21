// review
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "config.hpp"
#include "gateway_impl.hpp"
#include "http_server.hpp"
#include "logger.hpp"
#include "session_store.hpp"
#include "thread_pool.hpp"
#include "ws_registry.hpp"

namespace {

std::atomic<bool> g_running{true};
std::atomic<int> g_ws_active_connections{0};

// review
void onShutdownSignal(int /*signo*/) {
    g_running.store(false);
}

// review
void installShutdownHandlers() {
    struct sigaction action {};
    action.sa_handler = onShutdownSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (::sigaction(SIGINT, &action, nullptr) != 0 || ::sigaction(SIGTERM, &action, nullptr) != 0) {
        throw std::runtime_error(std::string("注册退出信号处理失败: ") + std::strerror(errno));
    }
}

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
bool readHttpRequest(int client_fd, gateway_impl::HttpRequest& request) {
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
    }

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
    }

    const std::size_t query_pos = target.find('?');
    if (query_pos == std::string::npos) {
        request.path = target;
    } else {
        request.path = target.substr(0, query_pos);
        request.query = target.substr(query_pos + 1);
    }

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
    }

    const std::string body_prefix = raw.substr(header_end + 4);
    if (body_prefix.size() >= content_length) {
        request.body = body_prefix.substr(0, content_length);
        return true;
    }

    request.body = body_prefix;
    while (request.body.size() < content_length) {
        const ssize_t n = ::read(client_fd, buffer, sizeof(buffer));
        if (n <= 0) {
            return false;
        }
        request.body.append(buffer, static_cast<std::size_t>(n));
    }
    request.body.resize(content_length);
    return true;
}

// review
void writeHttpResponse(int client_fd, const gateway_impl::HttpResponse& response) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << response.http_status << " OK\r\n";
    oss << "Content-Type: application/json; charset=utf-8\r\n";
    for (const auto& header : response.extra_headers) {
        oss << header.first << ": " << header.second << "\r\n";
    }
    oss << "Content-Length: " << response.body.size() << "\r\n";
    oss << "Connection: close\r\n\r\n";
    oss << response.body;
    const std::string payload = oss.str();
    ::write(client_fd, payload.data(), payload.size());
}

// review
void handleHttpClientConnection(int client_fd, const std::string& client_ip) {
    gateway_impl::HttpRequest request;
    request.client_ip = client_ip;
    if (!readHttpRequest(client_fd, request)) {
        LOG_WARN("读取 HTTP 请求失败: client_ip={}", request.client_ip);
        return;
    }

    LOG_INFO("收到请求: {} {}", request.method, request.path);
    const gateway_impl::HttpResponse response = gateway_impl::handleRequest(request);
    writeHttpResponse(client_fd, response);
}

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

// review
int createListenSocket(uint16_t port, int backlog) {
    const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        throw std::runtime_error("创建 socket 失败");
    }

    int reuse = 1;
    ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = INADDR_ANY;

    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        throw std::runtime_error(std::string("bind 失败: ") + std::strerror(errno));
    }
    if (::listen(server_fd, backlog) < 0) {
        throw std::runtime_error("listen 失败");
    }
    return server_fd;
}

// review
void handleWebSocketClient(int client_fd) {
    struct ConnectionGuard {
        ConnectionGuard() {
            g_ws_active_connections.fetch_add(1);
        }
        ~ConnectionGuard() {
            g_ws_active_connections.fetch_sub(1);
        }
    } guard;

    std::string raw;
    char buffer[4096];
    while (raw.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = ::read(client_fd, buffer, sizeof(buffer));
        if (n <= 0) {
            ::close(client_fd);
            return;
        }
        raw.append(buffer, static_cast<std::size_t>(n));
        if (raw.size() > 64 * 1024) {
            ::close(client_fd);
            return;
        }
    }

    std::string payment_session_id;
    if (!gateway_ws::performHandshake(client_fd, raw, payment_session_id)) {
        LOG_WARN("WebSocket 握手失败");
        ::close(client_fd);
        return;
    }

    gateway_ws::WsRegistry::instance().bind(payment_session_id, client_fd);
    gateway_ws::sendTextFrame(client_fd, R"({"type":"BIND_SUCCESS"})");
    LOG_INFO("WebSocket 绑定成功: payment_session_id={}", payment_session_id);

    while (g_running.load()) {
        const ssize_t n = ::read(client_fd, buffer, sizeof(buffer));
        if (n <= 0) {
            break;
        }
    }

    gateway_ws::WsRegistry::instance().unbind(client_fd);
    LOG_INFO("WebSocket 连接断开: payment_session_id={}", payment_session_id);
    ::close(client_fd);
}

// review
void runWebSocketServer(uint16_t port, int backlog, int max_connections) {
    const int server_fd = createListenSocket(port, backlog);
    const int flags = ::fcntl(server_fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
    }
    LOG_INFO("gateway websocket 启动: 0.0.0.0:{}, max_connections={}", port, max_connections);

    while (g_running.load()) {
        pollfd poll_event{};
        poll_event.fd = server_fd;
        poll_event.events = POLLIN;
        const int ready = ::poll(&poll_event, 1, 500);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERROR("websocket poll 失败: {}", std::strerror(errno));
            break;
        }
        if (ready == 0) {
            continue;
        }

        while (g_running.load()) {
            const int client_fd = ::accept(server_fd, nullptr, nullptr);
            if (client_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                LOG_ERROR("websocket accept 失败: {}", std::strerror(errno));
                break;
            }

            if (g_ws_active_connections.load() >= max_connections) {
                LOG_WARN("WebSocket 连接数已达上限 {}, 拒绝新连接", max_connections);
                ::close(client_fd);
                continue;
            }

            std::thread(handleWebSocketClient, client_fd).detach();
        }
    }

    ::close(server_fd);
    LOG_INFO("gateway websocket 已停止监听");
}

} // namespace

// review
int main(int argc, char** argv) {
    try {
        const std::string env_path = resolveEnvPath(argc, argv);
        auto& config = const_cast<payment_config::Config&>(payment_config::Config::instance());
        config.loadFromEnvFile(env_path);

        const auto logger_config = config.logger();
        logger::Logger::getInstance().init(true, logger_config.file, logger_config.max_file_size,
                                           logger_config.max_files, logger_config.console_enabled);
        LOG_INFO("开始启动 payment gateway, env={}", env_path);
        LOG_INFO("日志系统初始化完成: file={}", logger_config.file);
        gateway_session::SessionStore::instance().initFromConfig();

        const auto gateway = config.paymentGateway();
        const auto http_cfg = config.httpServer();
        const int ws_max_connections = config.wsMaxConnections();

        http_server::Options options;
        options.port = static_cast<uint16_t>(gateway.http_port);
        options.listen_backlog = http_cfg.listen_backlog;
        options.worker_threads = http_cfg.worker_threads;
        options.max_concurrent_connections = http_cfg.max_concurrent_connections;
        options.read_timeout_seconds = http_cfg.read_timeout_seconds;

        gateway_http::WorkerPool::instance().init(http_cfg.worker_threads);

        http_server::Server http_server(options);
        installShutdownHandlers();

        LOG_INFO("启动 gateway 服务: http=0.0.0.0:{}, websocket=0.0.0.0:{}", gateway.http_port, gateway.websocket_port);
        std::thread ws_thread(runWebSocketServer, static_cast<uint16_t>(gateway.websocket_port),
                              http_cfg.listen_backlog, ws_max_connections);
        http_server.run(handleHttpClientConnection, &g_running);
        g_running.store(false);
        if (ws_thread.joinable()) {
            ws_thread.join();
        }
        LOG_INFO("gateway 服务已停止");
    } catch (const std::exception& ex) {
        std::cerr << "启动失败: " << ex.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "启动失败: 未知异常" << std::endl;
        return 2;
    }

    return 0;
}
