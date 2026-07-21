// review
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "gateway_impl.hpp"

/*
测试：gateway_impl 路由与转发辅助函数
1. requiresAuth：需登录路径 vs 公开路径
2. isForwardApi：可转发 vs 404
3. cookieValue：从 Cookie 头解析 session_id
4. toForwardRequest：转发时保留 X-Forwarded-For、User-Agent、Cookie 等客户端头
5. toForwardRequest：无 X-Forwarded-For 时用 X-Real-IP 兜底写入 X-Forwarded-For
*/

// review
TEST(GatewayRoutingTest, RequiresAuthForProtectedRoutes) {
    EXPECT_TRUE(gateway_impl::requiresAuth("GET", "/api/payment/orders/ORD001"));
    EXPECT_TRUE(gateway_impl::requiresAuth("POST", "/api/payment/confirm"));
    EXPECT_TRUE(gateway_impl::requiresAuth("POST", "/api/users/real-auth/complete"));

    EXPECT_FALSE(gateway_impl::requiresAuth("POST", "/api/auth/login"));
    EXPECT_FALSE(gateway_impl::requiresAuth("POST", "/api/auth/logout"));
    EXPECT_FALSE(gateway_impl::requiresAuth("GET", "/api/payment/page"));
    EXPECT_FALSE(gateway_impl::requiresAuth("POST", "/api/payment/attempt/init"));
    EXPECT_FALSE(gateway_impl::requiresAuth("POST", "/api/payment/pay"));
}

// review
TEST(GatewayRoutingTest, IsForwardApiAcceptsKnownPrefixes) {
    EXPECT_TRUE(gateway_impl::isForwardApi("/api/payment/orders/ORD001"));
    EXPECT_TRUE(gateway_impl::isForwardApi("/api/payment/confirm"));
    EXPECT_TRUE(gateway_impl::isForwardApi("/api/payment/page"));
    EXPECT_TRUE(gateway_impl::isForwardApi("/api/auth/login"));
    EXPECT_TRUE(gateway_impl::isForwardApi("/api/users/real-auth/complete"));
    EXPECT_FALSE(gateway_impl::isForwardApi("/api/unknown"));
    EXPECT_FALSE(gateway_impl::isForwardApi("/internal/payment/result"));
}

// review
TEST(GatewayRoutingTest, CookieValueParsesSessionId) {
    EXPECT_EQ(gateway_impl::cookieValue("session_id=SIDABC; Path=/", "session_id"), "SIDABC");
    EXPECT_EQ(gateway_impl::cookieValue("a=1; session_id=SIDXYZ; b=2", "session_id"), "SIDXYZ");
    EXPECT_EQ(gateway_impl::cookieValue("other=1", "session_id"), "");
    EXPECT_EQ(gateway_impl::cookieValue("", "session_id"), "");
}

// review
TEST(GatewayRoutingTest, ToForwardRequestPreservesClientHeaders) {
    gateway_impl::HttpRequest request;
    request.method = "POST";
    request.path = "/api/payment/attempt/init";
    request.body = R"({"qr_token":"t"})";
    request.headers["Content-Type"] = "application/json";
    request.headers["Cookie"] = "session_id=abc";
    request.headers["X-Forwarded-For"] = "203.0.113.10";
    request.headers["User-Agent"] = "MobileBrowser/1.0";

    const gateway_forward::HttpRequest forward = gateway_impl::toForwardRequest(request);
    EXPECT_EQ(forward.headers.at("X-Forwarded-For"), "203.0.113.10");
    EXPECT_EQ(forward.headers.at("User-Agent"), "MobileBrowser/1.0");
    EXPECT_EQ(forward.headers.at("Cookie"), "session_id=abc");
}

// review
TEST(GatewayRoutingTest, ToForwardRequestFallsBackToXRealIp) {
    gateway_impl::HttpRequest request;
    request.method = "POST";
    request.path = "/api/payment/pay";
    request.headers["X-Real-IP"] = "198.51.100.20";

    const gateway_forward::HttpRequest forward = gateway_impl::toForwardRequest(request);
    EXPECT_EQ(forward.headers.at("X-Forwarded-For"), "198.51.100.20");
}
