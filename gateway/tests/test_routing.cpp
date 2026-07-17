// review
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "gateway_impl.hpp"

/*
测试：
1. requiresAuth：需登录路径 vs 公开路径
2. isForwardApi：可转发 vs 404
3. cookieValue：解析 session_id
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
