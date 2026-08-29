#include "Router.hpp"
#include "util.hpp"

#include <glaze/glaze.hpp>

namespace webengine {

void Router::add_route(http::verb method, std::string path, RouteHandler handler,
                       std::optional<Role> min_role,
                       std::optional<std::size_t> max_body_bytes)
{
    std::unique_lock lock(mutex_);
    routes_[Key{method, std::move(path)}] =
        Route{std::move(handler), min_role, max_body_bytes};
}

std::optional<std::size_t> Router::body_limit(
    http::verb method, std::string_view target) const
{
    std::string path(target);
    if (auto query = path.find('?'); query != std::string::npos)
        path.resize(query);
    std::shared_lock lock(mutex_);
    const auto route = routes_.find(Key{method, path});
    return route == routes_.end() ? std::nullopt
                                 : route->second.max_body_bytes;
}

void Router::add_prefix_route(http::verb method, std::string prefix,
                              RouteHandler handler,
                              std::optional<Role> min_role)
{
    std::unique_lock lock(mutex_);
    for (auto& pr : prefix_routes_) {       // replace an existing (method, prefix)
        if (pr.method == method && pr.prefix == prefix) {
            pr.handler  = std::move(handler);
            pr.min_role = min_role;
            return;
        }
    }
    prefix_routes_.push_back({method, std::move(prefix), std::move(handler), min_role});
}

bool Router::set_route_role(const std::string& path, std::optional<Role> min_role)
{
    std::unique_lock lock(mutex_);
    bool found = false;
    for (auto& [key, route] : routes_) {
        if (key.second == path) {
            route.min_role = min_role;
            found = true;
        }
    }
    return found;
}

HandlerResult Router::dispatch(const Request& req) const
{
    // Match on the path only; ignore any query string.
    auto target = req.target();
    std::string path(target.data(), target.size());
    if (auto q = path.find('?'); q != std::string::npos)
        path.resize(q);

    // Copy what we need out of the table, then release the lock before running
    // the handler — handlers may call back into the engine (e.g. set_api_role).
    RouteHandler        handler;
    std::optional<Role> min_role;
    {
        std::shared_lock lock(mutex_);
        auto it = routes_.find(Key{req.method(), path});
        if (it != routes_.end()) {
            handler  = it->second.handler;
            min_role = it->second.min_role;
        } else {
            // No exact route — fall back to the longest matching prefix route.
            const PrefixRoute* best = nullptr;
            for (const auto& pr : prefix_routes_) {
                if (pr.method == req.method() && path.rfind(pr.prefix, 0) == 0)
                    if (!best || pr.prefix.size() > best->prefix.size())
                        best = &pr;
            }
            if (!best)
                return text(http::status::not_found, "Not Found");
            handler  = best->handler;
            min_role = best->min_role;
        }
    }

    RequestContext ctx{req, std::nullopt};

    // Enforce authentication/authorization for protected routes.
    if (min_role) {
        auto entry = validated_token(req, tokens_);
        if (!entry)
            return json(http::status::unauthorized,
                glz::write_json(glz::obj{"error", "authentication required"}).value_or(std::string{}));
        if (!role_satisfies(entry->role, *min_role))
            return json(http::status::forbidden,
                glz::write_json(glz::obj{"error", "insufficient permissions"}).value_or(std::string{}));
        ctx.user = UserInfo{entry->username, entry->role};
    }

    // Handlers are arbitrary user code; a throw must not escape into io_context::run()
    // (which would propagate out of a worker thread and call std::terminate). Contain
    // it and report 500 instead.
    try {
        return handler(ctx);
    } catch (...) {
        return json(http::status::internal_server_error,
            glz::write_json(glz::obj{"error", "internal server error"}).value_or(std::string{}));
    }
}

} // namespace webengine
