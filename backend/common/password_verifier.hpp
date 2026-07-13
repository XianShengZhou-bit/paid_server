// review
#pragma once

#include <iomanip>
#include <sstream>
#include <string>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace payment_security {

// 登录密码哈希：users.password_hash 只保存一个 HMAC-SHA256 十六进制摘要值。
class PasswordVerifier {
  public:
    // review
    static bool verifyLoginPasswordWithHash(const std::string& plain_password, const std::string& password_hash,
                                            const std::string& secret) {
        if (plain_password.empty() || password_hash.empty()) {
            return false;
        }

        if (secret.empty() || password_hash.size() != 64) {
            return false;
        }

        return constantTimeEqual(hashLoginPassword(plain_password, secret), password_hash);
    }

  private:
    // review
    static std::string hashLoginPassword(const std::string& plain_password, const std::string& secret) {
        if (plain_password.empty() || secret.empty()) {
            return "";
        }
        return hmacSha256Hex(plain_password, secret);
    }

    // review
    static bool constantTimeEqual(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) {
            return false;
        }
        return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
    }

    // review
    static std::string hmacSha256Hex(const std::string& data, const std::string& secret) {
        unsigned int len = EVP_MAX_MD_SIZE;
        unsigned char digest[EVP_MAX_MD_SIZE];

        HMAC(EVP_sha256(), reinterpret_cast<const unsigned char*>(secret.data()), static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest, &len);

        return toHex(digest, len);
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

} // namespace payment_security
