#pragma once
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "AuthProvider.hpp"

namespace webengine {

// A ready-to-use, in-memory AuthProvider for examples, tests and PoCs.
//
// Credentials are kept in a hash map; passwords are stored as salted PBKDF2
// hashes (never plaintext) and verified in constant time. The default
// constructor injects a few test accounts so the demo works out of the box:
//
//     admin  / admin123   (Role::Admin)
//     alice  / user123     (Role::User)
//     viewer / viewer123   (Role::Viewer)
//
// Swap this out for your own AuthProvider (config file, database, …) in
// production — the engine does not care which provider it is given.
class TestAuthProvider : public AuthProvider {
public:
    TestAuthProvider() {
        // Seed test accounts. Replace with a config/DB-backed provider for
        // production; the engine API is identical either way.
        seed_user("admin",  "admin123",  Role::Admin);
        seed_user("alice",  "user123",   Role::User);
        seed_user("viewer", "viewer123", Role::Viewer);
    }

    std::optional<Role>
    authenticate(const std::string& username, const std::string& password) override
    {
        // Fetch the stored hash under the lock, then verify outside it. PBKDF2
        // is intentionally slow; holding the lock during it would serialise
        // every login on this provider.
        std::string stored_hash;
        Role        role;
        {
            std::shared_lock lock(mutex_);
            auto it = users_.find(username);
            if (it == users_.end()) return std::nullopt;
            stored_hash = it->second.password_hash;
            role        = it->second.role;
        }

        if (!pbkdf2_verify(password, stored_hash)) return std::nullopt;
        return role;
    }

    bool supports_management() const override { return true; }

    bool add_user(const std::string& username, const std::string& password, Role role) override {
        seed_user(username, password, role);
        return true;
    }

    bool remove_user(const std::string& username) override {
        std::unique_lock lock(mutex_);
        return users_.erase(username) > 0;
    }

    bool set_user_role(const std::string& username, Role role) override {
        std::unique_lock lock(mutex_);
        auto it = users_.find(username);
        if (it == users_.end()) return false;
        it->second.role = role;
        return true;
    }

    std::vector<UserInfo> list_users() const override {
        std::shared_lock lock(mutex_);
        std::vector<UserInfo> out;
        out.reserve(users_.size());
        for (const auto& [name, rec] : users_)
            out.push_back({name, rec.role});
        return out;
    }

private:
    struct UserRecord {
        std::string password_hash; // "iterations:salt_hex:key_hex"
        Role        role;
    };

    // Non-virtual upsert, safe to call from the constructor.
    void seed_user(const std::string& username, const std::string& password, Role role) {
        std::string hash = pbkdf2_hash(password); // computed outside the lock — slow
        std::unique_lock lock(mutex_);
        users_[username] = {std::move(hash), role};
    }

    static constexpr int ITERATIONS = 200'000;
    static constexpr int SALT_LEN   = 16;
    static constexpr int KEY_LEN    = 32;

    static std::string to_hex(const unsigned char* data, int len) {
        std::string out(len * 2, '\0');
        for (int i = 0; i < len; ++i)
            snprintf(&out[i * 2], 3, "%02x", data[i]);
        return out;
    }

    static std::vector<unsigned char> from_hex(const std::string& hex) {
        std::vector<unsigned char> out(hex.size() / 2);
        for (size_t i = 0; i < out.size(); ++i) {
            unsigned int b = 0;
            sscanf(hex.c_str() + i * 2, "%02x", &b);
            out[i] = static_cast<unsigned char>(b);
        }
        return out;
    }

    // Returns "iterations:salt_hex:key_hex".
    static std::string pbkdf2_hash(const std::string& password) {
        unsigned char salt[SALT_LEN];
        if (RAND_bytes(salt, SALT_LEN) != 1)
            throw std::runtime_error("TestAuthProvider: RAND_bytes failed");

        unsigned char key[KEY_LEN];
        PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                          salt, SALT_LEN, ITERATIONS, EVP_sha256(),
                          KEY_LEN, key);

        return std::to_string(ITERATIONS)
             + ":" + to_hex(salt, SALT_LEN)
             + ":" + to_hex(key,  KEY_LEN);
    }

    // Verifies a password against "iterations:salt_hex:key_hex".
    // Uses CRYPTO_memcmp to prevent timing-based attacks.
    static bool pbkdf2_verify(const std::string& password, const std::string& stored) {
        auto p1 = stored.find(':');
        auto p2 = stored.find(':', p1 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos) return false;

        int iterations;
        try { iterations = std::stoi(stored.substr(0, p1)); }
        catch (...) { return false; }

        auto salt     = from_hex(stored.substr(p1 + 1, p2 - p1 - 1));
        auto expected = from_hex(stored.substr(p2 + 1));
        if (expected.size() != KEY_LEN) return false;

        unsigned char computed[KEY_LEN];
        PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                          salt.data(), static_cast<int>(salt.size()),
                          iterations, EVP_sha256(),
                          KEY_LEN, computed);

        return CRYPTO_memcmp(computed, expected.data(), KEY_LEN) == 0;
    }

    std::unordered_map<std::string, UserRecord> users_;
    mutable std::shared_mutex                   mutex_;
};

} // namespace webengine
