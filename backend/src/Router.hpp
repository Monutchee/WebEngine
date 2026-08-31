#pragma once
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <functional>
#include <utility>
#include <vector>

#include "webengine/Http.hpp"
#include "webengine/Role.hpp"
#include "TokenStore.hpp"

namespace webengine {

// Generic request router.
//
// Holds a table of (HTTP method, exact path) → {handler, minimum role}. On
// dispatch it matches the request, enforces the route's role requirement using
// the session-token store, and invokes the handler with an authenticated
// RequestContext. The table is guarded by a shared mutex so routes may be added
// or re-roled while the engine is serving.
class Router {
public:
    using RouteHandler = std::function<HandlerResult(const RequestContext&)>;

    explicit Router(TokenStore& tokens) : tokens_(tokens) {}

    // Register/replace a handler for (method, path).
    void add_route(http::verb method, std::string path, RouteHandler handler,
                   std::optional<Role> min_role,
                   std::optional<std::size_t> max_body_bytes = std::nullopt);

    // Register/replace a prefix handler for `method`: it matches any request whose
    // path starts with `prefix` when no exact (method, path) route matches. Longest
    // matching prefix wins. Used to serve a gated static subtree (see
    // WebEngine::serve_protected_files). The handler itself enforces any
    // authorization (so min_role is typically nullopt here).
    void add_prefix_route(http::verb method, std::string prefix,
                          RouteHandler handler,
                          std::optional<Role> min_role);

    // Set the minimum role for every method registered at `path`.
    // Returns true if at least one route matched.
    bool set_route_role(const std::string& path, std::optional<Role> min_role);

    // Match, authorize and invoke. Never throws; always returns a transport
    // result (ordinary response or fixed-length streaming download).
    HandlerResult dispatch(const Request& req) const;
    std::optional<std::size_t> body_limit(
        http::verb method, std::string_view target) const;

private:
    struct Route {
        RouteHandler        handler;
        std::optional<Role> min_role; // nullopt → public
        std::optional<std::size_t> max_body_bytes;
    };

    struct PrefixRoute {
        http::verb          method;
        std::string         prefix;
        RouteHandler        handler;
        std::optional<Role> min_role;
    };

    using Key = std::pair<http::verb, std::string>;

    std::map<Key, Route>      routes_;
    std::vector<PrefixRoute>  prefix_routes_;   // fallback when no exact route matches
    mutable std::shared_mutex mutex_;
    TokenStore&               tokens_;
};

} // namespace webengine
