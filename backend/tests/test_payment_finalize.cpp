// review
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <mysql/mysql.h>

#include "mysql_pool.hpp"

/*
测试：MySqlPool::finalizePaymentSuccess
1. 支付成功：订单已付、账号已售、支付会话已付、attempt 成功
2. 账号 Trading 状态也可支付成功
3. 同一 request_id 在会话已付后再次提交 → 失败
4. 已付后再换新 request_id 支付 → 失败（会话已 Paid）
5. 订单已付但会话仍 WaitingPay → ORDER_ALREADY_PAID
6. 会话绑定不一致 → SESSION_BINDING_MISMATCH
7. 会话不可支付（非 WaitingPay）→ SESSION_NOT_PAYABLE
8. 订单快照不一致（价格）→ ORDER_SNAPSHOT_MISMATCH
9. 账号不可交易（已售）→ ACCOUNT_NOT_TRADABLE
10. 买家未实名 → USER_REAL_AUTH_INVALID
11. attempt 已是 Failed → 直接失败
*/

namespace {

// review
void execSql(const std::string& sql) {
    auto guard = payment_mysql::MySqlPool::instance().acquire();
    MYSQL* conn = guard.get();
    if (mysql_query(conn, sql.c_str()) != 0) {
        throw std::runtime_error(std::string("mysql_query failed: ") + mysql_error(conn));
    }
}

// review
void cleanupCase(const std::string& suffix) {
    if (suffix.empty()) {
        throw std::runtime_error("cleanupCase: suffix 不能为空，否则会删掉整表数据");
    }
    execSql("DELETE FROM payment_attempts WHERE request_id LIKE '%" + suffix + "%'");
    execSql("DELETE FROM payment_sessions WHERE payment_session_id LIKE '%" + suffix + "%'");
    execSql("DELETE FROM orders WHERE order_sn LIKE '%" + suffix + "%'");
    execSql("DELETE FROM accounts WHERE account_id LIKE '%" + suffix + "%'");
    execSql("DELETE FROM users WHERE user_id LIKE '%" + suffix + "%'");
}

struct Seed {
    std::string user_id;
    std::string account_id;
    std::string order_sn;
    std::string payment_session_id;
    std::string request_id;
    int64_t price = 9900;
};

struct SeedOptions {
    int order_status = static_cast<int>(payment_mysql::OrderStatus::PendingPay);
    int account_status = static_cast<int>(payment_mysql::AccountStatus::OnSale);
    int session_status = static_cast<int>(payment_mysql::PaymentSessionStatus::WaitingPay);
    int attempt_result = static_cast<int>(payment_mysql::PaymentAttemptResult::Processing);
    int real_auth_completed = 1;
    int user_status = static_cast<int>(payment_mysql::UserStatus::Normal);
};

// review
Seed seedPending(const std::string& suffix, const SeedOptions& opt = {}) {
    Seed s;
    s.user_id = "USRTEST" + suffix;
    s.account_id = "ACCTEST" + suffix;
    s.order_sn = "ORDTEST" + suffix;
    s.payment_session_id = "PSTEST" + suffix;
    s.request_id = "REQTEST" + suffix;

    cleanupCase(suffix);
    execSql("INSERT INTO users (user_id, username, password_hash, email_hash, email_masked, id_card_hash, "
            "id_card_masked, real_auth_completed, user_type, status, last_login_at) VALUES ('" +
            s.user_id + "','testuser" + suffix +
            "','0000000000000000000000000000000000000000000000000000000000000000',"
            "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','tes****@example.com',"
            "'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb','440300********1234'," +
            std::to_string(opt.real_auth_completed) + ",0," + std::to_string(opt.user_status) + ",NOW())");
    execSql("INSERT INTO accounts (account_id, seller_id, game_name, server_area, price, account_level, "
            "hero_count, skin_count, rare_items, status, version) VALUES ('" +
            s.account_id + "','" + s.user_id + "','王者荣耀','微信区'," + std::to_string(s.price) + ",30,50,80,'[]'," +
            std::to_string(opt.account_status) + ",0)");
    execSql("INSERT INTO orders (order_sn, account_id, buyer_id, price, buyer_email, buyer_id_card_hash, "
            "buyer_id_card_masked, status) VALUES ('" +
            s.order_sn + "','" + s.account_id + "','" + s.user_id + "'," + std::to_string(s.price) +
            ",'buyer@example.com','cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc',"
            "'440300********5678'," +
            std::to_string(opt.order_status) + ")");
    execSql("INSERT INTO payment_sessions (payment_session_id, order_sn, account_id, buyer_id, buyer_email, status, "
            "expire_at) VALUES ('" +
            s.payment_session_id + "','" + s.order_sn + "','" + s.account_id + "','" + s.user_id +
            "','buyer@example.com'," + std::to_string(opt.session_status) + ", DATE_ADD(NOW(), INTERVAL 10 MINUTE))");
    execSql("INSERT INTO payment_attempts (request_id, payment_session_id, order_sn, account_id, buyer_id, result) "
            "VALUES ('" +
            s.request_id + "','" + s.payment_session_id + "','" + s.order_sn + "','" + s.account_id + "','" +
            s.user_id + "'," + std::to_string(opt.attempt_result) + ")");
    return s;
}

// review
payment_mysql::PaymentFinalizeInput toInput(const Seed& s) {
    payment_mysql::PaymentFinalizeInput input;
    input.payment_session_id = s.payment_session_id;
    input.order_sn = s.order_sn;
    input.account_id = s.account_id;
    input.buyer_id = s.user_id;
    input.price = s.price;
    input.request_id = s.request_id;
    return input;
}

// review
void expectFailedAttempt(const std::string& request_id, const std::string& fail_reason) {
    const auto attempt = payment_mysql::MySqlPool::instance().getPaymentAttemptByRequestId(request_id);
    ASSERT_TRUE(attempt.has_value());
    EXPECT_EQ(attempt->result, static_cast<int>(payment_mysql::PaymentAttemptResult::Failed));
    EXPECT_EQ(attempt->fail_reason, fail_reason);
}

} // namespace

class PaymentFinalizeTest : public ::testing::Test {
  protected:
    // review
    void SetUp() override {
        try {
            payment_mysql::MySqlPool::instance().initFromConfig();
        } catch (const std::exception& e) {
            GTEST_SKIP() << "MySQL 不可用: " << e.what();
        }
    }

    // review
    void TearDown() override {
        if (!suffix_.empty()) {
            cleanupCase(suffix_);
        }
    }

    std::string suffix_;
};

// review
TEST_F(PaymentFinalizeTest, FinalizePaymentSuccessUpdatesAllStates) {
    suffix_ = "SUCCESS01";
    const auto seed = seedPending(suffix_);
    const auto input = toInput(seed);

    auto& pool = payment_mysql::MySqlPool::instance();
    ASSERT_TRUE(pool.finalizePaymentSuccess(input));

    const auto order = pool.getOrderBySn(seed.order_sn);
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->status, static_cast<int>(payment_mysql::OrderStatus::Paid));

    const auto account = pool.getAccountById(seed.account_id);
    ASSERT_TRUE(account.has_value());
    EXPECT_EQ(account->status, static_cast<int>(payment_mysql::AccountStatus::Sold));

    const auto session = pool.getPaymentSessionById(seed.payment_session_id);
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->status, static_cast<int>(payment_mysql::PaymentSessionStatus::Paid));

    const auto attempt = pool.getPaymentAttemptByRequestId(seed.request_id);
    ASSERT_TRUE(attempt.has_value());
    EXPECT_EQ(attempt->result, static_cast<int>(payment_mysql::PaymentAttemptResult::Success));
}

// review
TEST_F(PaymentFinalizeTest, FinalizeSucceedsWhenAccountIsTrading) {
    suffix_ = "TRADING01";
    SeedOptions opt;
    opt.account_status = static_cast<int>(payment_mysql::AccountStatus::Trading);
    const auto seed = seedPending(suffix_, opt);

    ASSERT_TRUE(payment_mysql::MySqlPool::instance().finalizePaymentSuccess(toInput(seed)));
    const auto account = payment_mysql::MySqlPool::instance().getAccountById(seed.account_id);
    ASSERT_TRUE(account.has_value());
    EXPECT_EQ(account->status, static_cast<int>(payment_mysql::AccountStatus::Sold));
}

// review
TEST_F(PaymentFinalizeTest, DuplicateFinalizeWithSameRequestIdIsRejectedAfterSuccess) {
    suffix_ = "DUPPAY01";
    const auto seed = seedPending(suffix_);
    const auto input = toInput(seed);

    auto& pool = payment_mysql::MySqlPool::instance();
    ASSERT_TRUE(pool.finalizePaymentSuccess(input));
    // 会话已变为 Paid，再次提交会因 SESSION_NOT_PAYABLE 失败
    EXPECT_FALSE(pool.finalizePaymentSuccess(input));
    expectFailedAttempt(seed.request_id, "SESSION_NOT_PAYABLE");
}

// review
TEST_F(PaymentFinalizeTest, RejectsRepayAfterAlreadyPaid) {
    suffix_ = "REPAY01";
    const auto seed = seedPending(suffix_);
    const auto first = toInput(seed);

    auto& pool = payment_mysql::MySqlPool::instance();
    ASSERT_TRUE(pool.finalizePaymentSuccess(first));

    execSql("INSERT INTO payment_attempts (request_id, payment_session_id, order_sn, account_id, buyer_id, result) "
            "VALUES ('REQTEST" +
            suffix_ + "B','" + seed.payment_session_id + "','" + seed.order_sn + "','" + seed.account_id + "','" +
            seed.user_id + "',0)");

    payment_mysql::PaymentFinalizeInput second = first;
    second.request_id = "REQTEST" + suffix_ + "B";
    EXPECT_FALSE(pool.finalizePaymentSuccess(second));
    expectFailedAttempt(second.request_id, "SESSION_NOT_PAYABLE");
}

// review
TEST_F(PaymentFinalizeTest, RejectsOrderAlreadyPaid) {
    suffix_ = "ORDPAID01";
    SeedOptions opt;
    opt.order_status = static_cast<int>(payment_mysql::OrderStatus::Paid);
    opt.session_status = static_cast<int>(payment_mysql::PaymentSessionStatus::WaitingPay);
    const auto seed = seedPending(suffix_, opt);

    EXPECT_FALSE(payment_mysql::MySqlPool::instance().finalizePaymentSuccess(toInput(seed)));
    expectFailedAttempt(seed.request_id, "ORDER_ALREADY_PAID");
}

// review
TEST_F(PaymentFinalizeTest, RejectsSessionBindingMismatch) {
    suffix_ = "BINDMIS01";
    const auto seed = seedPending(suffix_);
    auto input = toInput(seed);
    input.order_sn = "ORD_WRONG_BINDING";

    EXPECT_FALSE(payment_mysql::MySqlPool::instance().finalizePaymentSuccess(input));
    expectFailedAttempt(seed.request_id, "SESSION_BINDING_MISMATCH");
}

// review
TEST_F(PaymentFinalizeTest, RejectsSessionNotPayable) {
    suffix_ = "SESSBAD01";
    SeedOptions opt;
    opt.session_status = static_cast<int>(payment_mysql::PaymentSessionStatus::Expired);
    const auto seed = seedPending(suffix_, opt);

    EXPECT_FALSE(payment_mysql::MySqlPool::instance().finalizePaymentSuccess(toInput(seed)));
    expectFailedAttempt(seed.request_id, "SESSION_NOT_PAYABLE");
}

// review
TEST_F(PaymentFinalizeTest, RejectsOrderSnapshotMismatch) {
    suffix_ = "PRICEMIS01";
    const auto seed = seedPending(suffix_);
    auto input = toInput(seed);
    input.price = seed.price + 1;

    EXPECT_FALSE(payment_mysql::MySqlPool::instance().finalizePaymentSuccess(input));
    expectFailedAttempt(seed.request_id, "ORDER_SNAPSHOT_MISMATCH");
}

// review
TEST_F(PaymentFinalizeTest, RejectsAccountNotTradable) {
    suffix_ = "ACCTSOLD01";
    SeedOptions opt;
    opt.account_status = static_cast<int>(payment_mysql::AccountStatus::Sold);
    const auto seed = seedPending(suffix_, opt);

    EXPECT_FALSE(payment_mysql::MySqlPool::instance().finalizePaymentSuccess(toInput(seed)));
    expectFailedAttempt(seed.request_id, "ACCOUNT_NOT_TRADABLE");
}

// review
TEST_F(PaymentFinalizeTest, RejectsUserWithoutRealAuth) {
    suffix_ = "NOAUTH01";
    SeedOptions opt;
    opt.real_auth_completed = 0;
    const auto seed = seedPending(suffix_, opt);

    EXPECT_FALSE(payment_mysql::MySqlPool::instance().finalizePaymentSuccess(toInput(seed)));
    expectFailedAttempt(seed.request_id, "USER_REAL_AUTH_INVALID");
}

// review
TEST_F(PaymentFinalizeTest, RejectsAlreadyFailedAttempt) {
    suffix_ = "ATTEMPTFAIL01";
    SeedOptions opt;
    opt.attempt_result = static_cast<int>(payment_mysql::PaymentAttemptResult::Failed);
    const auto seed = seedPending(suffix_, opt);

    EXPECT_FALSE(payment_mysql::MySqlPool::instance().finalizePaymentSuccess(toInput(seed)));
    const auto attempt = payment_mysql::MySqlPool::instance().getPaymentAttemptByRequestId(seed.request_id);
    ASSERT_TRUE(attempt.has_value());
    EXPECT_EQ(attempt->result, static_cast<int>(payment_mysql::PaymentAttemptResult::Failed));
}
