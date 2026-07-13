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
    explicit MySqlException(const std::string& message) : std::runtime_error(message) {}
};

enum class UserStatus : int {
    Normal = 0,
    Frozen = 1,
    Cancelled = 2,
    RiskLimited = 3
};

struct UserInfo {
    std::string user_id;
    std::string password_hash;
    int status = 0;

    bool isNormal() const {
        return status == static_cast<int>(UserStatus::Normal);
    }
};

class MySqlPool {
  public:
    class ConnectionGuard {
      public:
        ConnectionGuard(MySqlPool* pool, MYSQL* conn) : pool_(pool), conn_(conn) {}
        ~ConnectionGuard() {
            reset();
        }

        ConnectionGuard(ConnectionGuard&& other) noexcept : pool_(other.pool_), conn_(other.conn_) {
            other.pool_ = nullptr;
            other.conn_ = nullptr;
        }

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

        MYSQL* get() const {
            return conn_;
        }

        explicit operator bool() const {
            return conn_ != nullptr;
        }

      private:
        void reset() {
            if (pool_ != nullptr && conn_ != nullptr) {
                pool_->release(conn_);
            }
            pool_ = nullptr;
            conn_ = nullptr;
        }

        ConnectionGuard(const ConnectionGuard&) = delete;
        ConnectionGuard& operator=(const ConnectionGuard&) = delete;

        MySqlPool* pool_ = nullptr;
        MYSQL* conn_ = nullptr;
    };

    static MySqlPool& instance() {
        static MySqlPool pool;
        return pool;
    }

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

    void initFromConfig(const payment_config::Config& config = payment_config::Config::instance()) {
        init(config.mysql());
    }

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
        cv_.notify_all();
    }

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
        if (mysql_ping(conn) != 0) {
            mysql_close(conn);
            conn = createConnection();
        }
        return ConnectionGuard(this, conn);
    }

    std::optional<UserInfo> getUserByUsername(const std::string& username) {
        auto guard = acquire();
        const std::string sql = "SELECT user_id, password_hash, status FROM users WHERE username='" +
                                escape(guard.get(), username) + "' LIMIT 1";
        auto rows = queryRows(guard.get(), sql);
        if (rows.empty()) {
            return std::nullopt;
        }
        return parseUser(rows[0]);
    }

  private:
    MySqlPool() = default;
    ~MySqlPool() {
        shutdown();
    }

    MySqlPool(const MySqlPool&) = delete;
    MySqlPool& operator=(const MySqlPool&) = delete;

    payment_config::MySqlConfig cfg_;
    std::queue<MYSQL*> connections_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool initialized_ = false;
    bool shutting_down_ = false;

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

    void release(MYSQL* conn) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutting_down_) {
            mysql_close(conn);
            return;
        }
        connections_.push(conn);
        cv_.notify_one();
    }

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

    static std::string escape(MYSQL* conn, const std::string& value) {
        std::string out;
        out.resize(value.size() * 2 + 1);
        unsigned long len = mysql_real_escape_string(conn, &out[0], value.c_str(), value.size());
        out.resize(len);
        return out;
    }

    static std::string get(const std::vector<std::string>& row, std::size_t idx) {
        if (idx >= row.size()) {
            return "";
        }
        return row[idx];
    }

    static int64_t toInt64(const std::vector<std::string>& row, std::size_t idx) {
        const std::string v = get(row, idx);
        if (v.empty()) {
            return 0;
        }
        return std::stoll(v);
    }

    static UserInfo parseUser(const std::vector<std::string>& row) {
        UserInfo user;
        user.user_id = get(row, 0);
        user.password_hash = get(row, 1);
        user.status = static_cast<int>(toInt64(row, 2));
        return user;
    }
};

} // namespace payment_mysql
