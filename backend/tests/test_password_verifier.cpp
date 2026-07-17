// review
#include <cctype>
#include <gtest/gtest.h>

#include "password_verifier.hpp"

/*
测试：
1. 正确密码：hash 后再 verify 应通过
2. 错误密码：verify 应失败
3. 非法入参：空密码 / 空 hash / 空 secret / hash 长度不是 64 → verify 应失败
4. hashLoginPassword：空密码或空 secret → 返回空串；正常输入 → 得到 64 位 hex，且同输入同结果
*/

namespace {

constexpr const char* kSecret = "test_password_hash_secret";
constexpr const char* kPassword = "PayPass123";

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
TEST(PasswordVerifierTest, AcceptsCorrectPassword) {
    const std::string hash = payment_security::PasswordVerifier::hashLoginPassword(kPassword, kSecret);
    EXPECT_TRUE(payment_security::PasswordVerifier::verifyLoginPasswordWithHash(kPassword, hash, kSecret));
}

// review
TEST(PasswordVerifierTest, RejectsWrongPassword) {
    const std::string hash = payment_security::PasswordVerifier::hashLoginPassword(kPassword, kSecret);
    EXPECT_FALSE(payment_security::PasswordVerifier::verifyLoginPasswordWithHash("WrongPass", hash, kSecret));
}

// review
TEST(PasswordVerifierTest, RejectsInvalidVerifyInputs) {
    const std::string hash = payment_security::PasswordVerifier::hashLoginPassword(kPassword, kSecret);

    EXPECT_FALSE(payment_security::PasswordVerifier::verifyLoginPasswordWithHash("", hash, kSecret));
    EXPECT_FALSE(payment_security::PasswordVerifier::verifyLoginPasswordWithHash(kPassword, "", kSecret));
    EXPECT_FALSE(payment_security::PasswordVerifier::verifyLoginPasswordWithHash(kPassword, hash, ""));
    EXPECT_FALSE(payment_security::PasswordVerifier::verifyLoginPasswordWithHash(kPassword, "abc", kSecret));
}

// review
TEST(PasswordVerifierTest, HashLoginPasswordBehavior) {
    EXPECT_TRUE(payment_security::PasswordVerifier::hashLoginPassword("", kSecret).empty());
    EXPECT_TRUE(payment_security::PasswordVerifier::hashLoginPassword(kPassword, "").empty());

    const std::string a = payment_security::PasswordVerifier::hashLoginPassword(kPassword, kSecret);
    const std::string b = payment_security::PasswordVerifier::hashLoginPassword(kPassword, kSecret);
    ASSERT_TRUE(isSha256HexDigest(a));
    EXPECT_EQ(a, b);
}
