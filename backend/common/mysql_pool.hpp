// review
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <mysql/mysql.h>

#include "config.hpp"
#include "logger.hpp"

namespace payment_mysql {

class MySqlException : public std::runtime_error {
  public:
    // review
    explicit MySqlException(const std::string& message) : std::runtime_error(message) {}
};

// 用户状态枚举
// 对应 users.status 字段，用于表示用户账号当前是否允许正常参与登录、下单、支付、上架等业务流程。
// 数据库中以 TINYINT/INT 形式保存，C++ 代码中通过该强类型枚举避免直接使用 0、1、2、3 等数字。
enum class UserStatus : int {
    Normal = 0,     // 正常：用户账号可正常使用
    Frozen = 1,     // 冻结：用户账号被冻结，禁止或限制核心业务操作
    Cancelled = 2,  // 注销：用户账号已注销，不允许继续参与业务流程
    RiskLimited = 3 // 风控限制：用户触发风控策略，限制支付、下单、上架等高风险操作
};

// 订单状态枚举
// 对应 orders.status 字段，用于表示交易订单在支付流程中的当前状态。
// 支付服务主要根据该状态判断订单是否仍可支付，并在支付成功事务中将订单从待支付更新为已支付。
enum class OrderStatus : int {
    PendingPay = 0, // 待支付：订单已创建，但尚未完成支付
    Paid = 1,       // 已支付：订单已经完成支付处理
    Cancelled = 2,  // 已取消：订单已被业务系统或用户取消，禁止继续支付
    Expired = 3     // 已过期：订单超过允许支付时间，禁止继续支付
};

// 游戏账号商品状态枚举
// 对应 accounts.status 字段，用于表示游戏账号商品当前是否可交易。
// 支付成功后，支付服务应在同一事务中将账号状态更新为已售出，防止账号被重复购买。
enum class AccountStatus : int {
    OnSale = 0,  // 在售：账号当前可被购买
    Trading = 1, // 交易中：账号已被订单锁定，正在支付或交易处理中
    Sold = 2,    // 已售出：账号已完成交易，不允许再次购买
    Removed = 3  // 已下架：账号被卖家或平台下架，不允许购买
};

// 支付会话状态枚举
// 对应 payment_sessions.status 字段，用于表示页面 A、二维码、页面 B、WebSocket 推送之间的支付会话状态。
// 支付服务通过该状态判断二维码是否仍有效、会话是否已完成支付、是否已失效或过期。
enum class PaymentSessionStatus : int {
    WaitingPay = 0, // 待支付：支付会话已创建，二维码仍可用于支付
    Paid = 1,       // 已支付：该支付会话已完成支付
    Invalid = 2,    // 已失效：会话因订单变化、重新生成、主动废弃等原因失效
    Expired = 3     // 已过期：支付会话超过 expire_at，禁止继续支付
};

// 支付尝试结果枚举
// 对应 payment_attempts.result 字段，用于记录一次支付请求 request_id 的处理结果。
// 该枚举主要服务于支付幂等控制：同一个 request_id 重复提交时，可根据结果直接返回处理中、成功或失败。
enum class PaymentAttemptResult : int {
    Processing = 0, // 处理中：支付请求已创建或正在处理，暂未得到最终结果
    Success = 1,    // 成功：支付请求已经处理成功
    Failed = 2      // 失败：支付请求已经处理失败，失败原因应写入 fail_reason
};

struct UserInfo {
    int64_t id = 0;
    std::string user_id;
    std::string username;
    std::string password_hash; // 仅服务端支付密码校验使用，禁止返回给前端
    std::string email_hash;
    std::string email_masked;
    std::string id_card_hash;
    std::string id_card_masked;
    int real_auth_completed = 0;
    int user_type = 0;
    int status = 0;
    std::string last_login_at;
    std::string created_at;

    bool isNormal() const {
        return status == static_cast<int>(UserStatus::Normal);
    }

    // 判断当前用户是否已经完成支付前置实名认证。
    bool isRealAuthCompleted() const {
        return real_auth_completed == 1 && !email_hash.empty() && !id_card_hash.empty();
    }
};

struct OrderInfo {
    int64_t id = 0;
    std::string order_sn;
    std::string account_id;
    std::string buyer_id;
    int64_t price = 0; // 单位：分
    std::string buyer_email;
    std::string buyer_id_card_hash;
    std::string buyer_id_card_masked;
    int status = 0;
    std::string create_time;
    std::string paid_at;
    std::string update_time;
};

struct AccountInfo {
    int64_t id = 0;
    std::string account_id;
    std::string seller_id;
    std::string game_name;
    std::string server_area;
    int64_t price = 0; // 单位：分
    int account_level = 0;
    int hero_count = 0;
    int skin_count = 0;
    std::string rare_items;
    int status = 0;
    int version = 0;
    std::string created_at;
    std::string updated_at;
};

struct PaymentSessionInfo {
    int64_t id = 0;
    std::string payment_session_id;
    std::string order_sn;
    std::string account_id;
    std::string buyer_id;
    std::string buyer_email;
    int status = 0;
    std::string expire_at;
    std::string created_at;
    std::string updated_at;
};

struct PaymentAttemptInfo {
    int64_t id = 0;
    std::string request_id;
    std::string payment_session_id;
    std::string order_sn;
    std::string account_id;
    std::string buyer_id;
    int result = 0;
    std::string fail_reason;
    std::string client_ip;
    std::string user_agent;
    std::string created_at;
    std::string updated_at;
};

struct CreatePaymentSessionInput {
    std::string payment_session_id;
    std::string order_sn;
    std::string account_id;
    std::string buyer_id;
    std::string buyer_contact_masked;
    int ttl_seconds = 300;
};

struct UserRealAuthUpdate {
    std::string user_id;
    std::string email_hash;
    std::string email_masked;
    std::string id_card_hash;
    std::string id_card_masked;
};

struct CreatePaymentAttemptInput {
    std::string request_id;
    std::string payment_session_id;
    std::string order_sn;
    std::string account_id;
    std::string buyer_id;
    std::string client_ip;
    std::string user_agent;
};

struct PaymentFinalizeInput {
    std::string payment_session_id;
    std::string order_sn;
    std::string account_id;
    std::string buyer_id;
    int64_t price = 0;
    std::string request_id;
};

class MySqlPool {
  public:
    // review
    // ConnectionGuard 是一个连接守卫对象(防止连接借出没归还)
    class ConnectionGuard {
      public:
        // review
        ConnectionGuard(MySqlPool* pool, MYSQL* conn) : pool_(pool), conn_(conn) {}
        // review
        ~ConnectionGuard() {
            reset();
        }

        // review
        // 移动构造
        ConnectionGuard(ConnectionGuard&& other) noexcept : pool_(other.pool_), conn_(other.conn_) {
            other.pool_ = nullptr;
            other.conn_ = nullptr;
        }

        // review
        // 移动赋值
        ConnectionGuard& operator=(ConnectionGuard&& other) noexcept {
            if (this != &other) { // 防止自赋值
                reset();
                pool_ = other.pool_;
                conn_ = other.conn_;
                other.pool_ = nullptr;
                other.conn_ = nullptr;
            }
            return *this;
        }

        // review
        MYSQL* get() const {
            return conn_;
        }

        // review
        // 允许 ConnectionGuard 在需要判断真假的地方被当成 bool 用，但不允许它随便隐式转换成数字或其他类型。
        explicit operator bool() const {
            return conn_ != nullptr;
        }

      private:
        // review
        void reset() {
            if (pool_ != nullptr && conn_ != nullptr) {
                pool_->release(conn_);
            }
            pool_ = nullptr;
            conn_ = nullptr;
        }

        // review
        ConnectionGuard(const ConnectionGuard&) = delete;
        // review
        ConnectionGuard& operator=(const ConnectionGuard&) = delete;

        MySqlPool* pool_ = nullptr;
        MYSQL* conn_ = nullptr;
    };

    // review
    static MySqlPool& instance() {
        static MySqlPool pool;
        return pool;
    }

    // review
    void init(const payment_config::MySqlConfig& cfg) {
        if (initialized_) {
            return;
        }
        cfg_ = cfg;
        shutting_down_ = false;
        for (int i = 0; i < cfg_.pool_size; ++i) {
            connections_.push(createConnection());
        }
        initialized_ = true;
        LOG_INFO("mysql pool initialized, host={}, database={}, pool_size={}", cfg_.host, cfg_.database,
                 cfg_.pool_size);
    }

    // review
    void initFromConfig(const payment_config::Config& config = payment_config::Config::instance()) {
        init(config.mysql());
    }

    // review
    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_ && connections_.empty()) {
            return;
        }
        shutting_down_ = true;
        while (!connections_.empty()) {
            MYSQL* conn = connections_.front();
            connections_.pop();
            if (conn != nullptr) {
                mysql_close(conn);
            }
        }
        initialized_ = false;
        cv_.notify_all(); // TODO，这是唤醒的谁？在哪里睡眠的线程？
    }

    // review
    // 从 MySQL 连接池中获取一个可用的空闲连接。
    ConnectionGuard acquire(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!initialized_) {
            throw MySqlException("mysql 连接池尚未初始化");
        }

        const bool ok = cv_.wait_for(lock, timeout, [&] { return shutting_down_ || !connections_.empty(); });
        if (!ok || shutting_down_) {
            throw MySqlException("获取 mysql 连接超时");
        }

        MYSQL* conn = connections_.front();
        connections_.pop();
        if (mysql_ping(conn) !=
            0) { // 如果连接不可用，则关闭连接并创建新的连接，并在归还的时候将新创建的连接放回到连接池中
            mysql_close(conn);
            conn = createConnection();
        }
        return ConnectionGuard(this, conn);
    }
    
    // review
    // 通过用户ID获取用户信息
    std::optional<UserInfo> getUserByUserId(const std::string& user_id) {
        auto guard = acquire();

        const std::string sql =
            userSelectColumns() + " FROM users WHERE user_id='" + escape(guard.get(), user_id) + "' LIMIT 1";

        auto rows = queryRows(guard.get(), sql);
        if (rows.empty()) {
            return std::nullopt;
        }

        return parseUser(rows[0]);
    }

    // 通过用户名获取用户信息，供后端登录接口校验账号密码。
    std::optional<UserInfo> getUserByUsername(const std::string& username) {
        auto guard = acquire();

        const std::string sql =
            userSelectColumns() + " FROM users WHERE username='" + escape(guard.get(), username) + "' LIMIT 1";
        auto rows = queryRows(guard.get(), sql);
        if (rows.empty()) {
            return std::nullopt;
        }
        return parseUser(rows[0]);
    }

    // review
    // 判断用户是否已完成邮箱号和身份证号实名认证
    bool isUserRealAuthCompleted(const std::string& user_id) {
        const auto user = getUserByUserId(user_id);
        return user.has_value() && user->isRealAuthCompleted();
    }

    // review
    // 更新用户实名认证信息（邮箱哈希、脱敏邮箱、身份证哈希、脱敏身份证）
    bool updateUserRealAuthProfile(const UserRealAuthUpdate& input) {
        if (input.user_id.empty() || input.email_hash.empty() || input.email_masked.empty() ||
            input.id_card_hash.empty() || input.id_card_masked.empty()) {
            return false;
        }
        auto guard = acquire();
        const std::string sql =
            "UPDATE users SET email_hash='" + escape(guard.get(), input.email_hash) + "', email_masked='" +
            escape(guard.get(), input.email_masked) + "', id_card_hash='" +
            escape(guard.get(), input.id_card_hash) + "', id_card_masked='" +
            escape(guard.get(), input.id_card_masked) + "', real_auth_completed=1 WHERE user_id='" +
            escape(guard.get(), input.user_id) + "'";
        return executeAffected(guard.get(), sql) == 1;
    }

    // review
    // 通过订单号获取订单信息
    std::optional<OrderInfo> getOrderBySn(const std::string& order_sn) {
        auto guard = acquire();

        const std::string sql =
            orderSelectColumns() + " FROM orders WHERE order_sn='" + escape(guard.get(), order_sn) + "' LIMIT 1";

        auto rows = queryRows(guard.get(), sql);
        if (rows.empty()) {
            return std::nullopt;
        }

        return parseOrder(rows[0]);
    }

    // review
    // 通过账号ID获取账号信息
    std::optional<AccountInfo> getAccountById(const std::string& account_id) {
        auto guard = acquire();

        const std::string sql = accountSelectColumns() + " FROM accounts WHERE account_id='" +
                                escape(guard.get(), account_id) + "' LIMIT 1";

        auto rows = queryRows(guard.get(), sql);
        if (rows.empty()) {
            return std::nullopt;
        }

        return parseAccount(rows[0]);
    }

    // review
    // 通过订单号获取支付会话信息，如果存在，就复用这个支付会话；如果不存在，就返回
    // std::nullopt，后续再创建新的支付会话。
    std::optional<PaymentSessionInfo> getActivePaymentSessionByOrderSn(const std::string& order_sn) {
        auto guard = acquire();
        const std::string sql = paymentSessionSelectColumns() + " FROM payment_sessions WHERE order_sn='" +
                                escape(guard.get(), order_sn) +
                                "' AND status=" + std::to_string(static_cast<int>(PaymentSessionStatus::WaitingPay)) +
                                " AND expire_at > NOW() LIMIT 1";
        auto rows = queryRows(guard.get(), sql);
        if (rows.empty()) {
            return std::nullopt;
        }
        return parsePaymentSession(rows[0]);
    }

    // review
    // 通过支付会话ID获取支付会话信息
    std::optional<PaymentSessionInfo> getPaymentSessionById(const std::string& payment_session_id) {
        auto guard = acquire();
        const std::string sql = paymentSessionSelectColumns() + " FROM payment_sessions WHERE payment_session_id='" +
                                escape(guard.get(), payment_session_id) + "' LIMIT 1";
        auto rows = queryRows(guard.get(), sql);
        if (rows.empty()) {
            return std::nullopt;
        }
        return parsePaymentSession(rows[0]);
    }

    // review
    // 创建支付会话并落入到数据库中
    bool createPaymentSession(const CreatePaymentSessionInput& input) {
        if (input.ttl_seconds <= 0) {
            throw MySqlException("支付会话 ttl 必须为正数");
        }
        auto guard = acquire();
        const std::string sql =
            "INSERT INTO payment_sessions "
            "(payment_session_id, order_sn, account_id, buyer_id, buyer_email, status, expire_at, "
            "created_at, updated_at) VALUES ('" +
            escape(guard.get(), input.payment_session_id) + "','" + escape(guard.get(), input.order_sn) + "','" +
            escape(guard.get(), input.account_id) + "','" + escape(guard.get(), input.buyer_id) + "','" +
            escape(guard.get(), input.buyer_contact_masked) + "'," +
            std::to_string(static_cast<int>(PaymentSessionStatus::WaitingPay)) + ", DATE_ADD(NOW(), INTERVAL " +
            std::to_string(input.ttl_seconds) + " SECOND), NOW(), NOW())";
        execute(guard.get(), sql);
        return true;
    }

    // review
    // 通过request_id获取支付尝试信息
    std::optional<PaymentAttemptInfo> getPaymentAttemptByRequestId(const std::string& request_id) {
        auto guard = acquire();
        const std::string sql = paymentAttemptSelectColumns() + " FROM payment_attempts WHERE request_id='" +
                                escape(guard.get(), request_id) + "' LIMIT 1";
        auto rows = queryRows(guard.get(), sql);
        if (rows.empty()) {
            return std::nullopt;
        }
        return parsePaymentAttempt(rows[0]);
    }

    // review
    // 创建支付尝试信息并落入到数据库中
    bool createPaymentAttemptProcessing(const CreatePaymentAttemptInput& input) {
        auto guard = acquire();
        const std::string sql =
            "INSERT INTO payment_attempts "
            "(request_id, payment_session_id, order_sn, account_id, buyer_id, result, fail_reason, client_ip, "
            "user_agent, created_at, updated_at) VALUES ('" +
            escape(guard.get(), input.request_id) + "','" + escape(guard.get(), input.payment_session_id) + "','" +
            escape(guard.get(), input.order_sn) + "','" + escape(guard.get(), input.account_id) + "','" +
            escape(guard.get(), input.buyer_id) + "'," +
            std::to_string(static_cast<int>(PaymentAttemptResult::Processing)) + ", NULL, '" +
            escape(guard.get(), input.client_ip) + "','" + escape(guard.get(), input.user_agent) + "', NOW(), NOW())";
        execute(guard.get(), sql);
        return true;
    }

    // review
    // 更新支付尝试结果
    bool updatePaymentAttemptResult(const std::string& request_id, PaymentAttemptResult result,
                                    const std::string& fail_reason = "") {
        auto guard = acquire();
        const std::string sql = "UPDATE payment_attempts SET result=" + std::to_string(static_cast<int>(result)) +
                                ", fail_reason=" + nullableString(guard.get(), fail_reason) +
                                ", updated_at=NOW() WHERE request_id='" + escape(guard.get(), request_id) + "'";
        return executeAffected(guard.get(), sql) == 1;
    }

    // review
    // 过期支付会话
    int expireTimeoutPaymentSessions() {
        auto guard = acquire();
        const std::string sql =
            "UPDATE payment_sessions SET status=" + std::to_string(static_cast<int>(PaymentSessionStatus::Expired)) +
            ", updated_at=NOW() WHERE status=" + std::to_string(static_cast<int>(PaymentSessionStatus::WaitingPay)) +
            " AND expire_at <= NOW()";
        return static_cast<int>(executeAffected(guard.get(), sql));
    }

    // review
    bool finalizePaymentSuccess(const PaymentFinalizeInput& input) {
        auto guard = acquire();
        MYSQL* conn = guard.get();

        try {
            begin(conn);

            // 文档要求的锁顺序：payment_sessions -> orders -> accounts -> payment_attempts。
            const std::string session_lock_sql =
                "SELECT payment_session_id, order_sn, account_id, buyer_id, status, expire_at "
                "FROM payment_sessions WHERE payment_session_id='" +
                escape(conn, input.payment_session_id) + "' FOR UPDATE";
            auto session_rows = queryRows(conn, session_lock_sql);
            if (session_rows.empty()) {
                rollback(conn);
                markAttemptFailedNoThrow(input.request_id, "SESSION_NOT_FOUND");
                return false;
            }

            const std::string session_order_sn = get(session_rows[0], 1);
            const std::string session_account_id = get(session_rows[0], 2);
            const std::string session_buyer_id = get(session_rows[0], 3);
            const int session_status = static_cast<int>(toInt64(session_rows[0], 4));

            if (session_order_sn != input.order_sn || session_account_id != input.account_id ||
                session_buyer_id != input.buyer_id) {
                rollback(conn);
                markAttemptFailedNoThrow(input.request_id, "SESSION_BINDING_MISMATCH");
                return false;
            }

            if (session_status != static_cast<int>(PaymentSessionStatus::WaitingPay)) {
                rollback(conn);
                markAttemptFailedNoThrow(input.request_id, "SESSION_NOT_PAYABLE");
                return false;
            }

            const std::string order_lock_sql =
                "SELECT status, account_id, buyer_id, price FROM orders WHERE order_sn='" +
                escape(conn, input.order_sn) + "' FOR UPDATE";
            auto order_rows = queryRows(conn, order_lock_sql);
            if (order_rows.empty()) {
                rollback(conn);
                markAttemptFailedNoThrow(input.request_id, "ORDER_NOT_FOUND");
                return false;
            }

            const int order_status = static_cast<int>(toInt64(order_rows[0], 0));
            const std::string order_account_id = get(order_rows[0], 1);
            const std::string order_buyer_id = get(order_rows[0], 2);
            const int64_t order_price = toInt64(order_rows[0], 3);

            if (order_status != static_cast<int>(OrderStatus::PendingPay)) {
                rollback(conn);
                markAttemptFailedNoThrow(input.request_id, order_status == static_cast<int>(OrderStatus::Paid)
                                                               ? "ORDER_ALREADY_PAID"
                                                               : "ORDER_NOT_PAYABLE");
                return false;
            }
            if (order_account_id != input.account_id || order_buyer_id != input.buyer_id ||
                order_price != input.price) {
                rollback(conn);
                markAttemptFailedNoThrow(input.request_id, "ORDER_SNAPSHOT_MISMATCH");
                return false;
            }

            const std::string account_lock_sql = "SELECT status, version FROM accounts WHERE account_id='" +
                                                 escape(conn, input.account_id) + "' FOR UPDATE";
            auto account_rows = queryRows(conn, account_lock_sql);
            if (account_rows.empty()) {
                rollback(conn);
                markAttemptFailedNoThrow(input.request_id, "ACCOUNT_NOT_FOUND");
                return false;
            }
            const int account_status = static_cast<int>(toInt64(account_rows[0], 0));
            if (account_status != static_cast<int>(AccountStatus::OnSale) &&
                account_status != static_cast<int>(AccountStatus::Trading)) {
                rollback(conn);
                markAttemptFailedNoThrow(input.request_id, "ACCOUNT_NOT_TRADABLE");
                return false;
            }

            const std::string attempt_lock_sql = "SELECT result, payment_session_id, order_sn, account_id, buyer_id "
                                                 "FROM payment_attempts WHERE request_id='" +
                                                 escape(conn, input.request_id) + "' FOR UPDATE";
            auto attempt_rows = queryRows(conn, attempt_lock_sql);
            if (attempt_rows.empty()) {
                rollback(conn);
                return false;
            }
            const int attempt_result = static_cast<int>(toInt64(attempt_rows[0], 0));
            if (attempt_result == static_cast<int>(PaymentAttemptResult::Success)) {
                commit(conn);
                return true;
            }
            if (attempt_result == static_cast<int>(PaymentAttemptResult::Failed)) {
                rollback(conn);
                return false;
            }
            if (get(attempt_rows[0], 1) != input.payment_session_id || get(attempt_rows[0], 2) != input.order_sn ||
                get(attempt_rows[0], 3) != input.account_id || get(attempt_rows[0], 4) != input.buyer_id) {
                rollback(conn);
                markAttemptFailedNoThrow(input.request_id, "REQUEST_BINDING_MISMATCH");
                return false;
            }

            auto user_rows = queryRows(
                conn,
                "SELECT status, real_auth_completed, email_hash, IFNULL(id_card_hash,'') FROM users WHERE user_id='" +
                    escape(conn, input.buyer_id) + "' LIMIT 1");
            if (user_rows.empty()) {
                rollback(conn);
                markAttemptFailedNoThrow(input.request_id, "USER_NOT_FOUND");
                return false;
            }
            const int user_status = static_cast<int>(toInt64(user_rows[0], 0));
            const int real_auth_completed = static_cast<int>(toInt64(user_rows[0], 1));
            if (user_status != static_cast<int>(UserStatus::Normal) || real_auth_completed != 1 ||
                get(user_rows[0], 2).empty() || get(user_rows[0], 3).empty()) {
                rollback(conn);
                markAttemptFailedNoThrow(input.request_id, "USER_REAL_AUTH_INVALID");
                return false;
            }

            const uint64_t order_affected = executeAffected(conn, "UPDATE orders SET status=1, paid_at=NOW(), update_time=NOW() WHERE order_sn='" + escape(conn, input.order_sn) + "' AND status=0");

            const uint64_t account_affected = executeAffected(
                conn, "UPDATE accounts SET status=2, version=version+1, updated_at=NOW() WHERE account_id='" +
                          escape(conn, input.account_id) + "' AND status IN (0,1)");
            const uint64_t session_affected = executeAffected(
                conn, "UPDATE payment_sessions SET status=1, updated_at=NOW() WHERE payment_session_id='" +
                          escape(conn, input.payment_session_id) + "' AND status=0");
            const uint64_t attempt_affected = executeAffected(
                conn, "UPDATE payment_attempts SET result=1, fail_reason=NULL, updated_at=NOW() WHERE request_id='" +
                          escape(conn, input.request_id) + "' AND result=0");

            if (order_affected != 1 || account_affected != 1 || session_affected != 1 || attempt_affected != 1) {
                rollback(conn);
                markAttemptFailedNoThrow(input.request_id, "PAYMENT_STATE_UPDATE_FAILED");
                return false;
            }

            commit(conn);
            return true;
        } catch (const std::exception& e) {
            rollback(conn);
            LOG_ERROR("[mysql] finalizePaymentSuccess 执行失败: {}", e.what());
            markAttemptFailedNoThrow(input.request_id, "PAYMENT_INTERNAL_ERROR");
            return false;
        }
    }

  private:
    // review
    MySqlPool() = default;

    // review
    ~MySqlPool() {
        shutdown();
    }

    // review
    MySqlPool(const MySqlPool&) = delete;
    // review
    MySqlPool& operator=(const MySqlPool&) = delete;

    payment_config::MySqlConfig cfg_;
    std::queue<MYSQL*> connections_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool initialized_ = false;
    bool shutting_down_ = false;

    // review
    MYSQL* createConnection() {
        MYSQL* conn = mysql_init(nullptr);
        if (conn == nullptr) {
            throw MySqlException("mysql_init 执行失败");
        }

        mysql_options(conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
        mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &cfg_.connect_timeout_seconds);
        mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &cfg_.read_timeout_seconds);
        mysql_options(conn, MYSQL_OPT_WRITE_TIMEOUT, &cfg_.write_timeout_seconds);

        if (mysql_real_connect(conn, cfg_.host.c_str(), cfg_.user.c_str(), cfg_.password.c_str(), cfg_.database.c_str(),
                               static_cast<unsigned int>(cfg_.port), nullptr, 0) == nullptr) {
            std::string err = mysql_error(conn);
            mysql_close(conn);
            throw MySqlException("mysql_real_connect 执行失败: " + err);
        }

        return conn;
    }

    // review
    // 连接池回收逻辑
    void release(MYSQL* conn) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutting_down_) {
            mysql_close(conn);
            return;
        }
        connections_.push(conn);
        cv_.notify_one();
    }

    // review
    // 只关心mysql执行是否成功
    static bool execute(MYSQL* conn, const std::string& sql) {
        if (mysql_query(conn, sql.c_str()) != 0) {
            throw MySqlException("mysql 执行失败: " + std::string(mysql_error(conn)));
        }
        drainResults(conn);
        return true;
    }

    // review
    // 关心mysql执行操作影响的行数
    static uint64_t executeAffected(MYSQL* conn, const std::string& sql) {
        if (mysql_query(conn, sql.c_str()) != 0) {
            throw MySqlException("mysql 执行失败: " + std::string(mysql_error(conn)));
        }
        const uint64_t affected = mysql_affected_rows(conn);
        drainResults(conn);
        return affected;
    }

    // review
    static void drainResults(MYSQL* conn) {
        MYSQL_RES* res = mysql_store_result(conn);
        if (res != nullptr) {
            mysql_free_result(res);
        }
        while (mysql_next_result(conn) == 0) {
            res = mysql_store_result(conn);
            if (res != nullptr) {
                mysql_free_result(res);
            }
        }
    }

    // review
    // 执行一条 SELECT SQL，并把结果集完整读取成“二维字符串数组”返回。
    static std::vector<std::vector<std::string>> queryRows(MYSQL* conn, const std::string& sql) {
        if (mysql_query(conn, sql.c_str()) != 0) {
            throw MySqlException("mysql 查询失败: " + std::string(mysql_error(conn)));
        }

        MYSQL_RES* res = mysql_store_result(conn);
        if (res == nullptr) {
            throw MySqlException("mysql_store_result 执行失败: " + std::string(mysql_error(conn)));
        }

        std::vector<std::vector<std::string>> rows;
        const unsigned int num_fields = mysql_num_fields(res);
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res)) != nullptr) {
            unsigned long* lengths = mysql_fetch_lengths(res);
            std::vector<std::string> values;
            values.reserve(num_fields);
            for (unsigned int i = 0; i < num_fields; ++i) {
                if (row[i] == nullptr) {
                    values.emplace_back("");
                } else {
                    values.emplace_back(row[i], lengths[i]);
                }
            }
            rows.push_back(std::move(values));
        }

        mysql_free_result(res);
        return rows;
    }

    // review
    // 开始事务
    static void begin(MYSQL* conn) {
        execute(conn, "START TRANSACTION");
    }

    // review
    // 提交事务
    static void commit(MYSQL* conn) {
        execute(conn, "COMMIT");
    }

    // review
    // 回滚
    static void rollback(MYSQL* conn) {
        try {
            execute(conn, "ROLLBACK");
        } catch (...) {
            LOG_ERROR("[mysql] rollback 执行失败");
        }
    }

    // review
    // 对字符串做 MySQL SQL 注入安全转义，使其可以安全嵌入 SQL 语句中。
    static std::string escape(MYSQL* conn, const std::string& value) {
        std::string out;
        out.resize(value.size() * 2 + 1);
        unsigned long len = mysql_real_escape_string(conn, &out[0], value.c_str(), value.size());
        out.resize(len);
        return out;
    }

    // review
    // 把 C++ 字符串转换成 SQL 字面量表达式，并支持 NULL 语义（空字符串 → SQL NULL）
    // 原因：sql有两种空，一种是空字符串，一种是NULL。
    // 如果为空则返回 NULL，否则返回带引号且已 escape 的字符串
    static std::string nullableString(MYSQL* conn, const std::string& value) {
        if (value.empty()) {
            return "NULL";
        }
        return "'" + escape(conn, value) + "'";
    }

    // review
    static std::string get(const std::vector<std::string>& row, std::size_t idx) {
        if (idx >= row.size()) {
            return "";
        }
        return row[idx];
    }

    // review
    static int64_t toInt64(const std::vector<std::string>& row, std::size_t idx) {
        const std::string v = get(row, idx);
        if (v.empty()) {
            return 0;
        }
        return std::stoll(v);
    }

    // review
    // 定义一条固定的 SELECT 字段列表，用于统一查询 users 表的字段结构。
    static std::string userSelectColumns() {
        return "SELECT id, user_id, username, password_hash, IFNULL(email_hash, ''), IFNULL(email_masked, ''), "
               "IFNULL(id_card_hash,''), IFNULL(id_card_masked,''), real_auth_completed, user_type, status, "
               "DATE_FORMAT(last_login_at, '%Y-%m-%d %H:%i:%s'), "
               "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s')";
    }

    // review
    // 定义一条固定的 SELECT 字段列表，用于统一查询 orders 表的字段结构。
    static std::string orderSelectColumns() {
        return "SELECT id, order_sn, account_id, buyer_id, price, buyer_email, buyer_id_card_hash, "
               "buyer_id_card_masked, status, "
               "DATE_FORMAT(create_time, '%Y-%m-%d %H:%i:%s'), "
               "IFNULL(DATE_FORMAT(paid_at, '%Y-%m-%d %H:%i:%s'), ''), "
               "DATE_FORMAT(update_time, '%Y-%m-%d %H:%i:%s')";
    }

    // review
    // 定义一条固定的 SELECT 字段列表，用于统一查询 accounts 表的字段结构。
    static std::string accountSelectColumns() {
        return "SELECT id, account_id, seller_id, game_name, server_area, price, account_level, hero_count, "
               "skin_count, "
               "CAST(rare_items AS CHAR), status, version, "
               "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s'), "
               "DATE_FORMAT(updated_at, '%Y-%m-%d %H:%i:%s')";
    }

    // review
    // 定义一条固定的 SELECT 字段列表，用于统一查询 payment_sessions 表的字段结构。
    static std::string paymentSessionSelectColumns() {
        return "SELECT id, payment_session_id, order_sn, account_id, buyer_id, buyer_email, "
               "status, DATE_FORMAT(expire_at, '%Y-%m-%d %H:%i:%s'), "
               "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s'), "
               "DATE_FORMAT(updated_at, '%Y-%m-%d %H:%i:%s')";
    }

    // review
    // 定义一条固定的 SELECT 字段列表，用于统一查询 payment_attempts 表的字段结构。
    static std::string paymentAttemptSelectColumns() {
        return "SELECT id, request_id, payment_session_id, order_sn, account_id, buyer_id, result, "
               "IFNULL(fail_reason,''), IFNULL(client_ip,''), IFNULL(user_agent,''), "
               "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s'), "
               "DATE_FORMAT(updated_at, '%Y-%m-%d %H:%i:%s')";
    }

    // review
    // 该函数依赖 SELECT 字段顺序的固定约定，r 的列索引与 UserInfo 字段一一映射, 因此通过 get(idx) 进行位置访问
    static UserInfo parseUser(const std::vector<std::string>& r) {
        UserInfo u;
        u.id = toInt64(r, 0);
        u.user_id = get(r, 1);
        u.username = get(r, 2);
        u.password_hash = get(r, 3);
        u.email_hash = get(r, 4);
        u.email_masked = get(r, 5);
        u.id_card_hash = get(r, 6);
        u.id_card_masked = get(r, 7);
        u.real_auth_completed = static_cast<int>(toInt64(r, 8));
        u.user_type = static_cast<int>(toInt64(r, 9));
        u.status = static_cast<int>(toInt64(r, 10));
        u.last_login_at = get(r, 11);
        u.created_at = get(r, 12);
        return u;
    }

    // review
    static OrderInfo parseOrder(const std::vector<std::string>& r) {
        OrderInfo o;
        o.id = toInt64(r, 0);
        o.order_sn = get(r, 1);
        o.account_id = get(r, 2);
        o.buyer_id = get(r, 3);
        o.price = toInt64(r, 4);
        o.buyer_email = get(r, 5);
        o.buyer_id_card_hash = get(r, 6);
        o.buyer_id_card_masked = get(r, 7);
        o.status = static_cast<int>(toInt64(r, 8));
        o.create_time = get(r, 9);
        o.paid_at = get(r, 10);
        o.update_time = get(r, 11);
        return o;
    }

    // review
    static AccountInfo parseAccount(const std::vector<std::string>& r) {
        AccountInfo a;
        a.id = toInt64(r, 0);
        a.account_id = get(r, 1);
        a.seller_id = get(r, 2);
        a.game_name = get(r, 3);
        a.server_area = get(r, 4);
        a.price = toInt64(r, 5);
        a.account_level = static_cast<int>(toInt64(r, 6));
        a.hero_count = static_cast<int>(toInt64(r, 7));
        a.skin_count = static_cast<int>(toInt64(r, 8));
        a.rare_items = get(r, 9);
        a.status = static_cast<int>(toInt64(r, 10));
        a.version = static_cast<int>(toInt64(r, 11));
        a.created_at = get(r, 12);
        a.updated_at = get(r, 13);
        return a;
    }

    // review
    static PaymentSessionInfo parsePaymentSession(const std::vector<std::string>& r) {
        PaymentSessionInfo s;
        s.id = toInt64(r, 0);
        s.payment_session_id = get(r, 1);
        s.order_sn = get(r, 2);
        s.account_id = get(r, 3);
        s.buyer_id = get(r, 4);
        s.buyer_email = get(r, 5);
        s.status = static_cast<int>(toInt64(r, 6));
        s.expire_at = get(r, 7);
        s.created_at = get(r, 8);
        s.updated_at = get(r, 9);
        return s;
    }

    // review
    static PaymentAttemptInfo parsePaymentAttempt(const std::vector<std::string>& r) {
        PaymentAttemptInfo a;
        a.id = toInt64(r, 0);
        a.request_id = get(r, 1);
        a.payment_session_id = get(r, 2);
        a.order_sn = get(r, 3);
        a.account_id = get(r, 4);
        a.buyer_id = get(r, 5);
        a.result = static_cast<int>(toInt64(r, 6));
        a.fail_reason = get(r, 7);
        a.client_ip = get(r, 8);
        a.user_agent = get(r, 9);
        a.created_at = get(r, 10);
        a.updated_at = get(r, 11);
        return a;
    }

    void markAttemptFailedNoThrow(const std::string& request_id, const std::string& fail_reason) {
        try {
            updatePaymentAttemptResult(request_id, PaymentAttemptResult::Failed, fail_reason);
        } catch (...) {
            LOG_ERROR("[mysql] markAttemptFailedNoThrow 收到异常，暂不做任何处理");
        }
    }
};

} // namespace payment_mysql
