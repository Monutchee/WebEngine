#pragma once
#include <string>

namespace webengine {

// Access levels, ordered from least to most privileged. The numeric values are
// meaningful: a role satisfies a requirement when its value is >= the required
// role's value (see Acl / Router enforcement).
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

// True when `have` is privileged enough to satisfy a `need` requirement.
inline bool role_satisfies(Role have, Role need) {
    return static_cast<int>(have) >= static_cast<int>(need);
}

} // namespace webengine
