#pragma once
#include "webengine/AuthProvider.hpp"
#include "webengine/Http.hpp"
#include "Acl.hpp"
#include "TokenStore.hpp"

namespace webengine {

// Implements the built-in authentication endpoints. Owned by the engine and
// wired to the pluggable AuthProvider, the session-token store and the
// static-path ACL.
class AuthHandler {
public:
    AuthHandler(AuthProvider& auth, TokenStore& tokens, Acl& acl)
        : auth_(auth), tokens_(tokens), acl_(acl) {}

    // POST /api/login — validates credentials, issues a session cookie on success.
    Response handle_login(const Request& req);

    // POST /api/logout — revokes the session cookie.
    Response handle_logout(const Request& req);

    // GET /auth-check — called internally by nginx auth_request.
    // Returns 200 (allow), 401 (not authenticated) or 403 (forbidden), and on
    // success echoes the user/role back in X-User / X-Role headers.
    Response handle_check(const Request& req);

private:
    AuthProvider& auth_;
    TokenStore&   tokens_;
    Acl&          acl_;
};

} // namespace webengine
