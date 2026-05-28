#pragma once
#include <boost/beast/http.hpp>
#include "AclStore.hpp"
#include "TokenStore.hpp"
#include "util.hpp"

namespace http = boost::beast::http;

class AuthHandler {
public:
    AuthHandler(AclStore& acl, TokenStore& tokens)
        : acl_(acl), tokens_(tokens) {}

    // POST /api/login — validates credentials, issues session cookie on success.
    http::response<http::string_body>
    handle_login(const http::request<http::string_body>& req);

    // POST /api/logout — revokes the session cookie.
    http::response<http::string_body>
    handle_logout(const http::request<http::string_body>& req);

    // GET /auth-check — called internally by nginx auth_request.
    // Returns 200 (allow), 401 (not authenticated), or 403 (forbidden).
    http::response<http::string_body>
    handle_check(const http::request<http::string_body>& req);

    // Extracts and validates the session token from the request (cookie or
    // Authorization: Bearer header). Returns nullopt if missing or expired.
    // Used by API routes that enforce auth themselves instead of via nginx.
    std::optional<TokenEntry>
    get_token_entry(const http::request<http::string_body>& req) const;

private:
    AclStore&    acl_;
    TokenStore&  tokens_;
};
