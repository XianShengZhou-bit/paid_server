// review
#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>

#include <hiredis/hiredis.h>

#include "config.hpp"
#include "logger.hpp"

namespace gateway_session {

class SessionStore {
  public:
    class ConnectionGuard {
      public:
        // review
        ConnectionGuard(SessionStore* store, redisContext* conn) : store_(store), conn_(conn) {}
        ~ConnectionGuard() {
            reset();
        }

        // review
        ConnectionGuard(ConnectionGuard&& other) noexcept : store_(other.store_), conn_(other.conn_) {
            other.store_ = nullptr;
            other.conn_ = nullptr;
        }

        // review
        ConnectionGuard& operator=(ConnectionGuard&& other) noexcept {
            if (this != &other) {
                reset();
                store_ = other.store_;
                conn_ = other.conn_;
                other.store_ = nullptr;
                other.conn_ = nullptr;
            }
            return *this;
        }

        // review
        redisContext* get() const {
            return conn_;
        }

      private:
        // review
        void reset() {
            if (store_ != nullptr && conn_ != nullptr) {
                store_->release(conn_);
            }
            store_ = nullptr;
            conn_ = nullptr;
        }

        ConnectionGuard(const ConnectionGuard&) = delete;
        ConnectionGuard& operator=(const ConnectionGuard&) = delete;

        SessionStore* store_ = nullptr;
        redisContext* conn_ = nullptr;
    };

    // review
    static SessionStore& instance() {
        static SessionStore store;
        return store;
    }

    // review
    void initFromConfig(const payment_config::Config& config = payment_config::Config::instance()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_) {
            return;
        }
        cfg_ = config.redis();
        shutting_down_ = false;
        for (int i = 0; i < cfg_.pool_size; ++i) {
            connections_.push(createConnection());
        }
        initialized_ = true;
        LOG_INFO("网关 Redis 只读连接池初始化完成: host={}, port={}, db={}, pool_size={}", cfg_.host, cfg_.port,
                 cfg_.db, cfg_.pool_size);
    }

    // review
    std::optional<std::string> userIdOf(const std::string& session_id) {
        if (session_id.empty()) {
            return std::nullopt;
        }

        auto guard = acquire();
        const std::string key = "payment:session:" + session_id;
        redisReply* reply = static_cast<redisReply*>(redisCommand(guard.get(), "GET %s", key.c_str()));
        if (reply == nullptr) {
            throw std::runtime_error("Redis GET 执行失败: " + connectionError(guard.get()));
        }
        if (reply->type == REDIS_REPLY_NIL) {
            freeReplyObject(reply);
            return std::nullopt;
        }
        if (reply->type != REDIS_REPLY_STRING) {
            freeReplyObject(reply);
            throw std::runtime_error("Redis GET 返回类型异常");
        }

        std::string user_id(reply->str, reply->len);
        freeReplyObject(reply);
        return user_id;
    }

  private:
    // review
    SessionStore() = default;
    // review
    ~SessionStore() {
        shutdown();
    }

    // review
    SessionStore(const SessionStore&) = delete;
    // review
    SessionStore& operator=(const SessionStore&) = delete;

    payment_config::RedisConfig cfg_;
    std::queue<redisContext*> connections_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool initialized_ = false;
    bool shutting_down_ = false;

    // review
    ConnectionGuard acquire(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!initialized_) {
            throw std::runtime_error("网关 Redis 连接池尚未初始化");
        }
        const bool ready = cv_.wait_for(lock, timeout, [&] { return shutting_down_ || !connections_.empty(); });
        if (!ready || shutting_down_) {
            throw std::runtime_error("获取网关 Redis 连接超时");
        }

        redisContext* conn = connections_.front();
        connections_.pop();
        lock.unlock();
        if (!ping(conn)) {
            redisFree(conn);
            conn = createConnection();
        }
        return ConnectionGuard(this, conn);
    }

    // review
    redisContext* createConnection() {
        timeval connect_timeout{};
        connect_timeout.tv_sec = static_cast<time_t>(cfg_.connect_timeout_seconds);
        redisContext* conn = redisConnectWithTimeout(cfg_.host.c_str(), cfg_.port, connect_timeout);
        if (conn == nullptr) {
            throw std::runtime_error("Redis 连接失败: 无法分配连接");
        }
        if (conn->err != 0) {
            const std::string error = connectionError(conn);
            redisFree(conn);
            throw std::runtime_error("Redis 连接失败: " + error);
        }

        timeval command_timeout{};
        command_timeout.tv_sec = static_cast<time_t>(cfg_.command_timeout_seconds);
        if (redisSetTimeout(conn, command_timeout) != REDIS_OK) {
            const std::string error = connectionError(conn);
            redisFree(conn);
            throw std::runtime_error("Redis 设置命令超时失败: " + error);
        }

        redisReply* auth_reply = cfg_.username.empty()
                                     ? static_cast<redisReply*>(redisCommand(conn, "AUTH %s", cfg_.password.c_str()))
                                     : static_cast<redisReply*>(redisCommand(conn, "AUTH %s %s", cfg_.username.c_str(),
                                                                             cfg_.password.c_str()));
        if (!statusOk(auth_reply)) {
            freeReplyObject(auth_reply);
            redisFree(conn);
            throw std::runtime_error("网关 Redis AUTH 失败");
        }
        freeReplyObject(auth_reply);

        redisReply* select_reply = static_cast<redisReply*>(redisCommand(conn, "SELECT %d", cfg_.db));
        if (!statusOk(select_reply)) {
            freeReplyObject(select_reply);
            redisFree(conn);
            throw std::runtime_error("网关 Redis SELECT 失败");
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
    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        shutting_down_ = true;
        while (!connections_.empty()) {
            redisFree(connections_.front());
            connections_.pop();
        }
        initialized_ = false;
        cv_.notify_all();
    }

    // review
    static bool ping(redisContext* conn) {
        if (conn == nullptr) {
            return false;
        }
        redisReply* reply = static_cast<redisReply*>(redisCommand(conn, "PING"));
        const bool ok = statusOk(reply, "PONG");
        freeReplyObject(reply);
        return ok;
    }

    // review
    static bool statusOk(redisReply* reply, const std::string& expected = "OK") {
        return reply != nullptr && reply->type == REDIS_REPLY_STATUS && reply->str != nullptr &&
               std::string(reply->str) == expected;
    }

    // review
    static std::string connectionError(redisContext* conn) {
        if (conn == nullptr || conn->errstr[0] == '\0') {
            return "未知错误";
        }
        return conn->errstr;
    }
};

} // namespace gateway_session
