// review
#include <gtest/gtest.h>

#include "ws_registry.hpp"

/*
测试：
1. queryParam 解析
2. makeAcceptKey 符合 RFC6455 示例向量
*/

// review
TEST(GatewayWsHelpersTest, QueryParamParsesPaymentSessionId) {
    EXPECT_EQ(gateway_ws::queryParam("payment_session_id=PS123&x=1", "payment_session_id"), "PS123");
    EXPECT_EQ(gateway_ws::queryParam("a=1&payment_session_id=PS999", "payment_session_id"), "PS999");
    EXPECT_EQ(gateway_ws::queryParam("payment_session_id=PSONLY", "payment_session_id"), "PSONLY");
    EXPECT_EQ(gateway_ws::queryParam("x=1", "payment_session_id"), "");
}

// review
TEST(GatewayWsHelpersTest, MakeAcceptKeyMatchesRfc6455Sample) {
    // RFC 6455 section 1.3 example
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    EXPECT_EQ(gateway_ws::makeAcceptKey(key), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}
