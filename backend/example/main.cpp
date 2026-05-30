// Example consumer of the webengine library.
//
// This is everything an application author has to write: pick an AuthProvider,
// declare endpoints and their required roles, drive nginx with NginxController,
// and run(). All the HTTP, session, routing and nginx-control machinery lives in
// the library.

#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>

#include <webengine/NginxController.hpp>
#include <webengine/TestAuthProvider.hpp>
#include <webengine/WebEngine.hpp>

using namespace webengine;

namespace {

// Small helper for the nginx admin endpoints.
Response nginx_result(bool ok, const NginxController& nx) {
    return ok ? json(http::status::ok, R"({"status":"ok"})")
              : json(http::status::internal_server_error,
                     std::string(R"({"error":"nginx command failed","detail":")")
                     + nx.last_error() + R"("})");
}

int env_int(const char* name, int fallback) {
    if (const char* v = std::getenv(name)) {
        try { return std::stoi(v); } catch (...) {}
    }
    return fallback;
}

// Extracts a top-level JSON value as a raw token, tolerating both quoted and
// unquoted forms — unlike json_field() (string-only), this also reads numbers
// and booleans, so {"http":8081} and {"force":true} parse, not just the quoted
// variants. Returns "" if absent.
std::string json_token(const std::string& body, const std::string& key) {
    const std::string q = "\"" + key + "\"";
    auto pos = body.find(q);
    if (pos == std::string::npos) return {};
    pos = body.find(':', pos + q.size());
    if (pos == std::string::npos) return {};
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    size_t end = pos;
    while (end < body.size() && body[end] != ',' && body[end] != '}') ++end;
    std::string v = body.substr(pos, end - pos);
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    return v;
}

} // namespace

int main()
{
    try {
        // 1. Auth data via a pluggable provider. TestAuthProvider injects a few
        //    in-memory test accounts (admin/admin123, alice/user123, …). Swap in
        //    your own AuthProvider subclass to load real users.
        auto auth = std::make_shared<TestAuthProvider>();

        // 2. nginx controller — starts/stops/reloads the reverse proxy and changes
        //    its listen port at runtime. Defaults match this repo's container layout
        //    (config /etc/nginx/nginx.conf, snippet /etc/nginx/conf.d/listen.conf,
        //    high ports so the unprivileged service user can bind them).
        NginxController nginx;

        // Ports the /port endpoint may switch to. nginx will happily bind any port,
        // but in Docker only the published range is reachable from the host, so the
        // deployment passes its routable range here (compose sets the env vars). On
        // the embedded target nginx binds the NIC directly — leave them unset for
        // the full 1..65535.
        const int port_min = env_int("WEBENGINE_HTTP_PORT_MIN", 1);
        const int port_max = env_int("WEBENGINE_HTTP_PORT_MAX", 65535);

        // 3. Build and configure the engine with a few friendly calls.
        WebEngine engine(auth);

        engine.set_socket_path("/tmp/backend.sock")
              .enable_auth_endpoints()                       // /api/login, /api/logout, /auth-check
              .protect_path("/protected/", Role::Admin);     // gate static files (nginx auth_request)

        // Public API — no authentication required.
        engine.add_api(http::verb::get, "/api/public", [](const RequestContext&) {
            return json(http::status::ok,
                R"({"message":"This is public data. No authentication required."})");
        });

        // Private API — requires an authenticated Admin. The authenticated user
        // is handed to the handler via ctx.user.
        engine.add_api(http::verb::get, "/api/private", [](const RequestContext& ctx) {
            const UserInfo& u = *ctx.user;   // guaranteed present by the min_role below
            return json(http::status::ok,
                R"({"message":"private data","user":")" + u.username +
                R"(","role":")" + std::string(role_name(u.role)) + R"("})");
        }, Role::Admin);

        // Admin-only management endpoints for users and protected paths.
        engine.enable_admin_endpoints();

        // ── nginx control endpoints (Admin only) ──────────────────────────────
        //   POST /api/admin/nginx/on    — start nginx
        //   POST /api/admin/nginx/off   — graceful stop (requires {"force":true})
        //   POST /api/admin/nginx/reset — hard restart
        //   POST /api/admin/nginx/port  — {"http": <port>} live HTTP port change
        //   GET  /api/admin/nginx/status
        engine.add_api(http::verb::post, "/api/admin/nginx/on", [&nginx](const RequestContext&) {
            return nginx_result(nginx.on(), nginx);
        }, Role::Admin);

        // nginx is this API's only ingress, so an unguarded stop would lock the
        // caller out (no way to reach /on again). Require an explicit confirmation.
        engine.add_api(http::verb::post, "/api/admin/nginx/off", [&nginx](const RequestContext& ctx) {
            if (json_token(ctx.request.body(), "force") != "true")
                return json(http::status::conflict,
                    R"({"error":"stopping nginx makes this API unreachable since nginx is the only ingress; resend with force=true to confirm, or use reset"})");
            return nginx_result(nginx.off(), nginx);
        }, Role::Admin);

        engine.add_api(http::verb::post, "/api/admin/nginx/reset", [&nginx](const RequestContext&) {
            return nginx_result(nginx.reset(), nginx);
        }, Role::Admin);

        engine.add_api(http::verb::post, "/api/admin/nginx/port",
                       [&nginx, port_min, port_max](const RequestContext& ctx) {
            std::string http_s = json_token(ctx.request.body(), "http");
            int p = 0;
            try { p = std::stoi(http_s); }
            catch (...) {
                return json(http::status::bad_request,
                            R"J({"error":"required: http (an integer port number)"})J");
            }
            if (p < port_min || p > port_max)
                return json(http::status::bad_request,
                    std::string(R"({"error":"http port must be in the routable range )")
                    + std::to_string(port_min) + "-" + std::to_string(port_max) + R"("})");
            return nginx_result(nginx.set_listen_port(static_cast<std::uint16_t>(p)), nginx);
        }, Role::Admin);

        engine.add_api(http::verb::get, "/api/admin/nginx/status", [&nginx](const RequestContext&) {
            return json(http::status::ok,
                std::string(R"({"running":)") + (nginx.is_running() ? "true" : "false")
                + R"(,"http_port":)"  + std::to_string(nginx.http_port())
                + R"(,"https_port":)" + std::to_string(nginx.https_port()) + "}");
        }, Role::Admin);

        // 4. Graceful shutdown on SIGINT/SIGTERM, the async-signal-safe way
        //    (Boost.Asio signal_set) — makes run() return so we can stop nginx.
        engine.enable_signal_shutdown();

        // 5. Start nginx, then run the API loop. The backend is the foreground
        //    process; nginx runs as a sibling the controller manages.
        if (!nginx.on())
            std::cerr << "warning: nginx failed to start: " << nginx.last_error() << '\n';

        std::cout << "backend listening on /tmp/backend.sock; nginx on port "
                  << nginx.http_port() << '\n';
        engine.run();   // blocks until SIGINT/SIGTERM

        // 6. Graceful shutdown.
        std::cout << "shutting down nginx\n";
        nginx.off();
    }
    catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
