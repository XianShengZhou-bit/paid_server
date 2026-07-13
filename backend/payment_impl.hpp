// review
#pragma once

#include <cctype>
#include <chrono>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <openssl/rand.h>

#include <nlohmann/json.hpp>

#include "config.hpp"
#include "gateway_notify.hpp"
#include "jwt.hpp"
#include "logger.hpp"
#include "mysql_pool.hpp"
#include "password_verifier.hpp"

namespace payment_impl {

using json = nlohmann::json;

// review
struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;
    std::string body;
};

// review
struct HttpResponse {
    int http_status = 200;
    json envelope = json::object();
};

// review
inline std::string toLower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

// review
// HTTP 标准规定：请求头的名字大小写不敏感，而不同客户端发过来的大小写又往往不一致，所以查找时要做不区分大小写的比较
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
inline std::string queryParam(const std::string& query, const std::string& key) {
    std::size_t pos = 0;
    while (pos < query.size()) {
        const std::size_t amp = query.find('&', pos);
        const std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            if (pair.substr(0, eq) == key) {
                return pair.substr(eq + 1);
            }
        }
        if (amp == std::string::npos) {
            break;
        }
        pos = amp + 1;
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
inline HttpResponse respond(int http_status, const json& envelope) {
    return HttpResponse{http_status, envelope};
}

// review
inline std::string randomHex(std::size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1) {
        throw std::runtime_error("生成随机数失败");
    }
    std::ostringstream oss;
    for (unsigned char byte : buf) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

// review
inline std::string makePaymentSessionId() {
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return "PS" + std::to_string(now) + randomHex(4);
}

// review
inline std::string makeRequestId() {
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return "REQ" + std::to_string(now) + randomHex(4);
}

// review
inline bool accountPayable(int status) {
    using payment_mysql::AccountStatus;
    return status == static_cast<int>(AccountStatus::OnSale) || status == static_cast<int>(AccountStatus::Trading);
}

// review
inline bool parseJsonBody(const HttpRequest& req, json& out, std::string& error) {
    if (req.body.empty()) {
        out = json::object();
        return true;
    }
    try {
        out = json::parse(req.body);
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

// review
// 提取订单号
inline bool extractOrderSn(const std::string& path, std::string& order_sn) {
    static const std::string prefix = "/api/payment/orders/";
    order_sn = path.substr(prefix.size());
    return !order_sn.empty();
}

// review
inline HttpResponse handleGetOrder(const HttpRequest& req) {
    std::string order_sn;
    extractOrderSn(req.path, order_sn);
    LOG_INFO("开始查询订单: order_sn={}", order_sn);

    const std::string user_id = headerValue(req, "X-User-Id");
    if (user_id.empty()) {
        LOG_WARN("查询订单失败: 缺少 X-User-Id, order_sn={}", order_sn);
        return respond(400, makeErr(40001, "缺少 X-User-Id"));
    }

    auto& pool = payment_mysql::MySqlPool::instance();
    const auto order = pool.getOrderBySn(order_sn);
    if (!order.has_value()) {
        LOG_WARN("查询订单失败: 订单不存在, order_sn={}, user_id={}", order_sn, user_id);
        return respond(404, makeErr(40401, "订单不存在"));
    }
    if (order->buyer_id != user_id) {
        LOG_WARN("查询订单失败: 订单归属不匹配, order_sn={}, user_id={}", order_sn, user_id);
        return respond(404, makeErr(40401, "订单不存在")); // 订单归属不匹配，但不对外暴露细节
    }

    const auto account = pool.getAccountById(order->account_id);
    if (!account.has_value()) {
        LOG_WARN("查询订单失败: 账号不存在, order_sn={}, account_id={}", order_sn, order->account_id);
        return respond(404, makeErr(40402, "账号不存在"));
    }

    LOG_INFO("查询订单成功: order_sn={}, user_id={}, status={}", order_sn, user_id, order->status);
    return respond(200, makeOk(json{{"order_sn", order->order_sn},
                                    {"price", order->price},
                                    {"status", order->status},
                                    {"account_id", order->account_id},
                                    {"account", json{{"account_id", account->account_id},
                                                     {"game_name", account->game_name},
                                                     {"server_area", account->server_area},
                                                     {"price", account->price},
                                                     {"account_level", account->account_level},
                                                     {"status", account->status}}}}));
}

// review
inline HttpResponse handleConfirm(const HttpRequest& req) {
    const std::string user_id = headerValue(req, "X-User-Id");
    LOG_INFO("开始支付确认: user_id={}", user_id.empty() ? "(missing)" : user_id);

    if (user_id.empty()) {
        LOG_WARN("支付确认失败: 缺少 X-User-Id");
        return respond(400, makeErr(40001, "缺少 X-User-Id"));
    }

    json body;
    std::string parse_error;
    if (!parseJsonBody(req, body, parse_error)) {
        LOG_WARN("支付确认失败: 请求体 JSON 非法, user_id={}", user_id);
        return respond(400, makeErr(40001, "请求体 JSON 非法"));
    }

    const std::string order_sn = body.value("order_sn", "");
    const bool agreement_checked = body.value("agreement_checked", false); // 前端不可信，因此后端仍然需要校验
    if (order_sn.empty() || !agreement_checked) {
        LOG_WARN("支付确认失败: 参数不完整或未勾选协议, user_id={}, order_sn={}", user_id, order_sn);
        return respond(400, makeErr(40001, "参数不完整或未勾选协议"));
    }

    auto& pool = payment_mysql::MySqlPool::instance();
    const auto user = pool.getUserByUserId(user_id);
    if (!user.has_value() || !user->isNormal()) {
        LOG_WARN("支付确认失败: 用户状态异常, user_id={}, order_sn={}", user_id, order_sn);
        return respond(403, makeErr(40301, "用户状态异常"));
    }
    if (!user->isRealAuthCompleted()) {
        LOG_WARN("支付确认失败: 用户未完成实名, user_id={}, order_sn={}", user_id, order_sn);
        return respond(
            409, makeErr(40910, "用户未补全邮箱号或身份证号")); // 前端识别40910错误码自动显式跳转按钮跳转到指定页面
    }

    const auto order = pool.getOrderBySn(order_sn);
    if (!order.has_value() || order->buyer_id != user_id) {
        LOG_WARN("支付确认失败: 订单不存在或归属不匹配, user_id={}, order_sn={}", user_id, order_sn);
        return respond(404, makeErr(40401, "订单不存在或归属不匹配")); // 统一返回40401，屏蔽底层细节
    }
    if (order->status != static_cast<int>(payment_mysql::OrderStatus::PendingPay)) {
        LOG_WARN("支付确认失败: 订单不可支付, order_sn={}, status={}", order_sn, order->status);
        return respond(409, makeErr(40901, "订单不可支付"));
    }

    const auto account = pool.getAccountById(order->account_id);
    if (!account.has_value() || !accountPayable(account->status)) {
        LOG_WARN("支付确认失败: 账号不可交易, order_sn={}, account_id={}", order_sn, order->account_id);
        return respond(409, makeErr(40903, "账号不可交易"));
    }

    const auto& config = payment_config::Config::instance();
    const auto runtime = config.runtime();
    const auto nginx = config.paymentNginx();
    payment_jwt::JwtService jwt_service = payment_jwt::JwtService::fromConfig();

    std::string payment_session_id;
    auto session = pool.getActivePaymentSessionByOrderSn(order_sn);
    if (session.has_value()) {
        payment_session_id = session->payment_session_id;
        LOG_INFO("复用已有支付会话: order_sn={}, payment_session_id={}", order_sn, payment_session_id);
    } else {
        payment_session_id = makePaymentSessionId();
        payment_mysql::CreatePaymentSessionInput input;
        input.payment_session_id = payment_session_id;
        input.order_sn = order_sn;
        input.account_id = order->account_id;
        input.buyer_id = user_id;
        input.buyer_contact_masked = user->email_masked;
        input.ttl_seconds = runtime.payment_session_ttl_seconds;
        pool.createPaymentSession(input);
        LOG_INFO("创建支付会话: order_sn={}, payment_session_id={}", order_sn, payment_session_id);
    }

    payment_jwt::PaymentQrPayload payload;
    payload.payment_session_id = payment_session_id;
    const std::string qr_token = jwt_service.issuePaymentQrToken(payload);

    const std::string qr_payload = "http://" + nginx.public_host + ":" + std::to_string(nginx.port) +
                                   nginx.scan_page_path + "?token=" + qr_token; // 手机扫码支付页路径
    const std::string websocket_url = "ws://" + nginx.public_host + ":" + std::to_string(nginx.port) +
                                      "/ws/payment?payment_session_id=" + payment_session_id; // 经 Nginx 反代到网关 WS

    LOG_INFO("支付确认成功: order_sn={}, payment_session_id={}, user_id={}", order_sn, payment_session_id, user_id);
    return respond(200, makeOk(json{{"payment_session_id", payment_session_id},
                                    {"qr_payload", qr_payload},
                                    {"websocket_url", websocket_url}}));
}

// review
inline HttpResponse handleRealAuthComplete(const HttpRequest& req) {
    const std::string user_id = headerValue(req, "X-User-Id");
    LOG_INFO("开始实名补全: user_id={}", user_id.empty() ? "(missing)" : user_id);

    if (user_id.empty()) {
        LOG_WARN("实名补全失败: 缺少 X-User-Id");
        return respond(400, makeErr(40001, "缺少 X-User-Id"));
    }

    json body;
    std::string parse_error;
    if (!parseJsonBody(req, body, parse_error)) {
        LOG_WARN("实名补全失败: 请求体 JSON 非法, user_id={}", user_id);
        return respond(400, makeErr(40001, "请求体 JSON 非法"));
    }

    const std::string email = body.value("email", "");
    const std::string id_card = body.value("id_card", "");
    const std::string return_url = body.value("return_url", "");
    if (email.empty() || id_card.empty() || return_url.empty()) {
        LOG_WARN("实名补全失败: 参数不完整, user_id={}", user_id);
        return respond(400, makeErr(40001, "参数不完整"));
    }

    try {
        payment_jwt::HashService hash_service = payment_jwt::HashService::fromConfig();
        const std::string email_norm = payment_jwt::HashService::normalizeEmail(email);
        const std::string id_card_norm = payment_jwt::HashService::normalizeIdCard(id_card);

        payment_mysql::UserRealAuthUpdate update;
        update.user_id = user_id;
        update.email_hash = hash_service.hashEmail(email_norm);
        update.email_masked = payment_jwt::HashService::maskEmail(email_norm);
        update.id_card_hash = hash_service.hashIdCard(id_card_norm);
        update.id_card_masked = payment_jwt::HashService::maskIdCard(id_card_norm);

        if (!payment_mysql::MySqlPool::instance().updateUserRealAuthProfile(update)) {
            LOG_ERROR("实名补全失败: 数据库更新失败, user_id={}", user_id);
            return respond(500, makeErr(50001, "更新实名信息失败"));
        }
    } catch (const std::exception& ex) {
        LOG_WARN("实名补全失败: {}, user_id={}", ex.what(), user_id);
        return respond(400, makeErr(40001, ex.what()));
    }

    LOG_INFO("实名补全成功: user_id={}, return_url={}", user_id, return_url);
    return respond(200, makeOk(json{{"redirect_url", return_url}}));
}

// review
// 校验 qr_token，查库，返回页面 B 要展示的 JSON 数据
inline HttpResponse handlePaymentPage(const HttpRequest& req) {
    const std::string token = queryParam(req.query, "token");
    LOG_INFO("开始加载支付页: token_present={}", !token.empty());

    if (token.empty()) {
        LOG_WARN("加载支付页失败: 缺少 token");
        return respond(400, makeErr(40001, "缺少 token"));
    }

    payment_jwt::JwtService jwt_service = payment_jwt::JwtService::fromConfig();
    const payment_jwt::PaymentQrVerifyResult verified = jwt_service.verifyPaymentQrToken(token);
    if (!verified.valid) {
        LOG_WARN("加载支付页失败: qr_token 无效, reason={}", verified.error_message);
        return respond(401, makeErr(40101, verified.error_message));
    }

    auto& pool = payment_mysql::MySqlPool::instance();
    const auto session = pool.getPaymentSessionById(verified.payload.payment_session_id);
    if (!session.has_value() || session->status != static_cast<int>(payment_mysql::PaymentSessionStatus::WaitingPay)) {
        LOG_WARN("加载支付页失败: 支付会话无效, payment_session_id={}", verified.payload.payment_session_id);
        return respond(409, makeErr(40903, "支付会话无效"));
    }

    const auto order = pool.getOrderBySn(session->order_sn);
    if (!order.has_value() || order->status != static_cast<int>(payment_mysql::OrderStatus::PendingPay)) {
        LOG_WARN("加载支付页失败: 订单不可支付, order_sn={}", session->order_sn);
        return respond(409, makeErr(40901, "订单不可支付"));
    }

    const auto account = pool.getAccountById(session->account_id);
    if (!account.has_value() || !accountPayable(account->status)) {
        LOG_WARN("加载支付页失败: 账号不可交易, account_id={}", session->account_id);
        return respond(409, makeErr(40902, "账号不可交易"));
    }

    LOG_INFO("加载支付页成功: payment_session_id={}, order_sn={}", session->payment_session_id, order->order_sn);
    return respond(200, makeOk(json{{"payment_session_id", session->payment_session_id},
                                    {"order_sn", order->order_sn},
                                    {"price", order->price},
                                    {"game_name", account->game_name},
                                    {"status", "WAITING_PAY"}}));
}

// review
inline HttpResponse handleAttemptInit(const HttpRequest& req) {
    LOG_INFO("开始初始化支付尝试");

    json body;
    std::string parse_error;
    if (!parseJsonBody(req, body, parse_error)) {
        LOG_WARN("初始化支付尝试失败: 请求体 JSON 非法");
        return respond(400, makeErr(40001, "请求体 JSON 非法"));
    }

    const std::string qr_token = body.value("qr_token", "");
    if (qr_token.empty()) {
        LOG_WARN("初始化支付尝试失败: 缺少 qr_token");
        return respond(400, makeErr(40001, "缺少 qr_token"));
    }

    payment_jwt::JwtService jwt_service = payment_jwt::JwtService::fromConfig();
    const payment_jwt::PaymentQrVerifyResult verified = jwt_service.verifyPaymentQrToken(qr_token);
    if (!verified.valid) {
        LOG_WARN("初始化支付尝试失败: qr_token 无效, reason={}", verified.error_message);
        return respond(401, makeErr(40102, verified.error_message));
    }

    auto& pool = payment_mysql::MySqlPool::instance();
    const auto session = pool.getPaymentSessionById(verified.payload.payment_session_id);
    if (!session.has_value() || session->status != static_cast<int>(payment_mysql::PaymentSessionStatus::WaitingPay)) {
        LOG_WARN("初始化支付尝试失败: 支付会话无效, payment_session_id={}", verified.payload.payment_session_id);
        return respond(409, makeErr(40911, "支付会话无效"));
    }

    const auto order = pool.getOrderBySn(session->order_sn);
    if (!order.has_value() || order->status != static_cast<int>(payment_mysql::OrderStatus::PendingPay)) {
        LOG_WARN("初始化支付尝试失败: 订单不可支付, order_sn={}", session->order_sn);
        return respond(409, makeErr(40901, "订单不可支付"));
    }

    const auto account = pool.getAccountById(session->account_id);
    if (!account.has_value() || !accountPayable(account->status)) {
        LOG_WARN("初始化支付尝试失败: 账号不可交易, account_id={}", session->account_id);
        return respond(409, makeErr(40903, "账号不可交易"));
    }

    const std::string request_id = makeRequestId();
    payment_mysql::CreatePaymentAttemptInput attempt;
    attempt.request_id = request_id;
    attempt.payment_session_id = session->payment_session_id;
    attempt.order_sn = session->order_sn;
    attempt.account_id = session->account_id;
    attempt.buyer_id = session->buyer_id;
    attempt.client_ip = headerValue(req, "X-Forwarded-For");
    attempt.user_agent = headerValue(req, "User-Agent");
    pool.createPaymentAttemptProcessing(attempt);

    LOG_INFO("初始化支付尝试成功: request_id={}, payment_session_id={}, order_sn={}", request_id,
             session->payment_session_id, session->order_sn);
    return respond(200, makeOk(json{{"request_id", request_id}}));
}

// review
inline HttpResponse handlePay(const HttpRequest& req) {
    json body;
    std::string parse_error;
    if (!parseJsonBody(req, body, parse_error)) {
        LOG_WARN("支付失败: 请求体 JSON 非法");
        return respond(400, makeErr(40001, "请求体 JSON 非法"));
    }

    const std::string request_id = body.value("request_id", "");
    const std::string qr_token = body.value("qr_token", "");
    const std::string login_password = body.value("login_password", "");
    LOG_INFO("开始支付: request_id={}", request_id.empty() ? "(missing)" : request_id);

    if (request_id.empty() || qr_token.empty() || login_password.empty()) {
        LOG_WARN("支付失败: 参数不完整, request_id={}", request_id);
        return respond(400, makeErr(40001, "参数不完整"));
    }

    auto& pool = payment_mysql::MySqlPool::instance();
    const auto attempt = pool.getPaymentAttemptByRequestId(request_id);
    if (!attempt.has_value()) {
        LOG_WARN("支付失败: 支付尝试不存在, request_id={}", request_id);
        return respond(404, makeErr(40404, "支付尝试不存在"));
    }

    if (attempt->result == static_cast<int>(payment_mysql::PaymentAttemptResult::Success)) {
        LOG_INFO("支付幂等返回: request_id={} 已成功", request_id);
        return respond(200, makeOk(json{{"payment_status", "PAID"}, {"request_id", request_id}}));
    }
    if (attempt->result == static_cast<int>(payment_mysql::PaymentAttemptResult::Failed)) {
        LOG_WARN("支付失败: 支付尝试已失败, request_id={}, reason={}", request_id, attempt->fail_reason);
        return respond(409, makeErr(40904, attempt->fail_reason.empty() ? "支付已失败" : attempt->fail_reason));
    }

    payment_jwt::JwtService jwt_service = payment_jwt::JwtService::fromConfig();
    const payment_jwt::PaymentQrVerifyResult verified = jwt_service.verifyPaymentQrToken(qr_token);
    if (!verified.valid) {
        pool.updatePaymentAttemptResult(request_id, payment_mysql::PaymentAttemptResult::Failed, "INVALID_QR_TOKEN");
        LOG_WARN("支付失败: qr_token 无效, request_id={}, reason={}", request_id, verified.error_message);
        return respond(401, makeErr(40102, verified.error_message));
    }

    const auto session = pool.getPaymentSessionById(verified.payload.payment_session_id);
    if (!session.has_value() || session->status != static_cast<int>(payment_mysql::PaymentSessionStatus::WaitingPay)) {
        pool.updatePaymentAttemptResult(request_id, payment_mysql::PaymentAttemptResult::Failed, "SESSION_INVALID");
        LOG_WARN("支付失败: 支付会话无效, request_id={}, payment_session_id={}", request_id,
                 verified.payload.payment_session_id);
        return respond(409, makeErr(40905, "支付会话无效"));
    }

    if (attempt->payment_session_id != session->payment_session_id) {
        pool.updatePaymentAttemptResult(request_id, payment_mysql::PaymentAttemptResult::Failed, "SESSION_MISMATCH");
        LOG_WARN("支付失败: 支付会话不匹配, request_id={}", request_id);
        return respond(409, makeErr(40905, "支付会话不匹配"));
    }

    const auto order = pool.getOrderBySn(session->order_sn);
    if (!order.has_value() || order->status != static_cast<int>(payment_mysql::OrderStatus::PendingPay)) {
        pool.updatePaymentAttemptResult(request_id, payment_mysql::PaymentAttemptResult::Failed, "ORDER_NOT_PAYABLE");
        LOG_WARN("支付失败: 订单不可支付, request_id={}, order_sn={}", request_id, session->order_sn);
        return respond(409, makeErr(40901, "订单不可支付"));
    }

    const auto account = pool.getAccountById(session->account_id);
    if (!account.has_value() || !accountPayable(account->status)) {
        pool.updatePaymentAttemptResult(request_id, payment_mysql::PaymentAttemptResult::Failed, "ACCOUNT_NOT_PAYABLE");
        LOG_WARN("支付失败: 账号不可交易, request_id={}, account_id={}", request_id, session->account_id);
        return respond(409, makeErr(40903, "账号不可交易"));
    }

    const auto user = pool.getUserByUserId(session->buyer_id);
    if (!user.has_value() || !user->isNormal()) {
        pool.updatePaymentAttemptResult(request_id, payment_mysql::PaymentAttemptResult::Failed, "USER_INVALID");
        LOG_WARN("支付失败: 用户状态异常, request_id={}, buyer_id={}", request_id, session->buyer_id);
        return respond(403, makeErr(40301, "用户状态异常"));
    }

    const std::string password_secret = payment_config::Config::instance().passwordHash().secret;
    if (!payment_security::PasswordVerifier::verifyLoginPasswordWithHash(login_password, user->password_hash,
                                                                         password_secret)) {
        pool.updatePaymentAttemptResult(request_id, payment_mysql::PaymentAttemptResult::Failed, "PASSWORD_INVALID");
        LOG_WARN("支付失败: 支付密码错误, request_id={}, buyer_id={}", request_id, session->buyer_id);
        return respond(401, makeErr(40104, "支付密码错误"));
    }

    payment_mysql::PaymentFinalizeInput finalize;
    finalize.payment_session_id = session->payment_session_id;
    finalize.order_sn = order->order_sn;
    finalize.account_id = account->account_id;
    finalize.buyer_id = session->buyer_id;
    finalize.price = order->price;
    finalize.request_id = request_id;

    if (!pool.finalizePaymentSuccess(finalize)) {
        pool.updatePaymentAttemptResult(request_id, payment_mysql::PaymentAttemptResult::Failed, "FINALIZE_TXN_FAILED");
        LOG_ERROR("支付失败: 事务提交失败, request_id={}, order_sn={}", request_id, order->order_sn);
        return respond(409, makeErr(40904, "支付事务失败"));
    }

    pool.updatePaymentAttemptResult(request_id, payment_mysql::PaymentAttemptResult::Success);
    LOG_INFO("支付成功: order_sn={}, payment_session_id={}, request_id={}", order->order_sn,
             session->payment_session_id, request_id);

    // 异步通知网关，避免网关 HTTP 单线程处理 pay 时阻塞 accept 导致死锁
    const std::string notify_session_id = session->payment_session_id;
    const std::string notify_order_sn = order->order_sn;
    std::thread([notify_session_id, notify_order_sn]() {
        payment_gateway_notify::notifyPaymentResult(notify_session_id, notify_order_sn);
    }).detach();
    // TODO 后期使用线程池进行优化

    return respond(200, makeOk(json{{"payment_status", "PAID"}, {"request_id", request_id}}));
}

// review
inline HttpResponse handleRequest(const HttpRequest& req) {
    LOG_INFO("开始处理请求: {} {}", req.method, req.path);
    try {
        HttpResponse response;
        if (req.method == "GET" && req.path.rfind("/api/payment/orders/", 0) == 0) {
            response = handleGetOrder(req);
        } else if (req.method == "POST" && req.path == "/api/payment/confirm") {
            response = handleConfirm(req);
        } else if (req.method == "POST" && req.path == "/api/users/real-auth/complete") {
            response = handleRealAuthComplete(req);
        } else if (req.method == "GET" && req.path == "/api/payment/page") {
            response = handlePaymentPage(req);
        } else if (req.method == "POST" && req.path == "/api/payment/attempt/init") {
            response = handleAttemptInit(req);
        } else if (req.method == "POST" && req.path == "/api/payment/pay") {
            response = handlePay(req);
        } else {
            LOG_WARN("请求处理失败: 未找到接口, {} {}", req.method, req.path);
            response = respond(404, makeErr(40400, "未找到接口"));
        }

        const int biz_code = response.envelope.value("code", -1);
        LOG_INFO("完成处理请求: {} {} -> http {}, code={}", req.method, req.path, response.http_status, biz_code);
        return response;
    } catch (const std::exception& ex) {
        LOG_ERROR("处理请求异常: {} {}, error={}", req.method, req.path, ex.what());
        return respond(500, makeErr(50001, "服务器内部错误"));
    }
}

} // namespace payment_impl
