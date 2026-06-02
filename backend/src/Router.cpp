#include "Router.hpp"

static std::string acl_entries_json(const std::vector<AclEntry>& entries) {
    std::string out = R"({"entries":[)";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i) out += ',';
        out += R"({"prefix":")" + entries[i].path_prefix
             + R"(","min_role":")" + role_name(entries[i].min_role) + R"("})";
    }
    return out + "]}";
}

static std::string users_json(const std::unordered_map<std::string, Role>& users) {
    std::string out = R"({"users":[)";
    bool first = true;
    for (const auto& [name, role] : users) {
        if (!first) out += ',';
        first = false;
        out += R"({"username":")" + name
             + R"(","role":")" + role_name(role) + R"("})";
    }
    return out + "]}";
}

http::response<http::string_body>
Router::dispatch(const http::request<http::string_body>& req)
{
    auto target = req.target();
    auto method = req.method();

    // ── Auth endpoints ────────────────────────────────────────────────────────
    if (target == "/api/login" && method == http::verb::post)
        return auth_.handle_login(req);

    if (target == "/api/logout" && method == http::verb::post)
        return auth_.handle_logout(req);

    // Called internally by nginx auth_request for /protected/* access.
    if (target == "/auth-check" && method == http::verb::get)
        return auth_.handle_check(req);

    // ── Public API — no authentication required ───────────────────────────────
    if (target == "/api/public" && method == http::verb::get)
        return make_json_response(http::status::ok,
            R"({"message":"This is public data. No authentication required."})");

    // ── Private API — role enforced via ACL ───────────────────────────────────
    if (target == "/api/private" && method == http::verb::get) {
        auto entry = auth_.get_token_entry(req);
        if (!entry)
            return make_json_response(http::status::unauthorized,
                R"({"error":"authentication required"})");
        std::string path(target.data(), target.size());
        if (!acl_.authorize(entry->role, path))
            return make_json_response(http::status::forbidden,
                R"({"error":"insufficient permissions"})");
        return make_json_response(http::status::ok,
            R"({"message":"private data","user":")" + entry->username
            + R"(","role":")" + role_name(entry->role) + R"("})");
    }

    // ── Admin API — require Admin role ────────────────────────────────────────
    // Shared guard: returns an error response if the request is not from an Admin.
    auto require_admin = [&]() -> std::optional<http::response<http::string_body>> {
        auto e = auth_.get_token_entry(req);
        if (!e)
            return make_json_response(http::status::unauthorized,
                R"({"error":"authentication required"})");
        if (e->role != Role::Admin)
            return make_json_response(http::status::forbidden,
                R"({"error":"admin role required"})");
        return std::nullopt;
    };

    // ACL management: GET/POST/DELETE /api/admin/acl
    if (target == "/api/admin/acl") {
        if (auto err = require_admin()) return *err;

        if (method == http::verb::get)
            return make_json_response(http::status::ok,
                acl_entries_json(acl_.list_acl_entries()));

        if (method == http::verb::post) {
            std::string prefix   = extract_json_field(req.body(), "prefix");
            std::string role_str = extract_json_field(req.body(), "role");
            Role min_role;
            if (prefix.empty() || !role_from_string(role_str, min_role))
                return make_json_response(http::status::bad_request,
                    "{\"error\":\"required: prefix (string), role (admin|user|viewer|guest)\"}");
            acl_.set_acl_entry(prefix, min_role);
            return make_json_response(http::status::ok, R"({"status":"ok"})");
        }

        if (method == http::verb::delete_) {
            std::string prefix = extract_json_field(req.body(), "prefix");
            if (prefix.empty())
                return make_json_response(http::status::bad_request,
                    "{\"error\":\"required: prefix (string)\"}");
            acl_.remove_acl_entry(prefix);
            return make_json_response(http::status::ok, R"({"status":"ok"})");
        }
    }

    // User management: GET/POST/DELETE/PATCH /api/admin/users
    if (target == "/api/admin/users") {
        if (auto err = require_admin()) return *err;

        if (method == http::verb::get)
            return make_json_response(http::status::ok,
                users_json(acl_.list_users()));

        if (method == http::verb::post) {
            std::string username = extract_json_field(req.body(), "username");
            std::string password = extract_json_field(req.body(), "password");
            std::string role_str = extract_json_field(req.body(), "role");
            Role role;
            if (username.empty() || password.empty() || !role_from_string(role_str, role))
                return make_json_response(http::status::bad_request,
                    "{\"error\":\"required: username, password, role (admin|user|viewer|guest)\"}");
            acl_.add_user(username, password, role);
            return make_json_response(http::status::ok, R"({"status":"ok"})");
        }

        if (method == http::verb::delete_) {
            std::string username = extract_json_field(req.body(), "username");
            if (username.empty())
                return make_json_response(http::status::bad_request,
                    "{\"error\":\"required: username (string)\"}");
            if (!acl_.remove_user(username))
                return make_json_response(http::status::not_found,
                    R"({"error":"user not found"})");
            return make_json_response(http::status::ok, R"({"status":"ok"})");
        }

        if (method == http::verb::patch) {
            std::string username = extract_json_field(req.body(), "username");
            std::string role_str = extract_json_field(req.body(), "role");
            Role role;
            if (username.empty() || !role_from_string(role_str, role))
                return make_json_response(http::status::bad_request,
                    "{\"error\":\"required: username, role (admin|user|viewer|guest)\"}");
            if (!acl_.set_user_role(username, role))
                return make_json_response(http::status::not_found,
                    R"({"error":"user not found"})");
            return make_json_response(http::status::ok, R"({"status":"ok"})");
        }
    }

    return make_response(http::status::not_found, "Not Found");
}
