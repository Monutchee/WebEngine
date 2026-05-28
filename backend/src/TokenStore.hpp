#pragma once
#include <chrono>
#include <iomanip>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>

#include <openssl/rand.h>

#include "AclStore.hpp"

struct TokenEntry {
    std::string                              username;
    Role                                     role;
    std::chrono::steady_clock::time_point    expires_at;
};

class TokenStore {
public:
    std::string issue(const std::string& username, Role role,
                      std::chrono::seconds ttl = std::chrono::hours(8))
    {
        unsigned char buf[32];
        RAND_bytes(buf, sizeof(buf));

        std::ostringstream oss;
        for (unsigned char b : buf)
            oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
        std::string token = oss.str();

        std::unique_lock lock(mutex_);
        store_[token] = {username, role, std::chrono::steady_clock::now() + ttl};
        return token;
    }

    std::optional<TokenEntry> validate(const std::string& token) const {
        std::shared_lock lock(mutex_);
        auto it = store_.find(token);
        if (it == store_.end()) return std::nullopt;
        if (it->second.expires_at < std::chrono::steady_clock::now())
            return std::nullopt;
        return it->second;
    }

    void revoke(const std::string& token) {
        std::unique_lock lock(mutex_);
        store_.erase(token);
    }

private:
    std::unordered_map<std::string, TokenEntry> store_;
    mutable std::shared_mutex                   mutex_;
};
