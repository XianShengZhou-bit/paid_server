// review
#pragma once

#include <ctime>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace gateway_session {

class SessionStore {
  public:
    // review
    static SessionStore& instance() {
        static SessionStore store;
        return store;
    }

    // review
    std::string create(const std::string& user_id) {
        const std::string session_id = makeSessionId();
        std::lock_guard<std::mutex> lock(mu_);
        sessions_[session_id] = user_id;
        return session_id;
    }

    // review
    std::optional<std::string> userIdOf(const std::string& session_id) const {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    // review
    void remove(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(mu_);
        sessions_.erase(session_id);
    }

  private:
    // review
    SessionStore() = default;

    // review
    static std::string makeSessionId() {
        static int counter = 0;
        return "SID" + std::to_string(++counter) +
               std::to_string(std::time(nullptr)); // 暂时先这样，后续增加随机数(后续会用redis缓存)
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, std::string> sessions_;
};

} // namespace gateway_session
