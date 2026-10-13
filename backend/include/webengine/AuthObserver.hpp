#pragma once
#include <functional>
#include <string>

namespace webengine {
// Completed authentication/administration outcomes. No credentials or tokens
// are exposed. Callbacks run on HTTP threads and must be thread-safe.
enum class AuthOperation { login, logout, user_saved, user_removed, role_changed,
                           acl_saved, acl_removed };
struct AuthEvent {
    AuthOperation operation;
    bool succeeded;
    std::string actor;
    std::string subject;
};
using AuthObserver = std::function<void(const AuthEvent&)>;
} // namespace webengine
