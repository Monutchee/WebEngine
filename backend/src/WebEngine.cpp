#include "webengine/WebEngine.hpp"

#include <algorithm>
#include <csignal>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <boost/asio/signal_set.hpp>

#include <unistd.h>          // ::unlink

#include "Acl.hpp"
#include "AuthHandler.hpp"
#include "Listener.hpp"
#include "Router.hpp"
#include "TokenStore.hpp"

namespace webengine {

// ── JSON serialisation for the admin endpoints ─────────────────────────────────

static std::string acl_entries_json(const std::vector<AclEntry>& entries) {
    std::string out = R"({"entries":[)";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i) out += ',';
        out += R"({"prefix":")" + entries[i].path_prefix
             + R"(","min_role":")" + role_name(entries[i].min_role) + R"("})";
    }
    return out + "]}";
}

static std::string users_json(const std::vector<UserInfo>& users) {
    std::string out = R"({"users":[)";
    for (size_t i = 0; i < users.size(); ++i) {
        if (i) out += ',';
        out += R"({"username":")" + users[i].username
             + R"(","role":")" + role_name(users[i].role) + R"("})";
    }
    return out + "]}";
}

// ── Engine internals (hidden behind the PIMPL) ─────────────────────────────────

struct WebEngine::Impl {
    explicit Impl(std::shared_ptr<AuthProvider> a)
        : auth(std::move(a)),
          router(tokens),
          auth_handler(*auth, tokens, acl) {}

    std::shared_ptr<AuthProvider> auth;
    TokenStore                    tokens;
    Acl                           acl;
    Router                        router;
    AuthHandler                   auth_handler;

    std::string                   socket_path = "/tmp/backend.sock";
    unsigned                      num_threads = 0;   // 0 → hardware_concurrency

    asio::io_context              ioc;
    std::shared_ptr<Listener>     listener;
    std::unique_ptr<asio::signal_set> signals;   // optional SIGINT/SIGTERM shutdown
};

// ── Construction / lifetime ────────────────────────────────────────────────────

// Validate before constructing Impl: its constructor binds an AuthHandler
// reference to *auth, so a null provider must be rejected up front.
static std::shared_ptr<AuthProvider> require_auth(std::shared_ptr<AuthProvider> auth) {
    if (!auth)
        throw std::invalid_argument("WebEngine: AuthProvider must not be null");
    return auth;
}

WebEngine::WebEngine(std::shared_ptr<AuthProvider> auth)
    : impl_(std::make_unique<Impl>(require_auth(std::move(auth)))) {}

WebEngine::~WebEngine()                            = default;
WebEngine::WebEngine(WebEngine&&) noexcept         = default;
WebEngine& WebEngine::operator=(WebEngine&&) noexcept = default;

// ── API registration ────────────────────────────────────────────────────────────

WebEngine& WebEngine::add_api(http::verb method, std::string path, Handler handler,
                              std::optional<Role> min_role)
{
    impl_->router.add_route(method, std::move(path), std::move(handler), min_role);
    return *this;
}

WebEngine& WebEngine::set_api_role(const std::string& path, std::optional<Role> min_role)
{
    impl_->router.set_route_role(path, min_role);
    return *this;
}

WebEngine& WebEngine::protect_path(const std::string& prefix, Role min_role)
{
    impl_->acl.set(prefix, min_role);
    return *this;
}

WebEngine& WebEngine::unprotect_path(const std::string& prefix)
{
    impl_->acl.remove(prefix);
    return *this;
}

WebEngine& WebEngine::serve_protected_files(std::string url_prefix, std::string fs_root)
{
    AuthHandler*      h      = &impl_->auth_handler;
    const std::string prefix = url_prefix;   // copied into the handler + used as the route key
    // Registered as a public prefix route — the handler itself applies the
    // token + ACL gate (so it can mirror nginx's 302/403 behaviour, not the
    // router's generic 401/403 JSON).
    impl_->router.add_prefix_route(http::verb::get, prefix,
        [h, prefix, root = std::move(fs_root)](const RequestContext& ctx) {
            return h->handle_protected_file(ctx.request, root, prefix);
        }, std::nullopt);
    return *this;
}

// ── Built-in endpoints ────────────────────────────────────────────────────────

WebEngine& WebEngine::enable_auth_endpoints()
{
    AuthHandler* h = &impl_->auth_handler;

    add_api(http::verb::post, "/api/login", [h](const RequestContext& ctx) {
        return h->handle_login(ctx.request);
    });
    add_api(http::verb::post, "/api/logout", [h](const RequestContext& ctx) {
        return h->handle_logout(ctx.request);
    });
    // Called internally by nginx auth_request for /protected/* access.
    add_api(http::verb::get, "/auth-check", [h](const RequestContext& ctx) {
        return h->handle_check(ctx.request);
    });
    return *this;
}

WebEngine& WebEngine::enable_admin_endpoints()
{
    Acl*          acl  = &impl_->acl;
    AuthProvider* auth = impl_->auth.get();

    // ── ACL management: GET/POST/DELETE /api/admin/acl ──────────────────────────
    add_api(http::verb::get, "/api/admin/acl", [acl](const RequestContext&) {
        return json(http::status::ok, acl_entries_json(acl->list()));
    }, Role::Admin);

    add_api(http::verb::post, "/api/admin/acl", [acl](const RequestContext& ctx) {
        std::string prefix   = json_field(ctx.request.body(), "prefix");
        std::string role_str = json_field(ctx.request.body(), "role");
        Role min_role;
        if (prefix.empty() || !role_from_string(role_str, min_role))
            return json(http::status::bad_request,
                R"J({"error":"required: prefix (string), role (admin|user|viewer|guest)"})J");
        acl->set(prefix, min_role);
        return json(http::status::ok, R"({"status":"ok"})");
    }, Role::Admin);

    add_api(http::verb::delete_, "/api/admin/acl", [acl](const RequestContext& ctx) {
        std::string prefix = json_field(ctx.request.body(), "prefix");
        if (prefix.empty())
            return json(http::status::bad_request, R"J({"error":"required: prefix (string)"})J");
        acl->remove(prefix);
        return json(http::status::ok, R"({"status":"ok"})");
    }, Role::Admin);

    // ── User management: GET/POST/DELETE/PATCH /api/admin/users ─────────────────
    add_api(http::verb::get, "/api/admin/users", [auth](const RequestContext&) {
        return json(http::status::ok, users_json(auth->list_users()));
    }, Role::Admin);

    add_api(http::verb::post, "/api/admin/users", [auth](const RequestContext& ctx) {
        if (!auth->supports_management())
            return json(http::status::not_implemented,
                R"({"error":"auth provider is read-only"})");
        std::string username = json_field(ctx.request.body(), "username");
        std::string password = json_field(ctx.request.body(), "password");
        std::string role_str = json_field(ctx.request.body(), "role");
        Role role;
        if (username.empty() || password.empty() || !role_from_string(role_str, role))
            return json(http::status::bad_request,
                R"J({"error":"required: username, password, role (admin|user|viewer|guest)"})J");
        auth->add_user(username, password, role);
        return json(http::status::ok, R"({"status":"ok"})");
    }, Role::Admin);

    add_api(http::verb::delete_, "/api/admin/users", [auth](const RequestContext& ctx) {
        if (!auth->supports_management())
            return json(http::status::not_implemented,
                R"({"error":"auth provider is read-only"})");
        std::string username = json_field(ctx.request.body(), "username");
        if (username.empty())
            return json(http::status::bad_request, R"J({"error":"required: username (string)"})J");
        if (!auth->remove_user(username))
            return json(http::status::not_found, R"({"error":"user not found"})");
        return json(http::status::ok, R"({"status":"ok"})");
    }, Role::Admin);

    add_api(http::verb::patch, "/api/admin/users", [auth](const RequestContext& ctx) {
        if (!auth->supports_management())
            return json(http::status::not_implemented,
                R"({"error":"auth provider is read-only"})");
        std::string username = json_field(ctx.request.body(), "username");
        std::string role_str = json_field(ctx.request.body(), "role");
        Role role;
        if (username.empty() || !role_from_string(role_str, role))
            return json(http::status::bad_request,
                R"J({"error":"required: username, role (admin|user|viewer|guest)"})J");
        if (!auth->set_user_role(username, role))
            return json(http::status::not_found, R"({"error":"user not found"})");
        return json(http::status::ok, R"({"status":"ok"})");
    }, Role::Admin);

    return *this;
}

// ── Server configuration ────────────────────────────────────────────────────────

WebEngine& WebEngine::set_socket_path(std::string path)
{
    impl_->socket_path = std::move(path);
    return *this;
}

WebEngine& WebEngine::set_threads(unsigned n)
{
    impl_->num_threads = n;
    return *this;
}

WebEngine& WebEngine::enable_signal_shutdown()
{
    Impl& impl = *impl_;
    impl.signals = std::make_unique<asio::signal_set>(impl.ioc, SIGINT, SIGTERM);
    // The completion runs as an ordinary handler inside run() — no cross-context
    // call into stop() from signal-handler context.
    impl.signals->async_wait([&impl](const boost::system::error_code& ec, int) {
        if (!ec) impl.ioc.stop();
    });
    return *this;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void WebEngine::run()
{
    Impl& impl = *impl_;

    // Clear any latched "stopped" flag from a prior run()/stop() so the loop
    // actually serves (io_context::run() returns immediately while stopped).
    impl.ioc.restart();

    // Remove any stale socket so bind() never sees EADDRINUSE.
    ::unlink(impl.socket_path.c_str());

    impl.listener = std::make_shared<Listener>(
        impl.ioc, uds::endpoint{impl.socket_path}, impl.router);
    impl.listener->run();

    const unsigned n = impl.num_threads
                     ? impl.num_threads
                     : std::max(1u, std::thread::hardware_concurrency());

    // Each thread keeps serving across any stray exception that escapes the event
    // loop. This must never throw: an exception leaving a std::thread entry, or
    // unwinding past joinable threads, would call std::terminate.
    auto serve = [&impl] {
        for (;;) {
            try { impl.ioc.run(); return; }   // clean exit when work is exhausted/stopped
            catch (...) { /* swallow and resume the loop to keep the engine alive */ }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(n - 1);
    for (unsigned i = 1; i < n; ++i)
        threads.emplace_back(serve);
    serve();
    for (auto& t : threads) t.join();
}

void WebEngine::stop()
{
    impl_->ioc.stop();
}

} // namespace webengine
