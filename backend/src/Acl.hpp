#pragma once
#include <algorithm>
#include <shared_mutex>
#include <string>
#include <vector>

#include "webengine/Role.hpp"

namespace webengine {

struct AclEntry {
    std::string path_prefix;
    Role        min_role;
};

// Thread-safe path-prefix → minimum-role access list.
//
// Used to gate resources by URI prefix — primarily the static files the reverse
// proxy guards via its auth_request sub-request to /auth-check. Longest matching
// prefix wins; if nothing matches, access defaults to Admin-only (deny-by-default).
class Acl {
public:
    // True if `role` is privileged enough for `path` under the matching rule.
    bool authorize(Role role, const std::string& path) const {
        std::shared_lock lock(mutex_);
        const AclEntry* best = nullptr;
        for (const auto& entry : entries_) {
            if (path.rfind(entry.path_prefix, 0) == 0) {
                if (!best || entry.path_prefix.size() > best->path_prefix.size())
                    best = &entry;
            }
        }
        Role required = best ? best->min_role : Role::Admin;
        return role_satisfies(role, required);
    }

    // Upsert a prefix rule.
    void set(const std::string& prefix, Role min_role) {
        std::unique_lock lock(mutex_);
        for (auto& e : entries_) {
            if (e.path_prefix == prefix) { e.min_role = min_role; return; }
        }
        entries_.push_back({prefix, min_role});
    }

    void remove(const std::string& prefix) {
        std::unique_lock lock(mutex_);
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
            [&](const AclEntry& e){ return e.path_prefix == prefix; }), entries_.end());
    }

    std::vector<AclEntry> list() const {
        std::shared_lock lock(mutex_);
        return entries_;
    }

private:
    std::vector<AclEntry>     entries_;
    mutable std::shared_mutex mutex_;
};

} // namespace webengine
