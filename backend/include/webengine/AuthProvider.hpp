#pragma once
#include <optional>
#include <string>
#include <vector>

#include "Role.hpp"

namespace webengine {

// A username paired with its role. Returned by listings; never carries secrets.
struct UserInfo {
    std::string username;
    Role        role;
};

// Pluggable source of authentication data.
//
// Implement this to back the engine with whatever user store you like — an
// in-memory map, a config file, an LDAP/SQL directory, etc. The engine only
// ever calls into the provider through this interface, so swapping the backend
// never touches engine or handler code.
//
// `authenticate()` is the only method you must implement. The user-management
// methods are optional: override them (and `supports_management()`) if you want
// the built-in admin user endpoints to be able to mutate your store. The base
// implementations report "unsupported" so a read-only provider stays read-only.
//
// All methods may be called concurrently from multiple worker threads, so
// implementations must be thread-safe.
class AuthProvider {
public:
    virtual ~AuthProvider() = default;

    // Verify credentials. Returns the user's role on success, std::nullopt on
    // any failure (unknown user, wrong password, etc.).
    virtual std::optional<Role>
    authenticate(const std::string& username, const std::string& password) = 0;

    // Whether the optional user-management methods below are functional.
    // Gate runtime mutation on this; the admin endpoints do.
    virtual bool supports_management() const { return false; }

    // Create or update a user. Returns true on success.
    virtual bool add_user(const std::string& /*username*/,
                          const std::string& /*password*/,
                          Role /*role*/) { return false; }

    // Remove a user. Returns true if a user was removed.
    virtual bool remove_user(const std::string& /*username*/) { return false; }

    // Change an existing user's role. Returns true if the user existed.
    virtual bool set_user_role(const std::string& /*username*/, Role /*role*/) { return false; }

    // Snapshot of all known users (no secrets). Empty if listing is unsupported.
    virtual std::vector<UserInfo> list_users() const { return {}; }
};

} // namespace webengine
