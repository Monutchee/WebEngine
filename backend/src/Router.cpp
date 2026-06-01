#include "Router.hpp"
#include "util.hpp"

namespace webengine {

void Router::add_route(http::verb method, std::string path, Handler handler,
                       std::optional<Role> min_role)
{
    std::unique_lock lock(mutex_);
    routes_[Key{method, std::move(path)}] = Route{std::move(handler), min_role};
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

Response Router::dispatch(const Request& req) const
{
    // Match on the path only; ignore any query string.
    auto target = req.target();
    std::string path(target.data(), target.size());
    if (auto q = path.find('?'); q != std::string::npos)
        path.resize(q);

    // Copy what we need out of the table, then release the lock before running
    // the handler — handlers may call back into the engine (e.g. set_api_role).
    Handler             handler;
    std::optional<Role> min_role;
    {
        std::shared_lock lock(mutex_);
        auto it = routes_.find(Key{req.method(), path});
        if (it == routes_.end())
            return text(http::status::not_found, "Not Found");
        handler  = it->second.handler;
        min_role = it->second.min_role;
    }

    RequestContext ctx{req, std::nullopt};

    // Enforce authentication/authorization for protected routes.
    if (min_role) {
        auto entry = validated_token(req, tokens_);
        if (!entry)
            return json(http::status::unauthorized,
                        R"({"error":"authentication required"})");
        if (!role_satisfies(entry->role, *min_role))
            return json(http::status::forbidden,
                        R"({"error":"insufficient permissions"})");
        ctx.user = UserInfo{entry->username, entry->role};
    }

    // Handlers are arbitrary user code; a throw must not escape into io_context::run()
    // (which would propagate out of a worker thread and call std::terminate). Contain
    // it and report 500 instead.
    try {
        return handler(ctx);
    } catch (...) {
        return json(http::status::internal_server_error,
                    R"({"error":"internal server error"})");
    }
}

} // namespace webengine
