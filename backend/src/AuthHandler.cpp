#include "AuthHandler.hpp"
#include "util.hpp"

#include <fstream>
#include <sstream>

namespace webengine {

namespace {

// Minimal extension → MIME map for the static files this PoC serves. Unknown
// extensions fall back to a safe binary default.
std::string content_type_for(const std::string& path) {
    auto dot = path.rfind('.');
    std::string ext = (dot == std::string::npos) ? "" : path.substr(dot + 1);
    if (ext == "html" || ext == "htm") return "text/html";
    if (ext == "txt")                  return "text/plain";
    if (ext == "css")                  return "text/css";
    if (ext == "js")                   return "application/javascript";
    if (ext == "json")                 return "application/json";
    if (ext == "svg")                  return "image/svg+xml";
    if (ext == "png")                  return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "ico")                  return "image/x-icon";
    return "application/octet-stream";
}

} // namespace

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

Response AuthHandler::handle_protected_file(const Request& req,
                                            const std::string& fs_root,
                                            const std::string& url_prefix)
{
    // Path only; drop any query string.
    auto target = req.target();
    std::string path(target.data(), target.size());
    if (auto q = path.find('?'); q != std::string::npos)
        path.resize(q);

    // Authn: no/invalid token → redirect to the login page (matches nginx's
    // `error_page 401 = @login_redirect` for /protected/).
    auto entry = validated_token(req, tokens_);
    if (!entry) {
        Response res{http::status::found, req.version()};
        res.set(http::field::location, "/?reason=unauthenticated");
        res.prepare_payload();
        return res;
    }

    // Authz: the same longest-prefix ACL /auth-check consults.
    if (!acl_.authorize(entry->role, path))
        return text(http::status::forbidden, "Access denied");

    // Map URL → filesystem path. Reject any parent-dir escape before touching disk.
    std::string rel = path.size() > url_prefix.size() ? path.substr(url_prefix.size())
                                                      : std::string();
    if (rel.empty() || rel.back() == '/') rel += "index.html";
    if (rel.find("..") != std::string::npos)
        return text(http::status::forbidden, "Access denied");
    while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());

    std::string file = fs_root;
    if (!file.empty() && file.back() == '/') file.pop_back();
    file += "/" + rel;

    std::ifstream in(file, std::ios::binary);
    if (!in)
        return text(http::status::not_found, "Not Found");
    std::ostringstream ss;
    ss << in.rdbuf();

    return make_response(http::status::ok, ss.str(), content_type_for(rel));
}

} // namespace webengine
