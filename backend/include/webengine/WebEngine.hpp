#pragma once
#include <memory>
#include <optional>
#include <string>

#include "AuthProvider.hpp"
#include "Http.hpp"
#include "Role.hpp"

namespace webengine {

// A small embeddable HTTP API engine.
//
// Construct it with an AuthProvider, declare your endpoints and access rules
// with a few chained calls, then run(). The transport (a Unix-domain socket,
// fronted by nginx in this demo), session tokens, routing and role enforcement
// are all handled internally.
//
//     auto auth = std::make_shared<TestAuthProvider>();
//     WebEngine engine(auth);
//     engine.set_socket_path("/tmp/backend.sock")
//           .enable_auth_endpoints()
//           .protect_path("/protected/", Role::Admin)
//           .add_api(http::verb::get, "/api/public", [](const RequestContext&) {
//               return json(http::status::ok, R"({"message":"hello"})");
//           })
//           .add_api(http::verb::get, "/api/private", my_handler, Role::User);
//     engine.run();   // blocks
//
// Configuration calls (add_api, set_api_role, protect_path, …) are safe to make
// both before run() and from within handlers while the engine is serving.
class WebEngine {
public:
    explicit WebEngine(std::shared_ptr<AuthProvider> auth);
    ~WebEngine();

    WebEngine(WebEngine&&) noexcept;
    WebEngine& operator=(WebEngine&&) noexcept;
    WebEngine(const WebEngine&)            = delete;
    WebEngine& operator=(const WebEngine&) = delete;

    // ── API registration ──────────────────────────────────────────────────────

    // Register a handler for a (method, exact-path) pair.
    //   min_role == std::nullopt  → public: no authentication required.
    //   min_role == some Role     → caller must present a valid session token
    //                               for a user whose role satisfies it; the
    //                               authenticated UserInfo is passed in the
    //                               RequestContext. Unauthenticated callers get
    //                               401, under-privileged callers get 403.
    WebEngine& add_api(http::verb method, std::string path, Handler handler,
                       std::optional<Role> min_role = std::nullopt);

    // Change the minimum role required for every method registered at `path`.
    // Pass std::nullopt to make the endpoint public again. Returns *this; the
    // change applies immediately, even while serving. No-op if no such path.
    WebEngine& set_api_role(const std::string& path, std::optional<Role> min_role);

    // ── Static-path protection (for nginx auth_request → /auth-check) ──────────

    // Require `min_role` for any request whose URI starts with `prefix`. Used by
    // enable_auth_endpoints()'s /auth-check to gate static files served by the
    // reverse proxy. Longest matching prefix wins; unmatched paths default to
    // Admin-only. Upserts an existing prefix.
    WebEngine& protect_path(const std::string& prefix, Role min_role);

    // Remove a previously protected prefix.
    WebEngine& unprotect_path(const std::string& prefix);

    // ── Gated static files (for reverse proxies without auth_request) ──────────

    // Serve static files under `url_prefix` from the filesystem directory
    // `fs_root`, gated by the SAME session token + protected-path ACL that
    // /auth-check enforces (so protect_path() and /api/admin/acl apply identically).
    // The served file is `fs_root` joined with the request path minus `url_prefix`.
    //
    // This exists for reverse proxies that lack nginx's auth_request — notably
    // lighttpd, which proxies /protected/ here so the backend gates AND serves
    // those files. With nginx (which gates /protected/ itself and serves it
    // directly) this stays dormant: nginx never proxies the subtree here. Serve
    // PUBLIC static files from the reverse proxy directly — use this only for
    // gated subtrees. Behaviour matches the nginx /protected/ flow: unauthenticated
    // → 302 to "/?reason=unauthenticated", under-privileged → 403, otherwise the
    // file (or 404). Registers a GET prefix route; ".." traversal is rejected.
    WebEngine& serve_protected_files(std::string url_prefix, std::string fs_root);

    // ── Built-in endpoints ─────────────────────────────────────────────────────

    // Registers POST /api/login, POST /api/logout and GET /auth-check, wired to
    // the AuthProvider and the internal session-token store.
    WebEngine& enable_auth_endpoints();

    // Registers the Admin-only management endpoints:
    //   /api/admin/acl   GET/POST/DELETE      — view/edit protected-path prefixes
    //   /api/admin/users GET/POST/DELETE/PATCH — view/edit users (needs a
    //                                            management-capable AuthProvider)
    //
    // NOTE: access control here uses two distinct mechanisms, by design:
    //   • Per-API role         — set via add_api(min_role) / set_api_role(); enforced
    //                            by the router on exact (method,path) routes.
    //   • Protected-path prefix — set via protect_path() and managed by /api/admin/acl;
    //                            enforced by /auth-check for nginx-fronted static files.
    // /api/admin/acl edits the prefix ACL only; it does NOT change an API route's role
    // (use set_api_role for that). The two never share an entry.
    WebEngine& enable_admin_endpoints();

    // ── Server configuration ───────────────────────────────────────────────────

    // Unix-domain socket path to listen on. Default: "/tmp/backend.sock".
    WebEngine& set_socket_path(std::string path);

    // Worker thread count for the I/O event loop. Default: hardware concurrency.
    WebEngine& set_threads(unsigned n);

    // Install graceful-shutdown handling for SIGINT/SIGTERM: when one arrives,
    // run() returns. This uses Boost.Asio's signal_set (the completion runs as a
    // normal in-loop handler), which is the correct, async-signal-safe approach —
    // prefer it over a std::signal handler that calls stop() directly. Call before
    // run().
    WebEngine& enable_signal_shutdown();

    // ── Lifecycle ───────────────────────────────────────────────────────────────

    // Bind the socket and run the event loop. Blocks until stop() is called or a
    // fatal error occurs. Throws std::exception on bind/listen failure.
    void run();

    // Ask the event loop to stop; run() returns shortly after. Safe to call from
    // another thread. NOT async-signal-safe — do not call directly from a POSIX
    // signal handler; use enable_signal_shutdown() for signal-driven shutdown.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace webengine
