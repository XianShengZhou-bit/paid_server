// review
#pragma once

#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/fmt/fmt.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace logger {

class Logger {
  public:
    // review
    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }

    // review
    bool init(bool debug,                                   // 控制“写不写 DEBUG 日志”
              const std::string& log_file,                  // 日志文件路径
              std::size_t max_file_size = 10 * 1024 * 1024, // 最大日志文件大小
              std::size_t max_files = 5,                    // 最大日志文件数量
              bool console_enabled = true                   // 是否在控制台输出日志
    ) {
        if (initialized_) {
            return false;
        }

        debug_ = debug;

        // spdlog 官方 wiki 明确说明：sink 是实际把日志写到目标位置的对象，每个 logger 可以包含一个或多个 sink。
        std::vector<spdlog::sink_ptr> sinks;

        sinks.reserve(console_enabled ? 2 : 1);

        if (console_enabled) {
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            console_sink->set_level(debug_ ? spdlog::level::debug : spdlog::level::info);
            sinks.push_back(console_sink);
        } // 首先是控制台显示日志

        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(log_file, max_file_size, max_files);
        file_sink->set_level(debug_ ? spdlog::level::debug : spdlog::level::info);
        sinks.push_back(file_sink);
        // 其次是文件显示日志

        logger_ = std::make_shared<spdlog::logger>("payment_service", sinks.begin(), sinks.end());
        logger_->set_level(debug_ ? spdlog::level::debug
                                  : spdlog::level::info); // 日志要真正输出，必须同时通过 logger 级别和 sink 级别。
        logger_->set_pattern("%Y-%m-%d %H:%M:%S.%e | %-5l | %t | [%s:%#] | %v");
        logger_->flush_on(spdlog::level::warn);

        spdlog::register_logger(logger_);

        initialized_ = true;
        return true;
    }

    // review
    void shutdown() {
        if (logger_) {
            logger_->flush();
            spdlog::drop(logger_->name());
            logger_.reset();
        }
        initialized_ = false;
    }

    // review
    template <typename... Args> void debug(const char* file, int line, const char* fmt_text, Args&&... args) {
        write(spdlog::level::debug, file, line, fmt_text, std::forward<Args>(args)...);
    }

    // review
    template <typename... Args> void info(const char* file, int line, const char* fmt_text, Args&&... args) {
        write(spdlog::level::info, file, line, fmt_text, std::forward<Args>(args)...);
    }

    // review
    template <typename... Args> void warn(const char* file, int line, const char* fmt_text, Args&&... args) {
        write(spdlog::level::warn, file, line, fmt_text, std::forward<Args>(args)...);
    }

    // review
    template <typename... Args> void error(const char* file, int line, const char* fmt_text, Args&&... args) {
        write(spdlog::level::err, file, line, fmt_text, std::forward<Args>(args)...);
    }

    // review
    void flush() {
        if (logger_) {
            logger_->flush();
        }
    }

    // review
    // 日志脱敏函数
    static std::string sanitize(const std::string& input) {
        static const std::regex jwt_pattern(R"(([A-Za-z0-9_-]{10,})\.([A-Za-z0-9_-]{10,})\.([A-Za-z0-9_-]{10,}))");

        static const std::regex hash_pattern(R"(\b[a-fA-F0-9]{48,128}\b)");

        static const std::regex key_value_sensitive_pattern(
            R"((("?)(password|login_password|pay_password|token|qr_token|payment_auth_token|secret|id_card|id_card_hash|email_hash)\2\s*[=:]\s*"?)[^,"\s\}\]]+)");

        static const std::regex header_sensitive_pattern(R"(((Authorization|Cookie)\s*:\s*)[^\r\n,]+)");

        std::string out = input;
        out = std::regex_replace(out, jwt_pattern, "<JWT_REDACTED>");
        out = std::regex_replace(out, hash_pattern, "<HASH_REDACTED>");
        out = std::regex_replace(out, key_value_sensitive_pattern, "$1<REDACTED>");
        out = std::regex_replace(out, header_sensitive_pattern, "$1<REDACTED>");

        return out;
    }

  private:
    // review
    Logger() = default;
    
    // review
    ~Logger()
    {
        shutdown();
    }

    // review
    Logger(const Logger&) = delete;
    // review
    Logger& operator=(const Logger&) = delete;

    // review
    template <typename... Args>
    void write(spdlog::level::level_enum level, const char* file, int line, const char* fmt_text, Args&&... args) {
        if (level == spdlog::level::debug && !debug_) {
            return;
        }

        std::string message = formatMessage(fmt_text, std::forward<Args>(args)...);
        message = sanitize(message);

        if (!logger_) {
            throw std::runtime_error("Logger not initialized");
        }

        logger_->log(spdlog::source_loc{file, line, ""}, level, "{}", message);
    }

    // review
    template <typename... Args> static std::string formatMessage(const char* fmt_text, Args&&... args) {
        try {
            return fmt::format(fmt::runtime(fmt_text), std::forward<Args>(args)...);
        } catch (const std::exception& e) {
            return fmt::format("日志格式化错误: {}; 原始格式={}", e.what(), fmt_text);
        }
    }

    std::shared_ptr<spdlog::logger> logger_;
    bool debug_ = false;
    bool initialized_ = false;
};

} // namespace logger

#define LOG_DEBUG(...) logger::Logger::getInstance().debug(__FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...) logger::Logger::getInstance().info(__FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...) logger::Logger::getInstance().warn(__FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) logger::Logger::getInstance().error(__FILE__, __LINE__, __VA_ARGS__)