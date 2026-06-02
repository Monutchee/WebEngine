#pragma once
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

enum class Role { Guest = 0, Viewer = 1, User = 2, Admin = 3 };

inline const char* role_name(Role r) {
    switch (r) {
        case Role::Admin:  return "admin";
        case Role::User:   return "user";
        case Role::Viewer: return "viewer";
        default:           return "guest";
    }
}

inline bool role_from_string(const std::string& s, Role& out) {
    if (s == "admin")  { out = Role::Admin;  return true; }
    if (s == "user")   { out = Role::User;   return true; }
    if (s == "viewer") { out = Role::Viewer; return true; }
    if (s == "guest")  { out = Role::Guest;  return true; }
    return false;
}

struct UserRecord {
    std::string password_hash; // "iterations:salt_hex:key_hex"
    Role        role;
};

struct AclEntry {
    std::string path_prefix;
    Role        min_role;
};

class AclStore {
public:
    AclStore() {
        // Seed users — replace with config-file load for production.
        // Store pre-computed hashes to avoid running PBKDF2 on every startup.
        add_user("admin",  "admin123",  Role::Admin);
        add_user("alice",  "user123",   Role::User);
        add_user("viewer", "viewer123", Role::Viewer);

        // Default ACL: all protected resources require Admin.
        // Lower the min_role for a prefix via set_acl_entry() to open access.
        set_acl_entry("/protected/",  Role::Admin);
        set_acl_entry("/api/private", Role::Admin);
    }

    bool authenticate(const std::string& username,
                      const std::string& password,
                      Role& out_role) const
    {
        // Fetch stored hash under lock, then verify outside it.
        // PBKDF2 is intentionally slow — holding the lock during it would
        // block every other request on this store.
        std::string stored_hash;
        Role role;
        {
            std::shared_lock lock(mutex_);
            auto it = users_.find(username);
            if (it == users_.end()) return false;
            stored_hash = it->second.password_hash;
            role        = it->second.role;
        }

        if (!pbkdf2_verify(password, stored_hash)) return false;
        out_role = role;
        return true;
    }

    // Matches the longest prefix in the ACL list.
    // If no rule matches, defaults to Admin-only.
    bool authorize(Role role, const std::string& path) const {
        std::shared_lock lock(mutex_);
        const AclEntry* best = nullptr;
        for (const auto& entry : acl_) {
            if (path.rfind(entry.path_prefix, 0) == 0) {
                if (!best || entry.path_prefix.size() > best->path_prefix.size())
                    best = &entry;
            }
        }
        Role required = best ? best->min_role : Role::Admin;
        return static_cast<int>(role) >= static_cast<int>(required);
    }

    // Upsert an ACL entry.
    void set_acl_entry(const std::string& prefix, Role min_role) {
        std::unique_lock lock(mutex_);
        for (auto& e : acl_) {
            if (e.path_prefix == prefix) { e.min_role = min_role; return; }
        }
        acl_.push_back({prefix, min_role});
    }

    void remove_acl_entry(const std::string& prefix) {
        std::unique_lock lock(mutex_);
        acl_.erase(std::remove_if(acl_.begin(), acl_.end(),
            [&](const AclEntry& e){ return e.path_prefix == prefix; }), acl_.end());
    }

    std::vector<AclEntry> list_acl_entries() const {
        std::shared_lock lock(mutex_);
        return acl_;
    }

    // ── User management ───────────────────────────────────────────────────────

    // Hash is computed outside the lock — PBKDF2 is intentionally slow.
    void add_user(const std::string& username, const std::string& password, Role role) {
        std::string hash = pbkdf2_hash(password);
        std::unique_lock lock(mutex_);
        users_[username] = {std::move(hash), role};
    }

    bool remove_user(const std::string& username) {
        std::unique_lock lock(mutex_);
        return users_.erase(username) > 0;
    }

    bool set_user_role(const std::string& username, Role role) {
        std::unique_lock lock(mutex_);
        auto it = users_.find(username);
        if (it == users_.end()) return false;
        it->second.role = role;
        return true;
    }

    // Returns username → role pairs (no password hashes).
    std::unordered_map<std::string, Role> list_users() const {
        std::shared_lock lock(mutex_);
        std::unordered_map<std::string, Role> result;
        for (const auto& [k, v] : users_)
            result[k] = v.role;
        return result;
    }

private:
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

    // Returns "iterations:salt_hex:key_hex"
    static std::string pbkdf2_hash(const std::string& password) {
        unsigned char salt[SALT_LEN];
        RAND_bytes(salt, SALT_LEN);

        unsigned char key[KEY_LEN];
        PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                          salt, SALT_LEN, ITERATIONS, EVP_sha256(),
                          KEY_LEN, key);

        return std::to_string(ITERATIONS)
             + ":" + to_hex(salt, SALT_LEN)
             + ":" + to_hex(key,  KEY_LEN);
    }

    // Verifies password against "iterations:salt_hex:key_hex".
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
    std::vector<AclEntry>                        acl_;
    mutable std::shared_mutex                    mutex_;
};
