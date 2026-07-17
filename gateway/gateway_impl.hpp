// review
#pragma once

#include <cctype>
#include <exception>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.hpp"
#include "http_forwarder.hpp"
#include "logger.hpp"
#include "session_store.hpp"
#include "ws_registry.hpp"

namespace gateway_impl {

using json = nlohmann::json;

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string client_ip;
};

struct HttpResponse {
    int http_status = 200;
    std::string body;
    std::vector<std::pair<std::string, std::string>> extra_headers;
};

// review
inline std::string toLower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

// review
inline std::string headerValue(const HttpRequest& req, const std::string& name) {
    const std::string key = toLower(name);
    for (const auto& item : req.headers) {
        if (toLower(item.first) == key) {
            return item.second;
        }
    }
    return "";
}

// review
// 从http报头中的cookie中获取指定 name键 对应的值
inline std::string cookieValue(const std::string& cookie_header, const std::string& name) {
    std::size_t pos = 0;
    while (pos < cookie_header.size()) {
        const std::size_t semi = cookie_header.find(';', pos);
        const std::string part = cookie_header.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        const std::size_t eq = part.find('=');
        if (eq != std::string::npos) {
            std::string key = part.substr(0, eq);
            std::string value = part.substr(eq + 1);
            while (!key.empty() && key.front() == ' ') {
                key.erase(key.begin());
            }
            while (!value.empty() && value.front() == ' ') {
                value.erase(value.begin());
            }
            if (key == name) {
                return value;
            }
        }
        if (semi == std::string::npos) {
            break;
        }
        pos = semi + 1;
    }
    return "";
}

// review
inline json makeOk(const json& data = json::object()) {
    return json{{"code", 0}, {"message", ""}, {"data", data}};
}

// review
inline json makeErr(int code, const std::string& message, const json& data = json::object()) {
    return json{{"code", code}, {"message", message}, {"data", data}};
}

// review
inline HttpResponse respondJson(int http_status, const json& envelope) {
    return HttpResponse{http_status, envelope.dump(), {}};
}

// review
// 判断是否需要登录认证
inline bool requiresAuth(const std::string& method, const std::string& path) {
    if (method == "GET" && path.rfind("/api/payment/orders/", 0) == 0) {
        return true;
    }
    if (method == "POST" && path == "/api/payment/confirm") {
        return true;
    }
    if (method == "POST" && path == "/api/users/real-auth/complete") {
        return true;
    }
    return false;
}

// review
// 判断是否要将该请求转发给后端
inline bool isForwardApi(const std::string& path) {
    return path.rfind("/api/payment/", 0) == 0 || path.rfind("/api/auth/", 0) == 0 ||
           path == "/api/users/real-auth/complete";
}

inline gateway_forward::HttpRequest toForwardRequest(const HttpRequest& request) {
    gateway_forward::HttpRequest forward;
    forward.method = request.method;
    forward.path = request.path;
    forward.query = request.query;
    forward.body = request.body;
    const std::string content_type = headerValue(request, "Content-Type");
    if (!content_type.empty()) {
        forward.headers["Content-Type"] = content_type;
    }
    const std::string cookie = headerValue(request, "Cookie");
    if (!cookie.empty()) {
        forward.headers["Cookie"] = cookie;
    }
    return forward;
}

// review
inline HttpResponse handlePaymentResult(const HttpRequest& request) {
    LOG_INFO("开始处理支付结果通知: client_ip={}", request.client_ip);
    if (request.client_ip != payment_config::Config::instance().paymentBackend().ip) {
        LOG_WARN("支付结果通知失败: 非后端来源, client_ip={}", request.client_ip);
        return respondJson(403, makeErr(40302, "仅允许后端调用"));
    }

    json body;
    try {
        body = json::parse(request.body);
    } catch (...) {
        LOG_WARN("支付结果通知失败: 请求体不是合法 JSON");
        return respondJson(400, makeErr(40000, "请求体不是合法 JSON"));
    }

    const std::string payment_session_id = body.value("payment_session_id", "");
    const std::string order_sn = body.value("order_sn", "");
    if (payment_session_id.empty()) {
        LOG_WARN("支付结果通知失败: payment_session_id 为空");
        return respondJson(400, makeErr(40000, "payment_session_id 不能为空"));
    }

    LOG_INFO("准备 WebSocket 推送: payment_session_id={}, order_sn={}", payment_session_id, order_sn);

    const json message = json{{"type", "PAYMENT_RESULT"},
                              {"payment_session_id", payment_session_id},
                              {"order_sn", order_sn},
                              {"payment_status", body.value("payment_status", "")},
                              {"message", body.value("message", "")}};

    if (!gateway_ws::WsRegistry::instance().push(payment_session_id, message.dump())) {
        LOG_WARN("支付结果通知失败: WebSocket 推送失败, payment_session_id={}", payment_session_id);
        return respondJson(500, makeErr(50003, "WebSocket 推送失败"));
    }

    LOG_INFO("支付结果通知成功: payment_session_id={}, order_sn={}", payment_session_id, order_sn);
    return respondJson(200, makeOk());
}

// review
// 同步阻塞等后端发响应
inline HttpResponse forwardRequest(const HttpRequest& request, const std::string& user_id) {
    LOG_INFO("开始转发后端: {} {} user_id={}", request.method, request.path, user_id.empty() ? "(none)" : user_id);
    const gateway_forward::HttpResponse backend = gateway_forward::forwardToBackend(toForwardRequest(request), user_id);
    LOG_INFO("转发后端完成: {} {} -> http {}", request.method, request.path, backend.status);
    return HttpResponse{backend.status, backend.body, backend.extra_headers};
}

// review
inline HttpResponse handleRequest(const HttpRequest& request) {
    LOG_INFO("开始处理请求: {} {} client_ip={}", request.method, request.path, request.client_ip);
    if (request.client_ip != payment_config::Config::instance().paymentBackend().ip &&
        request.client_ip != payment_config::Config::instance().paymentNginx().ip) {
        LOG_WARN("请求处理失败: 非合理来源, client_ip={}", request.client_ip);
        return respondJson(403, makeErr(40302, "调用来源非法"));
    }

    HttpResponse response;
    if (request.method == "POST" && request.path == "/internal/payment/result") {
        response = handlePaymentResult(request);
    } else if (!isForwardApi(request.path)) {
        LOG_WARN("请求处理失败: 接口不存在, {} {}", request.method, request.path);
        response = respondJson(404, makeErr(40400, "接口不存在"));
    } else {
        std::string user_id;
        if (requiresAuth(request.method, request.path)) {
            const std::string session_id = cookieValue(headerValue(request, "Cookie"), "session_id");
            if (session_id.empty()) {
                LOG_WARN("请求处理失败: 缺少 session_id, {} {}", request.method, request.path);
                response = respondJson(401, makeErr(40101, "登录态无效"));
            } else {
                try {
                    const auto uid = gateway_session::SessionStore::instance().userIdOf(session_id);
                    if (!uid.has_value()) {
                        LOG_WARN("请求处理失败: session 无效, {} {}", request.method, request.path);
                        response = respondJson(401, makeErr(40101, "登录态无效"));
                    } else {
                        user_id = *uid;
                        response = forwardRequest(request, user_id);
                    }
                } catch (const std::exception& ex) {
                    LOG_ERROR("Redis 鉴权失败: {} {}, error={}", request.method, request.path, ex.what());
                    response = respondJson(503, makeErr(50002, "登录态服务暂不可用"));
                }
            }
        } else {
            response = forwardRequest(request, user_id);
        }
    }

    LOG_INFO("完成处理请求: {} {} -> http {}", request.method, request.path, response.http_status);
    return response;
}

} // namespace gateway_impl
