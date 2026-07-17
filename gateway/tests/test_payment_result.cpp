#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "gateway_impl.hpp"
#include "test_support.hpp"
#include "ws_registry.hpp"

/*
测试：
1. 非后端 IP → 40302
2. 非法 JSON → 40000
3. 缺 payment_session_id → 40000
4. 未绑定 WS → 50003
5. 已 bind 的 socketpair → 200 且收到 PAYMENT_RESULT 帧
*/

using json = nlohmann::json;

namespace {

// review
gateway_impl::HttpRequest makeResultRequest(const std::string& client_ip, const std::string& body) {
    gateway_impl::HttpRequest req;
    req.method = "POST";
    req.path = "/internal/payment/result";
    req.client_ip = client_ip;
    req.headers["Content-Type"] = "application/json";
    req.body = body;
    return req;
}

} // namespace

// review
TEST(GatewayPaymentResultTest, RejectsNonBackendIp) {
    const auto resp = gateway_impl::handlePaymentResult(
        makeResultRequest("8.8.8.8", R"({"payment_session_id":"PS1","order_sn":"ORD1"})"));
    EXPECT_EQ(resp.http_status, 403);
    EXPECT_EQ(json::parse(resp.body).value("code", -1), 40302);
}

// review
TEST(GatewayPaymentResultTest, RejectsInvalidJson) {
    const auto resp = gateway_impl::handlePaymentResult(makeResultRequest(gateway_test::backendIp(), "{bad"));
    EXPECT_EQ(resp.http_status, 400);
    EXPECT_EQ(json::parse(resp.body).value("code", -1), 40000);
}

// review
TEST(GatewayPaymentResultTest, RejectsEmptyPaymentSessionId) {
    const auto resp = gateway_impl::handlePaymentResult(
        makeResultRequest(gateway_test::backendIp(), R"({"payment_session_id":"","order_sn":"ORD1"})"));
    EXPECT_EQ(resp.http_status, 400);
    EXPECT_EQ(json::parse(resp.body).value("code", -1), 40000);
}

// review
TEST(GatewayPaymentResultTest, FailsWhenNoWebSocketBound) {
    const auto resp = gateway_impl::handlePaymentResult(
        makeResultRequest(gateway_test::backendIp(),
                          R"({"payment_session_id":"PS_NO_BIND_XYZ","order_sn":"ORD1","payment_status":"PAID"})"));
    EXPECT_EQ(resp.http_status, 500);
    EXPECT_EQ(json::parse(resp.body).value("code", -1), 50003);
}

// review
TEST(GatewayPaymentResultTest, PushesPaymentResultToBoundSocket) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    const std::string session_id = "PSTESTRESULT01";
    gateway_ws::WsRegistry::instance().bind(session_id, fds[0]); // 绑定session_id和fd

    const auto resp = gateway_impl::handlePaymentResult(
        makeResultRequest(gateway_test::backendIp(), json{{"payment_session_id", session_id},
                                                          {"order_sn", "ORDTEST01"},
                                                          {"payment_status", "PAID"},
                                                          {"message", "支付成功"}}
                                                         .dump()));

    EXPECT_EQ(resp.http_status, 200);
    EXPECT_EQ(json::parse(resp.body).value("code", -1), 0);

    char buf[1024] = {};
    const ssize_t n = ::read(fds[1], buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);
    const std::string frame(buf, static_cast<std::size_t>(n));
    EXPECT_NE(frame.find("PAYMENT_RESULT"), std::string::npos);
    EXPECT_NE(frame.find(session_id), std::string::npos);
    EXPECT_NE(frame.find("PAID"), std::string::npos);

    gateway_ws::WsRegistry::instance().unbind(fds[0]);
    ::close(fds[0]);
    ::close(fds[1]);
}
