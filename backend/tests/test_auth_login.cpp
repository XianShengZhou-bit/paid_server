// review
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <mysql/mysql.h>
#include <nlohmann/json.hpp>

#include "config.hpp"
#include "mysql_pool.hpp"
#include "password_verifier.hpp"
#include "payment_impl.hpp"
#include "redis_pool.hpp"

using json = nlohmann::json;

/*
测试：
1. 正常登录 → Redis 有会话 + Cookie；登出 → 会话清除 + Cookie 清空
2. 密码错误 → 40104
3. 未知用户 → 40403
4. 用户冻结/非 Normal → 40403
5. 非法 JSON → 40001
6. 缺 username 或 password → 40001
7. 无 Cookie 登出 → 仍 200 且清空 Cookie（幂等）
*/

namespace {

// review
payment_impl::HttpRequest makeJsonRequest(const std::string& method, const std::string& path, const json& body) {
    payment_impl::HttpRequest req;
    req.method = method;
    req.path = path;
    req.headers["Content-Type"] = "application/json";
    req.body = body.dump();
    return req;
}

// review
void execSql(const std::string& sql) {
    auto guard = payment_mysql::MySqlPool::instance().acquire();
    MYSQL* conn = guard.get();
    if (mysql_query(conn, sql.c_str()) != 0) {
        throw std::runtime_error(std::string("mysql_query failed: ") + mysql_error(conn));
    }
}

// review
std::string seedLoginUser(const std::string& suffix, const std::string& plain_password,
                          int status = static_cast<int>(payment_mysql::UserStatus::Normal)) {
    const std::string user_id = "USRLOGIN" + suffix;
    const std::string username = "loginuser" + suffix;
    const std::string password_hash = payment_security::PasswordVerifier::hashLoginPassword(
        plain_password, payment_config::Config::instance().passwordHash().secret);

    execSql("DELETE FROM users WHERE user_id='" + user_id + "' OR username='" + username + "'");
    execSql("INSERT INTO users (user_id, username, password_hash, email_hash, email_masked, id_card_hash, "
            "id_card_masked, real_auth_completed, user_type, status, last_login_at) VALUES ('" +
            user_id + "','" + username + "','" + password_hash +
            "','aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','tes****@example.com',"
            "'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb','440300********1234',"
            "1,0," + std::to_string(status) + ",NOW())");
    return user_id;
}

// review
void cleanupLoginUser(const std::string& suffix) {
    execSql("DELETE FROM users WHERE user_id='USRLOGIN" + suffix + "' OR username='loginuser" + suffix + "'");
}

// review
std::string cookieValue(const std::vector<std::pair<std::string, std::string>>& headers, const std::string& name) {
    const std::string prefix = name + "=";
    for (const auto& h : headers) {
        if (h.first != "Set-Cookie" || h.second.rfind(prefix, 0) != 0) {
            continue;
        }
        const std::size_t end = h.second.find(';', prefix.size());
        return h.second.substr(prefix.size(), end == std::string::npos ? std::string::npos : end - prefix.size());
    }
    return "";
}

} // namespace

class AuthLoginTest : public ::testing::Test {
  protected:
    // review
    void SetUp() override {
        try {
            payment_mysql::MySqlPool::instance().initFromConfig();
            payment_redis::RedisPool::instance().initFromConfig();
        } catch (const std::exception& e) {
            GTEST_SKIP() << "登录集成测试需要可用的 MySQL 与 Redis: " << e.what();
        }
    }

    // review
    void TearDown() override {
        if (!session_id_.empty()) {
            payment_redis::RedisPool::instance().removeSession(session_id_);
        }
        if (!suffix_.empty()) {
            cleanupLoginUser(suffix_);
        }
    }

    std::string suffix_;
    std::string session_id_;
    std::string user_id_;
    std::string username_;
    std::string password_;
};

// review
TEST_F(AuthLoginTest, LoginAndLogoutSucceed) {
    suffix_ = "AUTH01";
    username_ = "loginuser" + suffix_;
    password_ = "secret-" + suffix_;
    user_id_ = seedLoginUser(suffix_, password_);

    const auto login_req =
        makeJsonRequest("POST", "/api/auth/login", json{{"username", username_}, {"password", password_}});
    const payment_impl::HttpResponse login_resp = payment_impl::handleRequest(login_req);

    EXPECT_EQ(login_resp.http_status, 200);
    EXPECT_EQ(login_resp.envelope.value("code", -1), 0);
    EXPECT_EQ(login_resp.envelope["data"].value("user_id", ""), user_id_);

    session_id_ = cookieValue(login_resp.extra_headers, "session_id");
    ASSERT_FALSE(session_id_.empty());
    EXPECT_EQ(session_id_.rfind("SID", 0), 0);

    const auto stored = payment_redis::RedisPool::instance().userIdOfSession(session_id_);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(*stored, user_id_);

    payment_impl::HttpRequest logout_req;
    logout_req.method = "POST";
    logout_req.path = "/api/auth/logout";
    logout_req.headers["Cookie"] = "session_id=" + session_id_;
    const payment_impl::HttpResponse logout_resp = payment_impl::handleRequest(logout_req);

    EXPECT_EQ(logout_resp.http_status, 200);
    EXPECT_TRUE(cookieValue(logout_resp.extra_headers, "session_id").empty());
    EXPECT_FALSE(payment_redis::RedisPool::instance().userIdOfSession(session_id_).has_value());
    session_id_.clear();
}

// review
TEST_F(AuthLoginTest, LoginRejectsWrongPassword) {
    suffix_ = "AUTH02";
    username_ = "loginuser" + suffix_;
    password_ = "secret-" + suffix_;
    user_id_ = seedLoginUser(suffix_, password_);

    const auto req =
        makeJsonRequest("POST", "/api/auth/login", json{{"username", username_}, {"password", "wrong-password"}});
    const payment_impl::HttpResponse response = payment_impl::handleLogin(req);

    EXPECT_EQ(response.http_status, 401);
    EXPECT_EQ(response.envelope.value("code", -1), 40104);
    EXPECT_TRUE(cookieValue(response.extra_headers, "session_id").empty());
}

// review
TEST_F(AuthLoginTest, LoginRejectsUnknownUser) {
    const auto req =
        makeJsonRequest("POST", "/api/auth/login", json{{"username", "no_such_user_xyz"}, {"password", "whatever"}});
    const payment_impl::HttpResponse response = payment_impl::handleLogin(req);

    EXPECT_EQ(response.http_status, 404);
    EXPECT_EQ(response.envelope.value("code", -1), 40403);
}

// review
TEST_F(AuthLoginTest, LoginRejectsFrozenUser) {
    suffix_ = "AUTH03";
    username_ = "loginuser" + suffix_;
    password_ = "secret-" + suffix_;
    user_id_ = seedLoginUser(suffix_, password_, static_cast<int>(payment_mysql::UserStatus::Frozen));

    const auto req =
        makeJsonRequest("POST", "/api/auth/login", json{{"username", username_}, {"password", password_}});
    const payment_impl::HttpResponse response = payment_impl::handleLogin(req);

    EXPECT_EQ(response.http_status, 404);
    EXPECT_EQ(response.envelope.value("code", -1), 40403);
    EXPECT_TRUE(cookieValue(response.extra_headers, "session_id").empty());
}

// review
TEST_F(AuthLoginTest, LoginRejectsInvalidJson) {
    payment_impl::HttpRequest req;
    req.method = "POST";
    req.path = "/api/auth/login";
    req.headers["Content-Type"] = "application/json";
    req.body = "{not-json";

    const payment_impl::HttpResponse response = payment_impl::handleLogin(req);
    EXPECT_EQ(response.http_status, 400);
    EXPECT_EQ(response.envelope.value("code", -1), 40001);
}

// review
TEST_F(AuthLoginTest, LoginRejectsEmptyCredentials) {
    const auto missing_password =
        makeJsonRequest("POST", "/api/auth/login", json{{"username", "someone"}, {"password", ""}});
    const payment_impl::HttpResponse resp_empty_password = payment_impl::handleLogin(missing_password);
    EXPECT_EQ(resp_empty_password.http_status, 400);
    EXPECT_EQ(resp_empty_password.envelope.value("code", -1), 40001);

    const auto missing_username =
        makeJsonRequest("POST", "/api/auth/login", json{{"username", ""}, {"password", "x"}});
    const payment_impl::HttpResponse resp_empty_username = payment_impl::handleLogin(missing_username);
    EXPECT_EQ(resp_empty_username.http_status, 400);
    EXPECT_EQ(resp_empty_username.envelope.value("code", -1), 40001);
}

// review
TEST_F(AuthLoginTest, LogoutWithoutCookieSucceeds) {
    payment_impl::HttpRequest logout_req;
    logout_req.method = "POST";
    logout_req.path = "/api/auth/logout";
    const payment_impl::HttpResponse logout_resp = payment_impl::handleLogout(logout_req);

    EXPECT_EQ(logout_resp.http_status, 200);
    EXPECT_EQ(logout_resp.envelope.value("code", -1), 0);
    EXPECT_TRUE(cookieValue(logout_resp.extra_headers, "session_id").empty());
}
