// review
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "redis_pool.hpp"

/*
测试：
1. 正常写入 → 可读回 → 删除后映射不存在
2. 不存在的 session → 返回 nullopt
3. TTL 到期 → 映射自动失效
4. setEx TTL ≤ 0 → 抛 RedisException
5. 删除不存在的 key；setEx/get 通用 KV 读写
6. 重复 saveSession → 覆盖为最新 user_id
7. acquire / ConnectionGuard 释放后可再次获取连接
*/

class RedisSessionTest : public ::testing::Test {
  protected:
    // review
    void SetUp() override {
        try {
            payment_redis::RedisPool::instance().initFromConfig();
        } catch (const std::exception& e) {
            GTEST_SKIP() << "Redis 不可用: " << e.what();
        }
    }

    // review
    void TearDown() override {
        auto& redis = payment_redis::RedisPool::instance();
        for (const auto& session_id : session_ids_) {
            redis.removeSession(session_id);
        }
    }

    std::vector<std::string> session_ids_;
};

// review
TEST_F(RedisSessionTest, SaveReadAndRemoveSession) {
    const std::string session_id = "SIDTESTSAVE01";
    const std::string user_id = "USRTESTSAVE01";
    session_ids_.push_back(session_id);

    auto& redis = payment_redis::RedisPool::instance();
    ASSERT_TRUE(redis.saveSession(session_id, user_id, 60));

    const auto stored = redis.userIdOfSession(session_id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(*stored, user_id);

    ASSERT_TRUE(redis.removeSession(session_id));
    EXPECT_FALSE(redis.userIdOfSession(session_id).has_value());
}

// review
TEST_F(RedisSessionTest, MissingSessionReturnsNullopt) {
    EXPECT_FALSE(payment_redis::RedisPool::instance().userIdOfSession("SID_NOT_EXISTS_XYZ").has_value());
}

// review
TEST_F(RedisSessionTest, SessionExpiresAfterTtl) {
    const std::string session_id = "SIDTESTTTL01";
    const std::string user_id = "USRTESTTTL01";
    session_ids_.push_back(session_id);

    auto& redis = payment_redis::RedisPool::instance();
    ASSERT_TRUE(redis.saveSession(session_id, user_id, 1));
    ASSERT_TRUE(redis.userIdOfSession(session_id).has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    EXPECT_FALSE(redis.userIdOfSession(session_id).has_value());
}

// review
TEST_F(RedisSessionTest, SetExRejectsNonPositiveTtl) {
    auto& redis = payment_redis::RedisPool::instance();
    const std::string key = payment_redis::sessionKey("SIDTESTTTLBAD");

    // 参数校验在连 Redis 之前抛出，不会写入 key
    EXPECT_THROW(redis.setEx(key, "v", 0), payment_redis::RedisException);
    EXPECT_THROW(redis.setEx(key, "v", -1), payment_redis::RedisException);
}

// review
TEST_F(RedisSessionTest, DelMissingKeyAndSetExGet) {
    auto& redis = payment_redis::RedisPool::instance();
    const std::string session_id = "SIDTESTKV01";
    const std::string key = payment_redis::sessionKey(session_id);
    session_ids_.push_back(session_id);

    EXPECT_FALSE(redis.get(key).has_value());
    ASSERT_TRUE(redis.del(key)); // 删除不存在的 key：DEL 返回 0，仍视为成功

    ASSERT_TRUE(redis.setEx(key, "present", 60));
    const auto stored = redis.get(key);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(*stored, "present");

    ASSERT_TRUE(redis.del(key));
    EXPECT_FALSE(redis.get(key).has_value());
}

// review
TEST_F(RedisSessionTest, SaveSessionOverwritesUserId) {
    const std::string session_id = "SIDTESTOVR01";
    session_ids_.push_back(session_id);

    auto& redis = payment_redis::RedisPool::instance();
    ASSERT_TRUE(redis.saveSession(session_id, "USR_OLD", 60));
    ASSERT_TRUE(redis.saveSession(session_id, "USR_NEW", 60));

    const auto stored = redis.userIdOfSession(session_id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(*stored, "USR_NEW");
}

// review
TEST_F(RedisSessionTest, AcquireAndReleaseConnection) {
    auto& redis = payment_redis::RedisPool::instance();

    {
        auto first = redis.acquire();
        ASSERT_TRUE(static_cast<bool>(first));
    }

    // ConnectionGuard 析构后连接应已归还，可再次获取（不依赖池大小 ≥ 2）
    auto again = redis.acquire();
    ASSERT_TRUE(static_cast<bool>(again));
}
