#include "AuthHandler.hpp"
#include "util.hpp"

namespace webengine {

Response AuthHandler::handle_login(const Request& req)
{
    const std::string& body = req.body();
    std::string username = json_field(body, "username");
    std::string password = json_field(body, "password");

    std::optional<Role> role;
    if (username.empty() || !(role = auth_.authenticate(username, password)))
        return json(http::status::unauthorized, R"({"error":"invalid credentials"})");

    std::string token = tokens_.issue(username, *role);

    auto res = json(http::status::ok, R"({"status":"ok"})");
    res.set(http::field::set_cookie,
            "session=" + token + "; HttpOnly; SameSite=Strict; Path=/");
    return res;
}

Response AuthHandler::handle_logout(const Request& req)
{
    auto cookie_it = req.find(http::field::cookie);
    if (cookie_it != req.end()) {
        std::string token = extract_cookie(cookie_it->value(), "session");
        if (!token.empty())
            tokens_.revoke(token);
    }

    auto res = json(http::status::ok, R"({"status":"ok"})");
    res.set(http::field::set_cookie,
            "session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
    return res;
}

Response AuthHandler::handle_check(const Request& req)
{
    auto entry = validated_token(req, tokens_);
    if (!entry)
        return status_response(http::status::unauthorized);

    auto uri_it = req.find("X-Original-URI");
    if (uri_it != req.end() && !uri_it->value().empty()) {
        std::string uri(uri_it->value().data(), uri_it->value().size());
        if (!acl_.authorize(entry->role, uri))
            return status_response(http::status::forbidden);
    }

    auto res = status_response(http::status::ok);
    res.set("X-User", entry->username);
    res.set("X-Role", role_name(entry->role));
    return res;
}

} // namespace webengine
