// review
#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <nlohmann/json.hpp>

#include "config.hpp"

namespace payment_jwt {

class JwtException : public std::runtime_error {
  public:
    // review
    explicit JwtException(const std::string& message) : std::runtime_error(message) {}
};

// review
// 支付二维码 Token 的业务载荷结构体，用于生成和解析 JWT
struct PaymentQrPayload {
    // 支付会话 ID，对应 payment_sessions.payment_session_id
    std::string payment_session_id;

    // Token 签发时间，Unix 时间戳，单位秒
    int64_t iat = 0;

    // Token 过期时间，Unix 时间戳，单位秒
    int64_t exp = 0;

    // 随机字符串，用于让同一时间生成的 Token 不完全相同
    std::string nonce;
};

// review
// 支付二维码 Token 校验结果结构体，用于返回验签、过期校验和载荷解析结果
struct PaymentQrVerifyResult {
    // 校验是否通过；true 表示 Token 合法，false 表示 Token 非法或已过期
    bool valid;

    // 校验失败时的错误信息；成功时为空字符串
    std::string error_message;

    // 校验成功后解析出的二维码 Token 业务载荷
    PaymentQrPayload payload;
};

class HashService {
  public:
    // review
    static HashService fromConfig(const payment_config::Config& config = payment_config::Config::instance()) {
        return HashService(config.hash());
    }

    // review
    std::string hashEmail(const std::string& email) const {
        return hmacSha256Hex(normalizeEmail(email), cfg_.user_email_hash_secret);
    }

    // review
    std::string hashIdCard(const std::string& id_card) const {
        return hmacSha256Hex(normalizeIdCard(id_card), cfg_.user_id_card_hash_secret);
    }

    // review
    // 标准化邮箱：去除首尾空格并统一转小写；格式非法时抛出 JwtException。
    static std::string normalizeEmail(const std::string& email) {
        std::string normalized = trim(email);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!isValidEmailFormat(normalized)) {
            throw JwtException("邮箱格式错误");
        }
        return normalized;
    }

    // review
    // 标准化身份证号：去除首尾空格并统一转大写；格式非法时抛出 JwtException。
    static std::string normalizeIdCard(const std::string& id_card) {
        std::string normalized = trim(id_card);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (!isValidIdCardFormat(normalized)) {
            throw JwtException("身份证号格式错误");
        }
        return normalized;
    }

    // review
    static bool isValidEmailFormat(const std::string& email) {
        if (email.empty() || email.size() > 254) {
            return false;
        }
        const std::size_t at_pos = email.find('@');
        if (at_pos == std::string::npos || at_pos == 0 || at_pos == email.size() - 1) {
            return false;
        }
        const std::string local = email.substr(0, at_pos);
        const std::string domain = email.substr(at_pos + 1);
        if (local.empty() || domain.empty() || domain.find('.') == std::string::npos) {
            return false;
        }
        if (local.front() == '.' || local.back() == '.' || domain.front() == '.' || domain.back() == '.') {
            return false;
        }
        return true;
    }

    // review
    static bool isValidIdCardFormat(const std::string& id_card) {
        if (id_card.size() == 15) { // 一代身份证
            return std::all_of(id_card.begin(), id_card.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
        }
        if (id_card.size() != 18) { // 二代身份证
            return false;
        }
        for (std::size_t i = 0; i < 17; ++i) {
            if (std::isdigit(static_cast<unsigned char>(id_card[i])) == 0) {
                return false;
            }
        }
        const char last = id_card[17];
        return std::isdigit(static_cast<unsigned char>(last)) != 0 || last == 'X';
    }

    // review
    // 对邮箱进行脱敏处理，例如 user@example.com -> use****@example.com
    static std::string maskEmail(const std::string& email) {

        // 查找邮箱中的 @ 符号位置，用于拆分邮箱本地部分和域名部分
        const std::size_t at_pos = email.find('@');

        // 如果没有找到 @，说明邮箱格式不合法，直接返回通用脱敏占位符
        if (at_pos == std::string::npos) {
            return "****";
        }

        // 提取 @ 前面的本地部分，例如 user@example.com 中的 user
        std::string local = email.substr(0, at_pos);

        // 提取 @ 及其后面的域名部分，例如 user@example.com 中的 @example.com
        std::string domain = email.substr(at_pos);

        // 如果本地部分为空，例如 @example.com，则只隐藏本地部分并保留域名
        if (local.empty()) {
            return "****" + domain;
        }

        // 如果本地部分长度小于等于 2，只保留第 1 个字符，其余用 **** 替代
        if (local.size() <= 2) {
            return local.substr(0, 1) + "****" + domain;
        }

        // 如果本地部分长度大于 2，则保留前 3 个字符，其余用 **** 替代，并拼接域名
        return local.substr(0, 3 > local.size() ? local.size() : 3) + "****" + domain;
    }

    // review
    // 对身份证号进行脱敏处理，例如 4403**************00 -> 4403**************00
    static std::string maskIdCard(const std::string& id_card) {
        if (id_card.size() < 6) {
            return "****";
        }
        if (id_card.size() <= 8) {
            return id_card.substr(0, 2) + "****" + id_card.substr(id_card.size() - 2);
        }
        return id_card.substr(0, 4) + std::string(id_card.size() - 6, '*') + id_card.substr(id_card.size() - 2);
    }

    // review
    // 进行哈希计算，并返回十六进制字符串(用于生成身份证号码等哈希值)
    static std::string hmacSha256Hex(const std::string& data, const std::string& secret) {
        // EVP_MAX_MD_SIZE宏是OpenSSL 支持的摘要算法中，摘要结果可能需要的最大缓冲区大小。
        unsigned int len = EVP_MAX_MD_SIZE;

        unsigned char digest[EVP_MAX_MD_SIZE];

        // 调用 OpenSSL 的 HMAC 函数，使用 SHA-256 算法和 secret 密钥对 data 计算
        HMAC(EVP_sha256(), reinterpret_cast<const unsigned char*>(secret.data()), static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest, &len);

        return toHex(digest, len);
    }

  private:
    payment_config::HmacHashConfig cfg_;

    // review
    explicit HashService(payment_config::HmacHashConfig cfg) : cfg_(std::move(cfg)) {}

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
    static std::string toHex(const unsigned char* data, unsigned int len) {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (unsigned int i = 0; i < len; ++i) {
            oss << std::setw(2) << static_cast<int>(data[i]);
        }
        return oss.str();
    }
};

// JWT 服务仅负责二维码 Token 的签发与校验。
class JwtService {
  public:
    // review
    static JwtService fromConfig(const payment_config::Config& config = payment_config::Config::instance()) {
        return JwtService(config.jwt());
    }

    // review
    static JwtService fromJwtConfig(const payment_config::JwtConfig& cfg) {
        return JwtService(cfg);
    }

    // review
    // 生成支付二维码 JWT Token；入参 payload 是二维码 Token 的业务载荷，返回 JWT Token 字符串
    std::string issuePaymentQrToken(PaymentQrPayload payload) const {
        validatePaymentQrPayloadForIssue(payload);
        const int64_t now = nowSeconds();
        payload.iat = now;
        payload.exp = payload.iat + qr_ttl_seconds_;
        payload.nonce = secureNonceHex(16);

        // 构造 JWT Payload 的 JSON 字段列表；后续 issueToken 会把它编码成 JSON 并签名
        const nlohmann::json body = {{"payment_session_id", payload.payment_session_id},
                                     {"iat", payload.iat},
                                     {"exp", payload.exp},
                                     {"nonce", payload.nonce}};

        // 调用底层 Token 签发函数：生成 header、payload、signature，并拼接成完整 JWT 字符串
        return issueToken(body);
    }

    // review
    // 校验支付二维码 JWT Token，并返回结构化校验结果(这边二维码内容是从前端来的，必须要经过校验证明合法性)
    PaymentQrVerifyResult verifyPaymentQrToken(const std::string& token) const {
        PaymentQrVerifyResult result;
        std::string payload_json;
        std::string error;

        // 校验 JWT 签名是否合法，并将 Payload 解析到 body 中
        if (!verifyToken(token, payload_json, error)) {
            result.valid = false;
            result.error_message = error;
            return result;
        }

        PaymentQrPayload payload;
        // 将 body 中的键值对转换为 PaymentQrPayload 结构体
        bool res = parsePaymentQrPayloadJson(payload_json, payload, error);

        // 判断 Payload 是否解析成功；如果缺少必要字段或字段类型错误，则解析失败
        if (!res) {
            result.error_message = error;
            result.valid = false;
            return result;
        }

        // 校验 Token 是否已经过期；clock_skew_seconds_ 用于允许少量服务器时钟误差
        if (payload.exp + clock_skew_seconds_ <= nowSeconds()) {
            result.error_message = "支付二维码 Token 已过期";
            result.valid = false;
            return result;
        }

        // 走到这里说明 JWT 格式、签名、Payload、类型、过期时间均校验通过
        result.valid = true;
        result.payload = payload;
        return result;
    }

  private:
    std::string algorithm_;
    std::string secret_;
    int qr_ttl_seconds_;
    int clock_skew_seconds_;

    // review
    explicit JwtService(payment_config::JwtConfig cfg)
        : algorithm_(std::move(cfg.algorithm)), secret_(std::move(cfg.secret)),
          qr_ttl_seconds_(cfg.qr_token_ttl_seconds), clock_skew_seconds_(cfg.clock_skew_seconds) {
        if (secret_.empty()) {
            throw JwtException("JWT_SECRET 不能为空");
        }
        if (algorithm_ != "HS256" && algorithm_ != "HS384" && algorithm_ != "HS512") {
            throw JwtException("不支持的 JWT 算法: " + algorithm_);
        }
        if (qr_ttl_seconds_ <= 0) {
            throw JwtException("JWT 二维码 Token 有效期必须为正数");
        }
    }

    // review
    // 负责签发 JWT。
    // 过程：Header + Payload -> HMAC 签名 -> 生成 {header.payload.signature}
    std::string issueToken(const nlohmann::json& payload_body) const {
        const nlohmann::json header_body = {{"alg", algorithm_}, {"typ", "JWT"}};

        const std::string header_part = base64UrlEncode(header_body.dump());
        const std::string payload_part = base64UrlEncode(payload_body.dump());
        const std::string signing_input = header_part + "." + payload_part;
        const std::string signature_part = base64UrlEncode(hmacRaw(signing_input));
        return signing_input + "." +
               signature_part; // 返回内容具体为：header(用的什么算法).payload(body字段数据).signature(签名)
        // 如：{"alg":"HS256","typ":"JWT"}.{"payment_session_id":"PS202606291234567890","iat":1719859200,"exp":1719862800,"nonce":"1234567890"}.jwt签名
    }

    // review
    // 负责校验 JWT，并返回 Payload JSON 字符串。
    // 过程：拆分 {header.payload.signature} -> 校验 header 算法 -> 重新计算签名 -> 对比签名 -> 返回 payload_json
    bool verifyToken(const std::string& token, std::string& payload_json, std::string& error) const {
        try {
            const std::vector<std::string> parts = split(token, '.');

            if (parts.size() != 3 || parts[0].empty() || parts[1].empty() || parts[2].empty()) {
                error = "JWT Token 格式非法";
                return false;
            }

            const std::string header_json_text = base64UrlDecode(parts[0]);
            const nlohmann::json header_json = nlohmann::json::parse(header_json_text);

            if (!header_json.is_object()) {
                error = "JWT Header 不是 JSON 对象";
                return false;
            }

            const std::string typ = header_json.at("typ").get<std::string>();

            if (typ != "JWT") {
                error = "JWT 类型不匹配";
                return false;
            }

            const std::string alg = header_json.at("alg").get<std::string>();

            if (alg != algorithm_) {
                error = "JWT 算法不匹配";
                return false;
            }

            const std::string signing_input = parts[0] + "." + parts[1];
            const std::string expected_signature = base64UrlEncode(hmacRaw(signing_input));

            if (!constantTimeEqual(expected_signature, parts[2])) {
                error = "JWT Token 签名非法";
                return false;
            }

            payload_json = base64UrlDecode(parts[1]);

            if (payload_json.empty()) {
                error = "JWT Payload 为空";
                return false;
            }

            return true;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }

    // review
    static int64_t nowSeconds() {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // review
    static void validatePaymentQrPayloadForIssue(const PaymentQrPayload& payload) {
        if (payload.payment_session_id.empty()) {
            throw JwtException("payment_session_id 不能为空");
        }
    }

    // review
    // 生成 JWT 第三段签名
    std::string hmacRaw(const std::string& data) const {
        const EVP_MD* md = nullptr;
        if (algorithm_ == "HS256") {
            md = EVP_sha256();
        } else if (algorithm_ == "HS384") {
            md = EVP_sha384();
        } else if (algorithm_ == "HS512") {
            md = EVP_sha512();
        } else {
            throw JwtException("不支持的 JWT 算法: " + algorithm_);
        }

        unsigned int len = 0;
        unsigned char digest[EVP_MAX_MD_SIZE];

        unsigned char* result =
            HMAC(md, reinterpret_cast<const unsigned char*>(secret_.data()), static_cast<int>(secret_.size()),
                 reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest, &len);

        if (result == nullptr) {
            throw JwtException("JWT HMAC 签名计算失败");
        }

        return std::string(reinterpret_cast<char*>(digest), len);
    }

    // review
    // 生成安全随机 nonce
    static std::string secureNonceHex(std::size_t bytes_len) {
        std::vector<unsigned char> bytes(bytes_len);
        if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
            throw JwtException("生成 nonce 时 RAND_bytes 调用失败");
        }
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (unsigned char b : bytes) {
            oss << std::setw(2) << static_cast<int>(b);
        }
        return oss.str();
    }

    // review
    static bool constantTimeEqual(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) {
            return false;
        }
        return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
    }

    // review
    // 将字符串编码为 base64url 格式
    static std::string base64UrlEncode(const std::string& input) {
        if (input.empty()) {
            return "";
        }

        std::string out;
        out.resize(4 * ((input.size() + 2) / 3));
        const int len =
            EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]),
                            reinterpret_cast<const unsigned char*>(input.data()), static_cast<int>(input.size()));
        out.resize(len);
        for (char& c : out) {
            if (c == '+')
                c = '-';
            else if (c == '/')
                c = '_';
        }
        while (!out.empty() && out.back() == '=') {
            out.pop_back();
        }
        return out;
    }

    // review
    // 将 base64url 字符串解码为原始字符串
    static std::string base64UrlDecode(std::string input) {
        for (char& c : input) {
            if (c == '-')
                c = '+';
            else if (c == '_')
                c = '/';
        }
        while (input.size() % 4 != 0) {
            input.push_back('=');
        }

        std::string out;
        out.resize((input.size() / 4) * 3);
        const int len =
            EVP_DecodeBlock(reinterpret_cast<unsigned char*>(&out[0]),
                            reinterpret_cast<const unsigned char*>(input.data()), static_cast<int>(input.size()));
        if (len < 0) {
            throw JwtException("base64url 解码失败");
        }
        std::size_t padding = 0;
        if (!input.empty() && input[input.size() - 1] == '=')
            padding++;
        if (input.size() >= 2 && input[input.size() - 2] == '=')
            padding++;
        out.resize(static_cast<std::size_t>(len) - padding);
        return out;
    }

    // review
    // 解析支付二维码 JWT Payload JSON。
    // 成功返回 true，并写入 payload；失败返回 false，并写入 error。
    static bool parsePaymentQrPayloadJson(const std::string& json_text, PaymentQrPayload& payload, std::string& error) {
        try {
            const nlohmann::json j = nlohmann::json::parse(json_text);

            if (!j.is_object()) {
                error = "JWT Payload 不是 JSON 对象";
                return false;
            }

            PaymentQrPayload parsed;
            parsed.payment_session_id = j.at("payment_session_id").get<std::string>();
            parsed.iat = j.at("iat").get<int64_t>();
            parsed.exp = j.at("exp").get<int64_t>();
            parsed.nonce = j.at("nonce").get<std::string>();

            if (parsed.payment_session_id.empty()) {
                error = "payment_session_id 不能为空";
                return false;
            }

            if (parsed.nonce.empty()) {
                error = "nonce 不能为空";
                return false;
            }

            if (parsed.iat <= 0 || parsed.exp <= 0 || parsed.exp <= parsed.iat) {
                error = "JWT Payload 时间字段非法";
                return false;
            }

            payload = std::move(parsed);
            return true;
        } catch (const std::exception& e) {
            error = std::string("支付二维码 JWT Payload 解析失败: ") + e.what();
            return false;
        }
    }

    // review
    //  按指定分隔符拆分字符串
    static std::vector<std::string> split(const std::string& s, char delimiter) {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : s) {
            if (c == delimiter) {
                parts.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        parts.push_back(cur);
        return parts;
    }
};

} // namespace payment_jwt