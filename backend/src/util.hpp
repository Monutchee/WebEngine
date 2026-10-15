#pragma once
#include <optional>
#include <string>

#include "webengine/Http.hpp"
#include "TokenStore.hpp"

namespace webengine {

namespace beast = boost::beast;

// Internal helpers shared by the router and the built-in auth handlers.
//
// Boost 1.74 header field values are boost::beast::string_view (not
// std::string_view); there is no implicit conversion between the two, so these
// helpers work in terms of beast::string_view.

// Extracts the value of cookie `name` from a Cookie header. Returns "" if absent.
inline std::string extract_cookie(beast::string_view cookie_header,
                                   beast::string_view name)
{
    while (!cookie_header.empty()) {
        auto end = cookie_header.find(';');
        auto item = cookie_header.substr(0, end);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        auto eq = item.find('=');
        if (eq != beast::string_view::npos && item.substr(0, eq) == name)
            return std::string(item.data() + eq + 1, item.size() - eq - 1);
        if (end == beast::string_view::npos) break;
        cookie_header.remove_prefix(end + 1);
    }
    return {};
}

// An explicit Authorization header takes precedence over the session cookie.
// Invalid Authorization never falls back to cookie credentials.
inline std::string extract_token(const Request& req) {
    auto auth = req.find(http::field::authorization);
    if (auth != req.end()) {
        auto value = auth->value();
        if (value.size() > 7 && boost::beast::iequals(value.substr(0, 7), "Bearer "))
            return std::string(value.data() + 7, value.size() - 7);
        return {}; // Explicit Authorization never falls back to a cookie.
    }
    auto cookie = req.find(http::field::cookie);
    return cookie == req.end() ? std::string{} : extract_cookie(cookie->value(), "session");
}

// Resolves and validates the session token on a request to its store entry.
// Returns std::nullopt if there is no token or it is invalid/expired.
inline std::optional<TokenEntry>
validated_token(const Request& req, const TokenStore& tokens) {
    std::string token = extract_token(req);
    if (token.empty()) return std::nullopt;
    return tokens.validate(token);
}

} // namespace webengine
