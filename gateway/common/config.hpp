// review

#ifndef PAYMENT_CONFIG_HPP
#define PAYMENT_CONFIG_HPP

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace payment_config {

class ConfigException : public std::runtime_error {
  public:
    explicit ConfigException(const std::string& message) : std::runtime_error(message) {}
};

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

struct PaymentBackendConfig {
    std::string ip;
    int http_port;
};

struct PaymentGatewayConfig {
    int websocket_port;
    int http_port;
};

// review
struct PaymentNginxConfig {
    // 外界可访问host地址
    std::string ip;
    int port;
};

struct LoggerConfig {
    std::string file;
    std::size_t max_file_size;
    std::size_t max_files;
    bool console_enabled;
};

class Config {
  public:
    static const Config& instance() {
        static Config cfg;
        return cfg;
    }

    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

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

    LoggerConfig logger() const {
        LoggerConfig cfg;
        cfg.file = requireString("LOG_FILE");
        cfg.max_file_size = requireInt("LOG_MAX_FILE_SIZE");
        cfg.max_files = requireInt("LOG_MAX_FILES");
        cfg.console_enabled = requireBool("LOG_CONSOLE_ENABLED");
        return cfg;
    }

    RedisConfig redis() const {
        RedisConfig cfg;
        cfg.host = requireString("REDIS_HOST");
        cfg.port = requireInt("REDIS_PORT");
        cfg.db = requireInt("REDIS_DB");
        cfg.username = requireString("REDIS_GATEWAY_USERNAME");
        cfg.password = requireString("REDIS_GATEWAY_PASSWORD");
        cfg.connect_timeout_seconds = requireInt("REDIS_CONNECT_TIMEOUT_SECONDS");
        cfg.command_timeout_seconds = requireInt("REDIS_COMMAND_TIMEOUT_SECONDS");
        cfg.pool_size = requireInt("REDIS_POOL_SIZE");
        requirePositive("REDIS_PORT", cfg.port);
        requirePositive("REDIS_POOL_SIZE", cfg.pool_size);
        if (cfg.db < 0) {
            throw ConfigException("REDIS_DB 必须 >= 0");
        }
        requirePositive("REDIS_CONNECT_TIMEOUT_SECONDS", static_cast<int>(cfg.connect_timeout_seconds));
        requirePositive("REDIS_COMMAND_TIMEOUT_SECONDS", static_cast<int>(cfg.command_timeout_seconds));
        return cfg;
    }

    PaymentBackendConfig paymentBackend() const {
        PaymentBackendConfig cfg;
        cfg.ip = requireString("PAYMENT_BACKEND_IP");
        cfg.http_port = requireInt("PAYMENT_BACKEND_HTTP_PORT");
        requirePositive("PAYMENT_BACKEND_HTTP_PORT", cfg.http_port);
        return cfg;
    }

    PaymentGatewayConfig paymentGateway() const {
        PaymentGatewayConfig cfg;
        cfg.websocket_port = requireInt("PAYMENT_GATEWAY_WEBSOCKET_PORT");
        cfg.http_port = requireInt("PAYMENT_GATEWAY_HTTP_PORT");
        requirePositive("PAYMENT_GATEWAY_WEBSOCKET_PORT", cfg.websocket_port);
        requirePositive("PAYMENT_GATEWAY_HTTP_PORT", cfg.http_port);
        return cfg;
    }

    // review
    PaymentNginxConfig paymentNginx() const {
        PaymentNginxConfig cfg;
        cfg.ip = requireString("PAYMENT_NGINX_IP");
        cfg.port = requireInt("PAYMENT_NGINX_PORT");
        requirePositive("PAYMENT_NGINX_PORT", cfg.port);
        return cfg;
    }

  private:
    Config() = default;

    std::optional<std::string> findString(const std::string& key) const {
        auto it = values_.find(key);
        if (it == values_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::string requireString(const std::string& key) const {
        auto value = findString(key);
        if (!value.has_value() || value->empty()) {
            throw ConfigException("缺少必需配置项: " + key);
        }
        return *value;
    }

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

    static void requirePositive(const std::string& key, int value) {
        if (value <= 0) {
            throw ConfigException(key + " 必须为正数");
        }
    }

    static std::string trim(const std::string& input) {
        std::size_t start = input.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) {
            return "";
        }
        std::size_t end = input.find_last_not_of(" \t\r\n");
        return input.substr(start, end - start + 1);
    }

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
