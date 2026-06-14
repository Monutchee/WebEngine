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

    // GET <gated subtree> — serve a static file with the SAME session-token + ACL
    // gate that handle_check applies, for a reverse proxy that lacks auth_request
    // (lighttpd). The file is read from `fs_root` joined with the request path
    // minus `url_prefix`. Behaviour mirrors the nginx /protected/ flow:
    //   no/invalid token   -> 302 redirect to "/?reason=unauthenticated"
    //   ACL denies the role-> 403 "Access denied"
    //   authorized + found -> 200 with the file body and a content-type by extension
    //   authorized, missing-> 404
    // Path traversal ("..") is rejected.
    Response handle_protected_file(const Request& req,
                                   const std::string& fs_root,
                                   const std::string& url_prefix);

private:
    AuthProvider& auth_;
    TokenStore&   tokens_;
    Acl&          acl_;
};

} // namespace webengine
