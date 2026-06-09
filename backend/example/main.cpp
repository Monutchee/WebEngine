// Example consumer of the webengine library.
//
// This is everything an application author has to write: pick an AuthProvider,
// declare endpoints and their required roles, drive the web server through the
// WebServerController interface, and run(). All the HTTP, session, routing and
// server-control machinery lives in the library.
//
// The reverse proxy is selected at startup by the WEBENGINE_WEBSERVER env var
// ("nginx" — default — or "lighttpd"); the rest of this file is server-agnostic
// because it only ever touches the WebServerController interface.

#include <cctype>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <webengine/TestAuthProvider.hpp>
#include <webengine/WebEngine.hpp>
#include <webengine/WebServerController.hpp>

using namespace webengine;

namespace {

// Small helper for the web-server admin endpoints.
Response server_result(bool ok, const WebServerController& srv) {
    return ok ? json(http::status::ok, R"({"status":"ok"})")
              : json(http::status::internal_server_error,
                     std::string(R"({"error":"web server command failed","detail":")")
                     + srv.last_error() + R"("})");
}

int env_int(const char* name, int fallback) {
    if (const char* v = std::getenv(name)) {
        try { return std::stoi(v); } catch (...) {}
    }
    return fallback;
}

std::string env_str(const char* name, const char* fallback) {
    if (const char* v = std::getenv(name)) return v;
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

        // 2. Web-server controller — starts/stops/reloads the reverse proxy and
        //    changes its listen port at runtime. Which server is driven is chosen
        //    by WEBENGINE_WEBSERVER ("nginx" default, or "lighttpd"); each controller
        //    defaults to this repo's container layout for its server.
        const std::string srv_name = env_str("WEBENGINE_WEBSERVER", "nginx");
        const WebServer kind = web_server_from_string(srv_name).value_or(WebServer::Nginx);
        std::unique_ptr<WebServerController> srv = make_web_server_controller(kind);
        const char* server = to_string(kind);

        // Initial listen ports (default 8080/8443). The deployment overrides these
        // per server so two containers can run side-by-side on different ports.
        // Calling set_listen_ports() while stopped just writes the listen snippet;
        // it takes effect at on().
        const int http_port  = env_int("WEBENGINE_HTTP_PORT",  8080);
        const int https_port = env_int("WEBENGINE_HTTPS_PORT", 8443);
        srv->set_listen_ports(static_cast<std::uint16_t>(http_port),
                              static_cast<std::uint16_t>(https_port));

        // Ports the /port endpoint may switch to. The server will happily bind any
        // port, but in Docker only the published range is reachable from the host,
        // so the deployment passes its routable range here (compose sets the env
        // vars). On the embedded target the server binds the NIC directly — leave
        // them unset for the full 1..65535.
        const int port_min = env_int("WEBENGINE_HTTP_PORT_MIN", 1);
        const int port_max = env_int("WEBENGINE_HTTP_PORT_MAX", 65535);

        // 3. Build and configure the engine with a few friendly calls.
        WebEngine engine(auth);

        engine.set_socket_path("/tmp/backend.sock")
              .enable_auth_endpoints()                       // /api/login, /api/logout, /auth-check
              // Gate /protected/ at Viewer: any logged-in account (viewer/user/admin)
              // can reach the dashboard; only unauthenticated callers are redirected to
              // login. Per-endpoint role is still demonstrated by /api/private (Admin-only)
              // below — e.g. alice/viewer see the dashboard but get 403 from /api/private.
              .protect_path("/protected/", Role::Viewer)
              // Serve /protected/ files through the backend, gated by the ACL above.
              // nginx gates them itself via auth_request and serves them directly, so
              // this is dormant there; lighttpd (no auth_request) proxies /protected/
              // here so the backend gates AND serves them — same session/ACL either way.
              .serve_protected_files("/protected/", "/www/protected");

        // Public API — no authentication required.
        engine.add_api(http::verb::get, "/api/public", [](const RequestContext&) {
            return json(http::status::ok,
                R"({"message":"This is public data. No authentication required."})");
        });

        // Public: which reverse proxy is in front. Lets the static frontend label
        // itself (e.g. "lighttpd + Beast PoC") without needing to log in — unlike
        // the Admin-only /api/admin/server/status. Returns {"server":"nginx"|"lighttpd"}.
        engine.add_api(http::verb::get, "/api/server", [server](const RequestContext&) {
            return json(http::status::ok,
                std::string(R"({"server":")") + server + R"("})");
        });

        // Private API — requires an authenticated Admin. The authenticated user
        // is handed to the handler via ctx.user.
        engine.add_api(http::verb::get, "/api/private", [](const RequestContext& ctx) {
            const UserInfo& u = *ctx.user;   // guaranteed present by the min_role below
            return json(http::status::ok,
                R"({"message":"private data","user":")" + u.username +
                R"(","role":")" + std::string(role_name(u.role)) + R"("})");
        }, Role::Admin);

        // ── Role-tiered test endpoints ────────────────────────────────────────
        // One endpoint per role level so each account can be exercised. The router
        // enforces the minimum role automatically: a caller below it gets 403
        // ("insufficient permissions"); an unauthenticated caller gets 401. On
        // success the body echoes the required level and the caller's own role.
        //   GET /api/public  none    → everyone (no login)
        //   GET /api/viewer  Viewer  → viewer, alice, admin : 200
        //   GET /api/user    User    → alice, admin : 200   | viewer : 403
        //   GET /api/admin   Admin   → admin : 200           | alice, viewer : 403
        // (roles are ordered guest < viewer < user < admin, so a higher role
        //  satisfies any lower requirement.)
        auto tiered = [](const char* level) {
            return [level](const RequestContext& ctx) {
                const UserInfo& u = *ctx.user;   // present: these routes require a role
                return json(http::status::ok,
                    std::string(R"({"message":")") + level + R"( access granted","required":")"
                    + level + R"(","user":")" + u.username
                    + R"(","role":")" + role_name(u.role) + R"("})");
            };
        };
        engine.add_api(http::verb::get, "/api/viewer", tiered("viewer"), Role::Viewer);
        engine.add_api(http::verb::get, "/api/user",   tiered("user"),   Role::User);
        engine.add_api(http::verb::get, "/api/admin",  tiered("admin"),  Role::Admin);

        // Admin-only management endpoints for users and protected paths.
        engine.enable_admin_endpoints();

        // ── Web-server control endpoints (Admin only) ─────────────────────────
        // Drive whichever reverse proxy was selected at startup (nginx/lighttpd):
        //   POST /api/admin/server/on    — start the server
        //   POST /api/admin/server/off   — graceful stop (requires {"force":true})
        //   POST /api/admin/server/reset — hard restart
        //   POST /api/admin/server/port  — {"http": <port>} live HTTP port change
        //   GET  /api/admin/server/status
        WebServerController* s = srv.get();   // captured by handlers; outlives them

        engine.add_api(http::verb::post, "/api/admin/server/on", [s](const RequestContext&) {
            return server_result(s->on(), *s);
        }, Role::Admin);

        // The reverse proxy is this API's only ingress, so an unguarded stop would
        // lock the caller out (no way to reach /on again). Require explicit confirm.
        engine.add_api(http::verb::post, "/api/admin/server/off", [s](const RequestContext& ctx) {
            if (json_token(ctx.request.body(), "force") != "true")
                return json(http::status::conflict,
                    R"({"error":"stopping the web server makes this API unreachable since it is the only ingress; resend with force=true to confirm, or use reset"})");
            return server_result(s->off(), *s);
        }, Role::Admin);

        engine.add_api(http::verb::post, "/api/admin/server/reset", [s](const RequestContext&) {
            return server_result(s->reset(), *s);
        }, Role::Admin);

        engine.add_api(http::verb::post, "/api/admin/server/port",
                       [s, port_min, port_max](const RequestContext& ctx) {
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
            return server_result(s->set_listen_port(static_cast<std::uint16_t>(p)), *s);
        }, Role::Admin);

        engine.add_api(http::verb::get, "/api/admin/server/status", [s, server](const RequestContext&) {
            return json(http::status::ok,
                std::string(R"({"server":")") + server
                + R"(","running":)" + (s->is_running() ? "true" : "false")
                + R"(,"http_port":)"  + std::to_string(s->http_port())
                + R"(,"https_port":)" + std::to_string(s->https_port()) + "}");
        }, Role::Admin);

        // 4. Graceful shutdown on SIGINT/SIGTERM, the async-signal-safe way
        //    (Boost.Asio signal_set) — makes run() return so we can stop the server.
        engine.enable_signal_shutdown();

        // 5. Start the web server, then run the API loop. The backend is the
        //    foreground process; the server runs as a sibling the controller manages.
        if (!srv->on())
            std::cerr << "warning: " << server << " failed to start: " << srv->last_error() << '\n';

        std::cout << "backend listening on /tmp/backend.sock; " << server
                  << " on port " << srv->http_port() << '\n';
        engine.run();   // blocks until SIGINT/SIGTERM

        // 6. Graceful shutdown.
        std::cout << "shutting down " << server << '\n';
        srv->off();
    }
    catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
