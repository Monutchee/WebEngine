#pragma once
#include <chrono>
#include <iomanip>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <mutex>

#include <openssl/rand.h>

#include "webengine/Role.hpp"

namespace webengine {

struct TokenEntry {
    std::string                           username;
    Role                                  role;
    std::chrono::steady_clock::time_point expires_at;
};

// Thread-safe in-memory session-token store. Tokens are 256 bits of CSPRNG
// output, hex-encoded.
class TokenStore {
public:
    std::string issue(const std::string& username, Role role,
                      std::chrono::seconds ttl = std::chrono::hours(8))
    {
        unsigned char buf[32];
        // Never mint a token from uninitialised memory: RAND_bytes returns 1 only
        // on success. On failure the caller turns this into a 5xx (see Router).
        if (RAND_bytes(buf, sizeof(buf)) != 1)
            throw std::runtime_error("TokenStore: RAND_bytes failed");

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

} // namespace webengine
