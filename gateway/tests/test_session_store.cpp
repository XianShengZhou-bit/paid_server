#include <string>

#include <gtest/gtest.h>

#include "session_store.hpp"
#include "test_support.hpp"

/*
测试：
1. 存在的 session → 读回 user_id
2. 不存在的 session → nullopt
*/

class GatewaySessionStoreTest : public ::testing::Test {
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
TEST_F(GatewaySessionStoreTest, UserIdOfExistingSession) {
    session_id_ = "SIDGWTESTREAD01";
    const std::string user_id = "USRGWTESTREAD01";
    try {
        gateway_test::writeSessionAsBackend(session_id_, user_id);
    } catch (const std::exception& e) {
        GTEST_SKIP() << "无法用 backend ACL 写入 Redis: " << e.what();
    }

    const auto stored = gateway_session::SessionStore::instance().userIdOf(session_id_);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(*stored, user_id);
}

// review
TEST_F(GatewaySessionStoreTest, UserIdOfMissingSessionReturnsNullopt) {
    EXPECT_FALSE(gateway_session::SessionStore::instance().userIdOf("SID_NOT_EXISTS_GW_XYZ").has_value());
}
