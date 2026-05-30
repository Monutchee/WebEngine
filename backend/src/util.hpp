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
    std::string search = std::string(name.data(), name.size()) + "=";
    auto pos = cookie_header.find(search);
    if (pos == beast::string_view::npos) return {};
    pos += search.size();
    auto end = cookie_header.find(';', pos);
    beast::string_view val = (end == beast::string_view::npos)
        ? cookie_header.substr(pos)
        : cookie_header.substr(pos, end - pos);
    while (!val.empty() && val.front() == ' ') val.remove_prefix(1);
    while (!val.empty() && val.back()  == ' ') val.remove_suffix(1);
    return std::string(val.data(), val.size());
}

// Pulls the session token from a request: a "session" cookie first, then an
// "Authorization: Bearer <token>" header. Returns "" if neither is present.
inline std::string extract_token(const Request& req) {
    auto cookie_it = req.find(http::field::cookie);
    if (cookie_it != req.end()) {
        std::string token = extract_cookie(cookie_it->value(), "session");
        if (!token.empty()) return token;
    }

    auto auth_it = req.find(http::field::authorization);
    if (auth_it != req.end()) {
        auto hv = auth_it->value(); // beast::string_view
        // beast::string_view has no starts_with() in Boost 1.74; use substr compare.
        if (hv.size() >= 7 && hv.substr(0, 7) == "Bearer ")
            return std::string(hv.data() + 7, hv.size() - 7);
    }
    return {};
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
