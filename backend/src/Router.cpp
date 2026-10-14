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
    CredentialPolicy credential_policy;
    {
        std::shared_lock lock(mutex_);
        credential_policy = credential_policy_;
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

    try {
        if (auto entry = validated_token(req, tokens_)) ctx.user = UserInfo{entry->username, entry->role};
        if (credential_policy) if (auto denial = credential_policy(ctx)) return *denial;
    } catch (...) { return json(http::status::service_unavailable, R"({"error":"authentication unavailable"})"); }
    if (min_role) {
        if (!ctx.user) {
            auto response = json(http::status::unauthorized, R"({"error":"authentication required"})");
            response.set(http::field::www_authenticate, "Bearer realm=\"device\"");
            return response;
        }
        if (!role_satisfies(ctx.user->role, *min_role))
            return json(http::status::forbidden, R"({"error":"insufficient permissions"})");
    }

    // Handlers are arbitrary user code; a throw must not escape into io_context::run()
    // (which would propagate out of a worker thread and call std::terminate). Contain
    // it and report 500 instead.
    try {
        auto result = handler(ctx);
        if (auto* upgrade = std::get_if<WebSocketUpgrade>(&result)) {
            if (!min_role) return text(http::status::forbidden, "WebSocket requires authentication");
            std::uint64_t generation;
            { std::lock_guard lock(streams_mutex_); generation = stream_generation_; }
            auto attach = std::move(upgrade->attach);
            upgrade->attach = [this, generation, attach = std::move(attach)](std::shared_ptr<WebSocketSink> sink) {
                {
                    std::lock_guard lock(streams_mutex_);
                    if (streams_stopped_ || generation != stream_generation_) { sink->deactivate(); return std::shared_ptr<void>{}; }
                    std::erase_if(streams_, [](const auto& weak) { return weak.expired(); });
                    streams_.push_back(sink);
                }
                return attach(std::move(sink));
            };
            const auto token = extract_token(req);
            upgrade->authorized = [this, token, required = *min_role] {
                try { const auto entry = tokens_.validate(token);
                return entry && role_satisfies(entry->role, required); } catch (...) { return false; }
            };
        }
        return result;
    } catch (...) {
        return json(http::status::internal_server_error,
            glz::write_json(glz::obj{"error", "internal server error"}).value_or(std::string{}));
    }
}

void Router::async_dispatch(Request request, std::function<void(Request, HandlerResult)> done) {
    std::lock_guard admission(dispatch_mutex_);
    if (work_stopped_ || pending_ >= 64) {
        done(std::move(request), json(http::status::service_unavailable, R"({"error":"request queue full"})"));
        return;
    }
    ++pending_;
    boost::asio::post(workers_, [this, request = std::move(request), done = std::move(done)]() mutable {
        auto result = dispatch(request);
        --pending_;
        done(std::move(request), std::move(result));
    });
}

void Router::join_dispatch() {
    {
        std::lock_guard admission(dispatch_mutex_);
        work_stopped_ = true;
    }
    workers_.join();
}

void Router::reset_websockets() {
    std::lock_guard lock(streams_mutex_);
    ++stream_generation_;
    streams_stopped_ = false;
}
void Router::stop_websockets() {
    std::lock_guard lock(streams_mutex_);
    streams_stopped_ = true;
    for (auto& weak : streams_) if (auto sink = weak.lock()) sink->deactivate();
    streams_.clear();
}

} // namespace webengine
