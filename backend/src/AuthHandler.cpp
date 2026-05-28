#include "AuthHandler.hpp"
#include "AclStore.hpp"

// All header field values from Boost 1.74 Beast are boost::beast::string_view,
// not std::string_view — no implicit conversion exists between the two.

http::response<http::string_body>
AuthHandler::handle_login(const http::request<http::string_body>& req)
{
    const std::string& body = req.body();
    std::string username = extract_json_field(body, "username");
    std::string password = extract_json_field(body, "password");

    Role role;
    if (username.empty() || !acl_.authenticate(username, password, role))
        return make_json_response(http::status::unauthorized,
                                  R"({"error":"invalid credentials"})");

    std::string token = tokens_.issue(username, role);

    auto res = make_json_response(http::status::ok, R"({"status":"ok"})");
    res.set(http::field::set_cookie,
            "session=" + token + "; HttpOnly; SameSite=Strict; Path=/");
    return res;
}

http::response<http::string_body>
AuthHandler::handle_logout(const http::request<http::string_body>& req)
{
    auto cookie_it = req.find(http::field::cookie);
    if (cookie_it != req.end()) {
        // cookie_it->value() is beast::string_view — pass directly, extract_cookie accepts it
        std::string token = extract_cookie(cookie_it->value(), "session");
        if (!token.empty())
            tokens_.revoke(token);
    }

    auto res = make_json_response(http::status::ok, R"({"status":"ok"})");
    res.set(http::field::set_cookie,
            "session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
    return res;
}

http::response<http::string_body>
AuthHandler::handle_check(const http::request<http::string_body>& req)
{
    std::string token;

    auto cookie_it = req.find(http::field::cookie);
    if (cookie_it != req.end())
        token = extract_cookie(cookie_it->value(), "session");

    if (token.empty()) {
        auto auth_it = req.find(http::field::authorization);
        if (auth_it != req.end()) {
            auto hv = auth_it->value();  // beast::string_view
            // beast::string_view has no starts_with() in Boost 1.74; use substr compare
            if (hv.size() >= 7 && hv.substr(0, 7) == "Bearer ")
                token = std::string(hv.data() + 7, hv.size() - 7);
        }
    }

    if (token.empty())
        return make_status_response(http::status::unauthorized);

    auto entry = tokens_.validate(token);
    if (!entry)
        return make_status_response(http::status::unauthorized);

    auto uri_it = req.find("X-Original-URI");
    if (uri_it != req.end() && !uri_it->value().empty()) {
        // Convert beast::string_view to std::string for AclStore
        std::string uri(uri_it->value().data(), uri_it->value().size());
        if (!acl_.authorize(entry->role, uri))
            return make_status_response(http::status::forbidden);
    }

    auto res = make_status_response(http::status::ok);
    res.set("X-User", entry->username);
    res.set("X-Role", role_name(entry->role));
    return res;
}

std::optional<TokenEntry>
AuthHandler::get_token_entry(const http::request<http::string_body>& req) const
{
    std::string token;

    auto cookie_it = req.find(http::field::cookie);
    if (cookie_it != req.end())
        token = extract_cookie(cookie_it->value(), "session");

    if (token.empty()) {
        auto auth_it = req.find(http::field::authorization);
        if (auth_it != req.end()) {
            auto hv = auth_it->value();
            if (hv.size() >= 7 && hv.substr(0, 7) == "Bearer ")
                token = std::string(hv.data() + 7, hv.size() - 7);
        }
    }

    if (token.empty()) return std::nullopt;
    return tokens_.validate(token);
}
