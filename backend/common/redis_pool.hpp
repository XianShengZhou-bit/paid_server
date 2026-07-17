#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>

#include <hiredis/hiredis.h>

#include "config.hpp"
#include "logger.hpp"

namespace payment_redis {

class RedisException : public std::runtime_error {
  public:
    // review
    explicit RedisException(const std::string& message) : std::runtime_error(message) {}
};

// 网关登录会话在 Redis 中的 key 前缀：payment:session:{session_id} -> user_id
// review
inline std::string sessionKey(const std::string& session_id) {
    return "payment:session:" + session_id;
}

class RedisPool {
  public:
    // 这是每条连接的封装对象，用于自动释放连接
    class ConnectionGuard {
      public:
        // review
        ConnectionGuard(RedisPool* pool, redisContext* conn) : pool_(pool), conn_(conn) {}

        // review
        ~ConnectionGuard() {
            reset();
        }

        // review
        ConnectionGuard(ConnectionGuard&& other) noexcept : pool_(other.pool_), conn_(other.conn_) {
            other.pool_ = nullptr;
            other.conn_ = nullptr;
        }

        // review
        ConnectionGuard& operator=(ConnectionGuard&& other) noexcept {
            if (this != &other) {
                reset();
                pool_ = other.pool_;
                conn_ = other.conn_;
                other.pool_ = nullptr;
                other.conn_ = nullptr;
            }
            return *this;
        }

        // review
        redisContext* get() const {
            return conn_;
        }

        // review
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

        RedisPool* pool_ = nullptr;
        redisContext* conn_ = nullptr;
    };

    // review
    static RedisPool& instance() {
        static RedisPool pool;
        return pool;
    }

    // review
    void init(const payment_config::RedisConfig& cfg) {
        if (initialized_) {
            return;
        }
        cfg_ = cfg;
        shutting_down_ = false;
        for (int i = 0; i < cfg_.pool_size; ++i) {
            connections_.push(createConnection()); // 不断创建连接放入连接池
        }
        initialized_ = true;
        LOG_INFO("redis pool initialized, host={}, port={}, db={}, pool_size={}", cfg_.host, cfg_.port, cfg_.db,
                 cfg_.pool_size);
    }

    // review
    void initFromConfig(const payment_config::Config& config = payment_config::Config::instance()) {
        init(config.redis());
    }

    // review
    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_ && connections_.empty()) {
            return;
        }
        shutting_down_ = true;
        while (!connections_.empty()) {
            redisContext* conn = connections_.front();
            connections_.pop();
            if (conn != nullptr) {
                redisFree(conn);
            }
        }
        initialized_ = false;
        cv_.notify_all();
    }

    // review
    ConnectionGuard acquire(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!initialized_) {
            throw RedisException("redis 连接池尚未初始化");
        }

        const bool ok = cv_.wait_for(lock, timeout, [&] { return shutting_down_ || !connections_.empty(); });
        if (!ok || shutting_down_) {
            throw RedisException("获取 redis 连接超时");
        }

        redisContext* conn = connections_.front();
        connections_.pop();
        lock.unlock();

        if (!ping(conn)) {
            redisFree(conn);
            conn = createConnection(); // 保持连接池中的连接总数恒定且均可用
        }
        return ConnectionGuard(this, conn);
    }

    // review
    // 写入字符串键值，并设置过期时间（秒）。
    bool setEx(const std::string& key, const std::string& value, int ttl_seconds) {
        if (ttl_seconds <= 0) {
            throw RedisException("redis SET EX ttl 必须为正数");
        }
        auto guard = acquire();
        redisReply* reply = static_cast<redisReply*>(
            redisCommand(guard.get(), "SET %s %s EX %d", key.c_str(), value.c_str(), ttl_seconds));
        const bool ok = checkStatusReply(reply, guard.get(), "SET");
        freeReplyObject(reply);
        return ok;
    }

    // review
    std::optional<std::string> get(const std::string& key) {
        auto guard = acquire();
        redisReply* reply = static_cast<redisReply*>(redisCommand(guard.get(), "GET %s", key.c_str()));
        if (reply == nullptr) {
            throw RedisException("redis GET 执行失败: " + connectionError(guard.get()));
        }
        if (reply->type == REDIS_REPLY_NIL) {
            freeReplyObject(reply);
            return std::nullopt;
        }
        if (reply->type != REDIS_REPLY_STRING) {
            freeReplyObject(reply);
            throw RedisException("redis GET 返回类型异常");
        }
        std::string value(reply->str, reply->len);
        freeReplyObject(reply);
        return value;
    }

    // review
    bool del(const std::string& key) {
        auto guard = acquire();
        redisReply* reply = static_cast<redisReply*>(redisCommand(guard.get(), "DEL %s", key.c_str()));
        const bool ok = checkIntegerReplyAtLeast(reply, guard.get(), "DEL", 0);
        freeReplyObject(reply);
        return ok;
    }

    // review
    bool exists(const std::string& key) {
        auto guard = acquire();
        redisReply* reply = static_cast<redisReply*>(redisCommand(guard.get(), "EXISTS %s", key.c_str()));
        const bool ok = checkIntegerReplyAtLeast(reply, guard.get(), "EXISTS", 1);
        freeReplyObject(reply);
        return ok;
    }

    // 网关登录会话：写入 session_id -> user_id，并设置 TTL。
    // review
    bool saveSession(const std::string& session_id, const std::string& user_id, int ttl_seconds) {
        return setEx(sessionKey(session_id), user_id, ttl_seconds);
    }

    // review
    std::optional<std::string> userIdOfSession(const std::string& session_id) {
        return get(sessionKey(session_id));
    }

    // review
    bool removeSession(const std::string& session_id) {
        return del(sessionKey(session_id));
    }

  private:
    RedisPool() = default;

    ~RedisPool() {
        shutdown();
    }

    RedisPool(const RedisPool&) = delete;
    RedisPool& operator=(const RedisPool&) = delete;

    payment_config::RedisConfig cfg_;
    std::queue<redisContext*> connections_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool initialized_ = false;
    bool shutting_down_ = false;

    // review
    static std::string connectionError(redisContext* conn) {
        if (conn == nullptr) {
            return "连接为空";
        }

        if (conn->errstr[0] != '\0') {
            return std::string(conn->errstr);
        }
        return "未知错误";
    }

    // review
    redisContext* createConnection() {
        timeval timeout{};
        timeout.tv_sec = static_cast<time_t>(cfg_.connect_timeout_seconds);
        timeout.tv_usec = 0;

        redisContext* conn = redisConnectWithTimeout(cfg_.host.c_str(), cfg_.port, timeout);
        if (conn == nullptr) {
            throw RedisException("redisConnectWithTimeout 执行失败: 无法分配连接");
        }
        if (conn->err != 0) {
            std::string err = connectionError(conn);
            redisFree(conn);
            throw RedisException("redis 连接失败: " + err);
        }

        // 命令读写超时：作用于后续 AUTH/SELECT/PING/GET/SET 等 redisCommand
        timeval command_timeout{};
        command_timeout.tv_sec = static_cast<time_t>(cfg_.command_timeout_seconds);
        command_timeout.tv_usec = 0;
        if (redisSetTimeout(conn, command_timeout) != REDIS_OK) {
            std::string err = connectionError(conn);
            redisFree(conn);
            throw RedisException("redis 设置命令超时失败: " + err);
        }

        if (!cfg_.password.empty()) { // 成功连接后，使用后端读写账号进行认证
            redisReply* auth_reply =
                cfg_.username.empty()
                    ? static_cast<redisReply*>(redisCommand(conn, "AUTH %s", cfg_.password.c_str()))
                    : static_cast<redisReply*>(
                          redisCommand(conn, "AUTH %s %s", cfg_.username.c_str(), cfg_.password.c_str()));
            if (!checkStatusReply(auth_reply, conn, "AUTH")) {
                freeReplyObject(auth_reply);
                redisFree(conn);
                throw RedisException("redis AUTH 失败");
            }
            freeReplyObject(auth_reply);
        }

        redisReply* select_reply = static_cast<redisReply*>(redisCommand(conn, "SELECT %d", cfg_.db));
        if (!checkStatusReply(select_reply, conn, "SELECT")) {
            freeReplyObject(select_reply);
            redisFree(conn);
            throw RedisException("redis SELECT 失败");
        }
        freeReplyObject(select_reply);
        return conn;
    }

    // review
    void release(redisContext* conn) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutting_down_) {
            redisFree(conn);
            return;
        }
        connections_.push(conn);
        cv_.notify_one();
    }

    // review
    static bool ping(redisContext* conn) {
        if (conn == nullptr) {
            return false;
        }
        redisReply* reply = static_cast<redisReply*>(redisCommand(conn, "PING"));
        const bool ok = checkStatusReply(reply, conn, "PING");
        freeReplyObject(reply);
        return ok;
    }

    // review
    static bool checkStatusReply(redisReply* reply, redisContext* conn, const char* op) {
        if (reply == nullptr) {
            throw RedisException(std::string("redis ") + op + " 执行失败: " + connectionError(conn));
        }
        if (reply->type == REDIS_REPLY_STATUS &&
            reply->str != nullptr) { // 正常无参 PING 走的都是 REDIS_REPLY_STATUS + PONG
            return std::string(reply->str) == "OK" || std::string(reply->str) == "PONG";
        }
        if (reply->type == REDIS_REPLY_STRING &&
            reply->str != nullptr) { // 带参 PING 如 PING hello 走的是 REDIS_REPLY_STRING + hello
            return std::string(reply->str) == "PONG";
        }
        throw RedisException(std::string("redis ") + op + " 返回异常响应");
    }

    // review
    static bool checkIntegerReplyAtLeast(redisReply* reply, redisContext* conn, const char* op, long long min_value) {
        if (reply == nullptr) {
            throw RedisException(std::string("redis ") + op + " 执行失败: " + connectionError(conn));
        }
        if (reply->type != REDIS_REPLY_INTEGER) {
            throw RedisException(std::string("redis ") + op + " 返回类型异常");
        }
        return reply->integer >= min_value;
    }
};

} // namespace payment_redis
