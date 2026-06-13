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

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include <glaze/glaze.hpp>

#include <webengine/TestAuthProvider.hpp>
#include <webengine/WebEngine.hpp>
#include <webengine/WebServerController.hpp>

using namespace webengine;

namespace {

// Request bodies for the web-server control endpoints.
struct ForceReq { std::optional<bool> force; };   // {"force": true}
struct PortReq  { std::optional<int>  http;  };   // {"http": <port>}

// Small helper for the web-server admin endpoints.
Response server_result(bool ok, const WebServerController& srv) {
    if (ok) return json(http::status::ok, R"({"status":"ok"})");
    const std::string detail = srv.last_error();
    return json(http::status::internal_server_error,
                glz::write_json(glz::obj{"error", "web server command failed", "detail", detail})
                    .value_or(std::string{R"({"error":"web server command failed"})"}));
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

        // Filesystem paths default to this repo's container layout (/etc/nginx,
        // /tmp). The embedded target overrides them via WEBENGINE_* in its systemd
        // unit to point config/state at /opt/monutchee/msys; an unset/empty value
        // keeps the default, so Docker is unaffected.
        WebServerPaths paths;
        paths.config      = env_str("WEBENGINE_CONFIG",      "");
        paths.listen_file = env_str("WEBENGINE_LISTEN_FILE", "");
        paths.pidfile     = env_str("WEBENGINE_PIDFILE",     "");
        paths.temp_root   = env_str("WEBENGINE_TEMP_ROOT",   "");
        std::unique_ptr<WebServerController> srv = make_web_server_controller(kind, paths);
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
            return json(http::status::ok, glz::write_json(
                glz::obj{"message", "This is public data. No authentication required."}).value_or(std::string{}));
        });

        // Public: which reverse proxy is in front. Lets the static frontend label
        // itself (e.g. "lighttpd + Beast PoC") without needing to log in — unlike
        // the Admin-only /api/admin/server/status. Returns {"server":"nginx"|"lighttpd"}.
        engine.add_api(http::verb::get, "/api/server", [server](const RequestContext&) {
            return json(http::status::ok, glz::write_json(glz::obj{"server", server}).value_or(std::string{}));
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
            return json(http::status::ok, glz::write_json(glz::obj{"message", "private data",
                "user", u.username, "role", role_name(u.role)}).value_or(std::string{}));
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
                const std::string message = std::string(level) + " access granted";
                return json(http::status::ok, glz::write_json(glz::obj{"message", message,
                    "required", level, "user", u.username, "role", role_name(u.role)}).value_or(std::string{}));
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
            ForceReq req{};
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(req, ctx.request.body())
                    || req.force != true)
                return json(http::status::conflict, glz::write_json(glz::obj{"error",
                    "stopping the web server makes this API unreachable since it is the only ingress; resend with force=true to confirm, or use reset"}).value_or(std::string{}));
            return server_result(s->off(), *s);
        }, Role::Admin);

        engine.add_api(http::verb::post, "/api/admin/server/reset", [s](const RequestContext&) {
            return server_result(s->reset(), *s);
        }, Role::Admin);

        engine.add_api(http::verb::post, "/api/admin/server/port",
                       [s, port_min, port_max](const RequestContext& ctx) {
            PortReq req{};
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(req, ctx.request.body())
                    || !req.http)
                return json(http::status::bad_request, glz::write_json(
                    glz::obj{"error", "required: http (an integer port number)"}).value_or(std::string{}));
            const int p = *req.http;
            if (p < port_min || p > port_max)
                return json(http::status::bad_request, glz::write_json(glz::obj{"error",
                    "http port must be in the routable range "
                    + std::to_string(port_min) + "-" + std::to_string(port_max)}).value_or(std::string{}));
            return server_result(s->set_listen_port(static_cast<std::uint16_t>(p)), *s);
        }, Role::Admin);

        engine.add_api(http::verb::get, "/api/admin/server/status", [s, server](const RequestContext&) {
            return json(http::status::ok, glz::write_json(glz::obj{"server", server,
                "running", s->is_running(),
                "http_port", s->http_port(),
                "https_port", s->https_port()}).value_or(std::string{}));
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
