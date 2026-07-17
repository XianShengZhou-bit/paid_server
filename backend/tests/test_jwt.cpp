// review
#include <cctype>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "config.hpp"
#include "jwt.hpp"

/*
测试：
1. 正常签发 → 校验通过，payment_session_id 一致
2. 篡改 token → 拒
3. 过期 → 拒
4. 非法格式 → 拒
5. 邮箱 hash：相等性 + 摘要合法
6. 身份证号 hash：相等性 + 摘要合法
*/

namespace {

// review
bool isSha256HexDigest(const std::string& digest) {
    if (digest.size() != 64) {
        return false;
    }
    for (unsigned char c : digest) {
        if (std::isxdigit(c) == 0) {
            return false;
        }
    }
    return true;
}

} // namespace

// review
TEST(JwtServiceTest, IssueAndVerifyPaymentQrToken) {
    payment_jwt::JwtService jwt = payment_jwt::JwtService::fromConfig();
    payment_jwt::PaymentQrPayload payload;
    payload.payment_session_id = "PS20260708000000000001";

    const std::string token = jwt.issuePaymentQrToken(payload);
    const payment_jwt::PaymentQrVerifyResult result = jwt.verifyPaymentQrToken(token);

    ASSERT_TRUE(result.valid) << result.error_message;
    EXPECT_EQ(result.payload.payment_session_id, payload.payment_session_id);
    EXPECT_GT(result.payload.exp, result.payload.iat);
}

// review
TEST(JwtServiceTest, RejectsTamperedToken) {
    payment_jwt::JwtService jwt = payment_jwt::JwtService::fromConfig();
    payment_jwt::PaymentQrPayload payload;
    payload.payment_session_id = "PS20260708000000000002";

    std::string token = jwt.issuePaymentQrToken(payload);
    token.back() = token.back() == 'a' ? 'b' : 'a';

    const payment_jwt::PaymentQrVerifyResult result = jwt.verifyPaymentQrToken(token);
    EXPECT_FALSE(result.valid);
    EXPECT_FALSE(result.error_message.empty());
}

// review
TEST(JwtServiceTest, RejectsExpiredToken) {
    payment_config::JwtConfig jwt_cfg = payment_config::Config::instance().jwt();
    // 用例自管 TTL / 时钟偏差，避免被 .env 里较长 ttl 或 clock_skew 拖死
    jwt_cfg.qr_token_ttl_seconds = 1;
    jwt_cfg.clock_skew_seconds = 0;

    payment_jwt::JwtService jwt = payment_jwt::JwtService::fromJwtConfig(jwt_cfg);
    payment_jwt::PaymentQrPayload payload;
    payload.payment_session_id = "PS20260708000000000003";

    const std::string token = jwt.issuePaymentQrToken(payload);
    std::this_thread::sleep_for(std::chrono::seconds(2));

    const payment_jwt::PaymentQrVerifyResult result = jwt.verifyPaymentQrToken(token);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_message, "支付二维码 Token 已过期");
}

// review
TEST(JwtServiceTest, RejectsMalformedToken) {
    payment_jwt::JwtService jwt = payment_jwt::JwtService::fromConfig();
    const payment_jwt::PaymentQrVerifyResult result = jwt.verifyPaymentQrToken("not-a-jwt");
    EXPECT_FALSE(result.valid);
}

// review
TEST(HashServiceTest, EmailHashStableAndValidDigest) {
    const payment_jwt::HashService hash = payment_jwt::HashService::fromConfig();
    const std::string email = "User.Test@Example.COM";

    const std::string a = hash.hashEmail(email);
    const std::string b = hash.hashEmail(email);

    EXPECT_TRUE(isSha256HexDigest(a));
    EXPECT_EQ(a, b);
    EXPECT_NE(a, hash.hashEmail("other@example.com"));
}

// review
TEST(HashServiceTest, IdCardHashStableAndValidDigest) {
    const payment_jwt::HashService hash = payment_jwt::HashService::fromConfig();
    const std::string id_card = "440300199001011234";

    const std::string a = hash.hashIdCard(id_card);
    const std::string b = hash.hashIdCard(id_card);

    EXPECT_TRUE(isSha256HexDigest(a));
    EXPECT_EQ(a, b);
    EXPECT_NE(a, hash.hashIdCard("440300199001019999"));
}
