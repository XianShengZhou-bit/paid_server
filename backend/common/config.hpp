// review

#ifndef PAYMENT_CONFIG_HPP
#define PAYMENT_CONFIG_HPP

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace payment_config {

class ConfigException : public std::runtime_error {
  public:
    // review
    explicit ConfigException(const std::string& message) : std::runtime_error(message) {}
};

// review
struct MySqlConfig {
    std::string host;
    int port;
    std::string user;
    std::string password;
    std::string database;
    unsigned int connect_timeout_seconds;
    unsigned int read_timeout_seconds;
    unsigned int write_timeout_seconds;
    int pool_size;
};

// review
struct RedisConfig {
    std::string host;
    int port;
    int db;
    std::string username;
    std::string password;
    unsigned int connect_timeout_seconds;
    unsigned int command_timeout_seconds;
    int pool_size;
};

// review
struct JwtConfig {
    std::string algorithm;
    std::string secret;

    // @brief 支付二维码 Token 的有效期，单位是秒。
    int qr_token_ttl_seconds;

    // @brief 用于处理服务器之间时间不完全一致的问题，单位是秒。
    int clock_skew_seconds;
};

// review
struct HmacHashConfig {
    std::string user_email_hash_secret;
    std::string user_id_card_hash_secret;
};

// review
struct PasswordHashConfig {
    // @brief 登录密码 HMAC-SHA256 统一密钥；与邮箱/身份证哈希 secret 一样仅保存在服务端环境变量中。
    std::string secret;
};

// review
struct PaymentBackendConfig {
    std::string ip;
    int http_port;
    int websocket_port;
};

// review
struct PaymentGatewayConfig {
    // 网关进程 bind 地址，固定监听所有网卡。
    static constexpr const char* kBindAddress = "0.0.0.0";

    // 供二维码、WebSocket 等返回给客户端的外网/局域网可访问主机名或 IP（来自 PAYMENT_GATEWAY_IP）。
    std::string public_host;
    int websocket_port;
    int http_port;
};

// review
struct PaymentNginxConfig {
    // Nginx 进程 bind 地址，固定监听所有网卡。
    static constexpr const char* kBindAddress = "0.0.0.0";

    // 供浏览器打开前端页面的外网/局域网可访问主机名或 IP（来自 PAYMENT_NGINX_IP）。
    std::string public_host;
    int port;
    // 手机扫码支付页路径（不含 query），由 Nginx 提供，如 /payment/scan。
    std::string scan_page_path;
};

// review
struct LoggerConfig {
    std::string file;          // 表示日志文件的路径
    std::size_t max_file_size; // 表示日志文件的最大大小
    std::size_t max_files;     // 表示日志文件的最大数量
    bool console_enabled;      // 表示是否在控制台输出日志
};

// review
struct PaymentRuntimeConfig {
    // @brief 登录会话在 Redis 中的有效期，单位是秒。
    int login_session_ttl_seconds;

    // @brief 这是创建支付会话的过期时间，支付会话保存在sql中
    int payment_session_ttl_seconds;

    // @brief 页面 B 最终确认支付时从后端获取的request_id有效期，单位是秒。
    int payment_attempt_request_ttl_seconds;

    // @brief WebSocket 心跳间隔，单位是秒。
    int websocket_heartbeat_interval_seconds;

    // @brief WebSocket 空闲超时时间，单位秒。超过这个时间没有心跳，认为连接断开。
    int websocket_idle_timeout_seconds;

    // @brief 登录密码再认证最多允许失败的次数。
    int login_reauth_max_retry;
};

class Config {
  public:
    // review
    static const Config& instance() {
        static Config cfg;
        return cfg;
    }

    // review
    Config(const Config&) = delete;

    // review
    Config& operator=(const Config&) = delete;

    // review
    // 从环境变量加载配置
    void loadFromEnvFile(const std::string& filepath = ".env") {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            throw ConfigException("无法打开配置文件: " + filepath);
        }

        std::string line;
        while (std::getline(file, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') {
                continue;
            }

            const std::size_t pos = line.find_first_of("=:");
            if (pos == std::string::npos) {
                throw ConfigException("无效的配置项: " + line);
            }

            std::string key = trim(line.substr(0, pos));
            std::string value = trim(line.substr(pos + 1));
            value = trim(stripInlineComment(value));

            if (!key.empty()) {
                if (values_.find(key) == values_.end()) {
                    values_[key] = value;
                } else {
                    throw ConfigException("重复的配置项: " + key);
                }
            } else {
                throw ConfigException("无效的配置项: " + line);
            }
        }
    }

    // review
    // 每次调用都是获取一个新的mysql连接池配置
    MySqlConfig mysql() const {
        MySqlConfig cfg;
        cfg.host = requireString("MYSQL_HOST");
        cfg.port = requireInt("MYSQL_PORT");
        cfg.user = requireString("MYSQL_USER");
        cfg.password = requireString("MYSQL_PASSWORD");
        cfg.database = requireString("MYSQL_DATABASE");
        cfg.connect_timeout_seconds = requireInt("MYSQL_CONNECT_TIMEOUT_SECONDS");
        cfg.read_timeout_seconds = requireInt("MYSQL_READ_TIMEOUT_SECONDS");
        cfg.write_timeout_seconds = requireInt("MYSQL_WRITE_TIMEOUT_SECONDS");
        cfg.pool_size = requireInt("MYSQL_POOL_SIZE");
        return cfg;
    }

    // review
    // 每次调用都是获取一个新的logger配置
    LoggerConfig logger() const {
        LoggerConfig cfg;
        cfg.file = requireString("LOG_FILE");
        cfg.max_file_size = requireInt("LOG_MAX_FILE_SIZE");
        cfg.max_files = requireInt("LOG_MAX_FILES");
        cfg.console_enabled = requireBool("LOG_CONSOLE_ENABLED");
        return cfg;
    }

    // review
    // 每次调用都是获取一个新的redis配置
    RedisConfig redis() const {
        RedisConfig cfg;
        cfg.host = requireString("REDIS_HOST");
        cfg.port = requireInt("REDIS_PORT");
        cfg.db = requireInt("REDIS_DB");
        cfg.username = requireString("REDIS_BACKEND_USERNAME");
        cfg.password = requireString("REDIS_BACKEND_PASSWORD");
        cfg.connect_timeout_seconds = requireInt("REDIS_CONNECT_TIMEOUT_SECONDS");
        cfg.command_timeout_seconds = requireInt("REDIS_COMMAND_TIMEOUT_SECONDS");
        cfg.pool_size = requireInt("REDIS_POOL_SIZE");
        requirePositive("REDIS_PORT", cfg.port);
        requirePositive("REDIS_POOL_SIZE", cfg.pool_size);
        if (cfg.db < 0) {
            throw ConfigException("REDIS_DB 必须 >= 0");
        }
        if (cfg.connect_timeout_seconds == 0) {
            throw ConfigException("REDIS_CONNECT_TIMEOUT_SECONDS 必须为正数");
        }
        if (cfg.command_timeout_seconds == 0) {
            throw ConfigException("REDIS_COMMAND_TIMEOUT_SECONDS 必须为正数");
        }
        return cfg;
    }

    // review
    // 每次调用都是获取一个新的jwt配置
    JwtConfig jwt() const {
        JwtConfig cfg;
        cfg.algorithm = requireString("JWT_ALGORITHM");
        cfg.secret = requireString("JWT_SECRET");
        cfg.qr_token_ttl_seconds = requireInt("JWT_QR_TOKEN_TTL_SECONDS");
        cfg.clock_skew_seconds = requireInt("JWT_CLOCK_SKEW_SECONDS");
        validateJwtAlgorithm(cfg.algorithm);
        requirePositive("JWT_QR_TOKEN_TTL_SECONDS", cfg.qr_token_ttl_seconds);
        if (cfg.clock_skew_seconds < 0) {
            throw ConfigException("JWT_CLOCK_SKEW_SECONDS 必须 >= 0");
        }
        return cfg;
    }

    // review
    // 每次调用都是获取一个新的hmac hash配置
    HmacHashConfig hash() const {
        HmacHashConfig cfg;
        cfg.user_email_hash_secret = requireString("USER_EMAIL_HASH_SECRET");
        cfg.user_id_card_hash_secret = requireString("USER_ID_CARD_HASH_SECRET");

        return cfg;
    }

    // review
    // 每次调用都是获取一个新的登录密码哈希配置
    PasswordHashConfig passwordHash() const {
        PasswordHashConfig cfg;
        cfg.secret = requireString("PASSWORD_HASH_SECRET");
        return cfg;
    }

    // review
    // 每次调用都是获取一个新的支付服务后端配置
    PaymentBackendConfig paymentBackend() const {
        PaymentBackendConfig cfg;
        cfg.ip = requireString("PAYMENT_BACKEND_IP");
        cfg.http_port = requireInt("PAYMENT_BACKEND_HTTP_PORT");
        cfg.websocket_port = requireInt("PAYMENT_BACKEND_WEBSOCKET_PORT");
        requirePositive("PAYMENT_BACKEND_HTTP_PORT", cfg.http_port);
        requirePositive("PAYMENT_BACKEND_WEBSOCKET_PORT", cfg.websocket_port);
        return cfg;
    }

    // review
    // 每次调用都是获取一个新的支付网关配置，后续可能需要支持多个支付网关，因此需要每次调用都获取一个新的配置。(负载均衡)
    PaymentGatewayConfig paymentGateway() const {
        PaymentGatewayConfig cfg;
        cfg.public_host = requireString("PAYMENT_GATEWAY_IP");
        cfg.websocket_port = requireInt("PAYMENT_GATEWAY_WEBSOCKET_PORT");
        cfg.http_port = requireInt("PAYMENT_GATEWAY_HTTP_PORT");
        requirePositive("PAYMENT_GATEWAY_WEBSOCKET_PORT", cfg.websocket_port);
        requirePositive("PAYMENT_GATEWAY_HTTP_PORT", cfg.http_port);
        return cfg;
    }

    // review
    // 每次调用都是获取一个新的支付nginx配置
    PaymentNginxConfig paymentNginx() const {
        PaymentNginxConfig cfg;
        cfg.public_host = requireString("PAYMENT_NGINX_IP");
        cfg.port = requireInt("PAYMENT_NGINX_PORT");
        cfg.scan_page_path = requireString("PAYMENT_SCAN_PAGE_PATH");
        requirePositive("PAYMENT_NGINX_PORT", cfg.port);
        if (cfg.scan_page_path.empty() || cfg.scan_page_path.front() != '/') {
            throw ConfigException("PAYMENT_SCAN_PAGE_PATH 必须以 / 开头");
        }
        return cfg;
    }

    // review
    // 获取一个新的支付运行时配置
    PaymentRuntimeConfig runtime() const {
        PaymentRuntimeConfig cfg;
        cfg.login_session_ttl_seconds =
            findString("LOGIN_SESSION_TTL_SECONDS").has_value() ? requireInt("LOGIN_SESSION_TTL_SECONDS") : 86400;
        cfg.payment_session_ttl_seconds = requireInt("PAYMENT_SESSION_TTL_SECONDS");
        cfg.payment_attempt_request_ttl_seconds = requireInt("PAYMENT_ATTEMPT_REQUEST_TTL_SECONDS");
        cfg.websocket_heartbeat_interval_seconds = requireInt("WEBSOCKET_HEARTBEAT_INTERVAL_SECONDS");
        cfg.websocket_idle_timeout_seconds = requireInt("WEBSOCKET_IDLE_TIMEOUT_SECONDS");
        cfg.login_reauth_max_retry = requireInt("LOGIN_REAUTH_MAX_RETRY");
        requirePositive("LOGIN_SESSION_TTL_SECONDS", cfg.login_session_ttl_seconds);
        requirePositive("PAYMENT_SESSION_TTL_SECONDS", cfg.payment_session_ttl_seconds);
        requirePositive("PAYMENT_ATTEMPT_REQUEST_TTL_SECONDS", cfg.payment_attempt_request_ttl_seconds);
        requirePositive("WEBSOCKET_HEARTBEAT_INTERVAL_SECONDS", cfg.websocket_heartbeat_interval_seconds);
        requirePositive("WEBSOCKET_IDLE_TIMEOUT_SECONDS", cfg.websocket_idle_timeout_seconds);
        requirePositive("LOGIN_REAUTH_MAX_RETRY", cfg.login_reauth_max_retry);
        return cfg;
    }

  private:
    // review
    Config() = default;

    // review
    // 从配置中查找一个字符串配置项，如果配置项不存在，返回 std::nullopt。
    std::optional<std::string> findString(const std::string& key) const {
        auto it = values_.find(key);
        if (it == values_.end()) {
            return std::nullopt;
        }

        return it->second;
    }

    // review
    // 从配置中查找一个字符串配置项，如果配置项不存在，抛出 ConfigException 异常。
    std::string requireString(const std::string& key) const {
        auto value = findString(key);
        if (!value.has_value() || value->empty()) {
            throw ConfigException("缺少必需配置项: " + key);
        }

        return *value;
    }

    // review
    // 从配置中查找一个整数配置项，如果配置项不存在，抛出 ConfigException 异常。
    int requireInt(const std::string& key) const {
        auto value = findString(key);
        if (!value.has_value() || value->empty()) {
            throw ConfigException("缺少必需配置项: " + key);
        }
        try {
            std::size_t idx = 0;
            int parsed = std::stoi(*value, &idx);
            if (idx != value->size()) {
                throw std::invalid_argument("存在多余字符");
            }
            return parsed;
        } catch (...) {
            throw ConfigException("无效的整数配置项: " + key);
        }
    }

    // review
    // 从配置中查找一个布尔配置项，如果配置项不存在，抛出 ConfigException 异常。
    bool requireBool(const std::string& key) const {
        auto value = findString(key);
        if (!value.has_value() || value->empty()) {
            throw ConfigException("缺少必需配置项: " + key);
        }

        std::string v = *value;
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (v == "true" || v == "1" || v == "yes" || v == "on") {
            return true;
        }
        if (v == "false" || v == "0" || v == "no" || v == "off") {
            return false;
        }
        throw ConfigException("无效的布尔配置项: " + key);
    }

    // review
    static void requirePositive(const std::string& key, int value) {
        if (value <= 0) {
            throw ConfigException(key + " 必须为正数");
        }
    }

    // review
    static void validateJwtAlgorithm(const std::string& algorithm) {
        if (algorithm != "HS256" && algorithm != "HS384" && algorithm != "HS512") {
            throw ConfigException("不支持的 JWT_ALGORITHM: " + algorithm);
        }
    }

    // review
    static std::string trim(const std::string& input) {
        std::size_t start = input.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) {
            return "";
        }
        std::size_t end = input.find_last_not_of(" \t\r\n");
        return input.substr(start, end - start + 1);
    }

    // review
    static std::string stripInlineComment(const std::string& input) {
        for (std::size_t i = 0; i < input.size(); ++i) {
            if (input[i] == '#') {
                return input.substr(0, i);
            }
        }
        return input;
    }

    std::map<std::string, std::string> values_;
};

} // namespace payment_config

#endif // PAYMENT_CONFIG_HPP