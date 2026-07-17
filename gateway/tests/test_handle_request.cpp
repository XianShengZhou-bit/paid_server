// review
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "gateway_impl.hpp"
#include "session_store.hpp"
#include "test_support.hpp"

/*
测试：gateway_impl::handleRequest
1. 来源 IP 不在白名单 → 40302
2. 路径不在网关转发列表 → 40400
3. 需登录接口未带 Cookie → 40101
4. 需登录接口 Cookie 无效 → 40101
5. Cookie 有效 → 鉴权通过（后端未启动时通常 502，不应再 401）
6. 支付结果内网回调 /internal/payment/result 只允许后端 IP 调用；用 nginx IP 模拟调用 → 40302
*/

using json = nlohmann::json;

namespace {

// review
gateway_impl::HttpRequest makeReq(const std::string& method, const std::string& path, const std::string& client_ip,
                                  const std::string& cookie = "") {
    gateway_impl::HttpRequest req;
    req.method = method;
    req.path = path;
    req.client_ip = client_ip;
    if (!cookie.empty()) {
        req.headers["Cookie"] = cookie;
    }
    return req;
}

} // namespace

class GatewayHandleRequestTest : public ::testing::Test {
  protected:
    // review
    void SetUp() override {
        try {
            gateway_session::SessionStore::instance().initFromConfig();
        } catch (const std::exception& e) {
            GTEST_SKIP() << "网关 Redis 不可用: " << e.what();
        }
    }

    // review
    void TearDown() override {
        if (!session_id_.empty()) {
            try {
                gateway_test::deleteSessionAsBackend(session_id_);
            } catch (...) {
            }
        }
    }

    std::string session_id_;
};

// review
TEST_F(GatewayHandleRequestTest, RejectsUnknownClientIp) {
    const auto resp = gateway_impl::handleRequest(makeReq("GET", "/api/payment/page", "1.2.3.4"));
    EXPECT_EQ(resp.http_status, 403);
    EXPECT_EQ(json::parse(resp.body).value("code", -1), 40302);
}

// review
TEST_F(GatewayHandleRequestTest, RejectsUnknownPath) {
    const auto resp = gateway_impl::handleRequest(makeReq("GET", "/api/not-exist", gateway_test::nginxIp()));
    EXPECT_EQ(resp.http_status, 404);
    EXPECT_EQ(json::parse(resp.body).value("code", -1), 40400);
}

// review
TEST_F(GatewayHandleRequestTest, AuthRouteRejectsMissingCookie) {
    const auto resp = gateway_impl::handleRequest(makeReq("POST", "/api/payment/confirm", gateway_test::nginxIp()));
    EXPECT_EQ(resp.http_status, 401);
    EXPECT_EQ(json::parse(resp.body).value("code", -1), 40101);
}

// review
TEST_F(GatewayHandleRequestTest, AuthRouteRejectsInvalidSession) {
    const auto resp = gateway_impl::handleRequest(
        makeReq("GET", "/api/payment/orders/ORDX", gateway_test::nginxIp(), "session_id=SID_INVALID_GW"));
    EXPECT_EQ(resp.http_status, 401);
    EXPECT_EQ(json::parse(resp.body).value("code", -1), 40101);
}

// review
TEST_F(GatewayHandleRequestTest, AuthRouteForwardsWhenSessionValid) {
    session_id_ = "SIDGWTESTAUTH01";
    try {
        gateway_test::writeSessionAsBackend(session_id_, "USRGWTESTAUTH01");
    } catch (const std::exception& e) {
        GTEST_SKIP() << "无法写入 Redis session: " << e.what();
    }

    const auto resp = gateway_impl::handleRequest(
        makeReq("GET", "/api/payment/orders/ORDGW01", gateway_test::nginxIp(), "session_id=" + session_id_));
    // 鉴权已通过；若本机无后端进程，转发失败为 502，不应再是 401
    EXPECT_NE(resp.http_status, 401);
    if (resp.http_status == 502) {
        GTEST_LOG_(INFO) << "鉴权已通过，但后端不可达(502)。若需验证完整转发，请先启动 payment_backend（"
                         << gateway_test::backendIp() << ":"
                         << payment_config::Config::instance().paymentBackend().http_port << "）。";
    }
    EXPECT_TRUE(resp.http_status == 502 || resp.http_status == 200 || resp.http_status == 404);
}

// review
TEST_F(GatewayHandleRequestTest, InternalPaymentResultRequiresBackendIp) {
    if (gateway_test::nginxIp() == gateway_test::backendIp()) {
        GTEST_SKIP() << "PAYMENT_NGINX_IP 与 PAYMENT_BACKEND_IP 相同，无法区分来源";
    }
    auto req = makeReq("POST", "/internal/payment/result", gateway_test::nginxIp());
    req.body = R"({"payment_session_id":"PS1","order_sn":"ORD1"})";
    const auto resp = gateway_impl::handleRequest(req);
    EXPECT_EQ(resp.http_status, 403);
    EXPECT_EQ(json::parse(resp.body).value("code", -1), 40302);
}
