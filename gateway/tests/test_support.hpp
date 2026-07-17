// review
#pragma once

#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

#include <hiredis/hiredis.h>

#include "config.hpp"

namespace gateway_test {

// review
inline std::string& envFilePath() {
    static std::string path;
    return path;
}

// review
inline std::map<std::string, std::string> loadEnvMap(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("无法打开 env: " + path);
    }
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto hash = line.find('#');
        if (hash != std::string::npos) {
            line = line.substr(0, hash);
        }
        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        auto key = line.substr(0, pos);
        auto value = line.substr(pos + 1);
        while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) {
            key.erase(key.begin());
        }
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
            key.pop_back();
        }
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.erase(value.begin());
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
            value.pop_back();
        }
        if (!key.empty()) {
            values[key] = value;
        }
    }
    return values;
}

// 用后端 Redis 写账号写入 session，供网关只读账号 GET（对齐 CI ACL）。
// review
inline void writeSessionAsBackend(const std::string& session_id, const std::string& user_id, int ttl_seconds = 60) {
    const auto env = loadEnvMap(envFilePath());
    const std::string host = env.at("REDIS_HOST");
    const int port = std::stoi(env.at("REDIS_PORT"));
    const int db = std::stoi(env.at("REDIS_DB"));
    const std::string username = env.at("REDIS_BACKEND_USERNAME");
    const std::string password = env.at("REDIS_BACKEND_PASSWORD");

    redisContext* conn = redisConnect(host.c_str(), port);
    if (conn == nullptr || conn->err != 0) {
        throw std::runtime_error("测试写入 Redis 连接失败");
    }
    redisReply* auth = static_cast<redisReply*>(redisCommand(conn, "AUTH %s %s", username.c_str(), password.c_str()));
    if (auth == nullptr || auth->type == REDIS_REPLY_ERROR) {
        freeReplyObject(auth);
        redisFree(conn);
        throw std::runtime_error("测试写入 Redis AUTH 失败（请用 backend ACL 账号）");
    }
    freeReplyObject(auth);

    redisReply* select = static_cast<redisReply*>(redisCommand(conn, "SELECT %d", db));
    freeReplyObject(select);

    const std::string key = "payment:session:" + session_id;
    redisReply* setex =
        static_cast<redisReply*>(redisCommand(conn, "SET %s %s EX %d", key.c_str(), user_id.c_str(), ttl_seconds));
    if (setex == nullptr || setex->type == REDIS_REPLY_ERROR) {
        freeReplyObject(setex);
        redisFree(conn);
        throw std::runtime_error("测试写入 Redis SETEX 失败");
    }
    freeReplyObject(setex);
    redisFree(conn);
}

// review
inline void deleteSessionAsBackend(const std::string& session_id) {
    const auto env = loadEnvMap(envFilePath());
    const std::string host = env.at("REDIS_HOST");
    const int port = std::stoi(env.at("REDIS_PORT"));
    const int db = std::stoi(env.at("REDIS_DB"));
    const std::string username = env.at("REDIS_BACKEND_USERNAME");
    const std::string password = env.at("REDIS_BACKEND_PASSWORD");

    redisContext* conn = redisConnect(host.c_str(), port);
    if (conn == nullptr || conn->err != 0) {
        return;
    }
    redisReply* auth = static_cast<redisReply*>(redisCommand(conn, "AUTH %s %s", username.c_str(), password.c_str()));
    freeReplyObject(auth);
    redisReply* select = static_cast<redisReply*>(redisCommand(conn, "SELECT %d", db));
    freeReplyObject(select);
    const std::string key = "payment:session:" + session_id;
    redisReply* del = static_cast<redisReply*>(redisCommand(conn, "DEL %s", key.c_str()));
    freeReplyObject(del);
    redisFree(conn);
}

// review
inline std::string nginxIp() {
    return payment_config::Config::instance().paymentNginx().ip;
}

// review
inline std::string backendIp() {
    return payment_config::Config::instance().paymentBackend().ip;
}

} // namespace gateway_test
