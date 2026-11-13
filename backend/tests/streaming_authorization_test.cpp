#include <utility>
#include <boost/asio.hpp>
#include <boost/asio/local/connect_pair.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "Router.hpp"
#include "Session.hpp"

using namespace webengine;
using namespace std::chrono_literals;
namespace asio = boost::asio;
namespace beast = boost::beast;
using uds = asio::local::stream_protocol;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static Request request(const std::string& token = {}) {
    Request req{http::verb::get, "/data", 11};
    req.keep_alive(false);
    if (!token.empty()) req.set(http::field::cookie, "session=" + token);
    return req;
}
static StreamingDownload data(std::function<void(std::uint64_t)> on_read = {},
                              std::function<bool()> authorized = {}) {
    return {"data.bin", "application/octet-stream", 150000,
        [on_read](std::uint64_t offset, std::span<std::byte> out) {
            if (on_read) on_read(offset);
            std::memset(out.data(), 'x', out.size());
            return out.size();
        }, std::move(authorized)};
}
struct Result { Response response; beast::error_code error; };
static Result transfer(Router& router, Request req) {
    asio::io_context server_io, client_io;
    auto work = asio::make_work_guard(server_io);
    uds::socket server(server_io), client(client_io);
    asio::local::connect_pair(server, client);
    std::make_shared<Session>(std::move(server), router)->run();
    std::thread worker([&] { server_io.run(); });
    Result result;
    beast::flat_buffer buffer;
    http::write(client, req);
    http::response_parser<http::string_body> parser;
    http::read(client, buffer, parser, result.error);
    result.response = parser.release();
    work.reset();
    worker.join();
    return result;
}
int main() try {
    TokenStore tokens;
    Router router(tokens);
    auto token = tokens.issue("reader", Role::Viewer);
    router.add_route(http::verb::get, "/data", [](const auto&) { return data(); }, Role::Viewer);
    auto active = transfer(router, request(token));
    require(!active.error && active.response.body().size() == 150000, "active session stream failed");
    require(active.response[http::field::cache_control] == "no-store", "authenticated stream must not be cached");

    // Logout uses this same token-store revoke operation. Revoke while the next
    // buffer is being produced: the newly read bytes must never be written.
    router.add_route(http::verb::get, "/data", [&](const auto&) {
        return data([&](auto offset) { if (offset) tokens.revoke(token); });
    }, Role::Viewer);
    auto revoked = transfer(router, request(token));
    require(revoked.error && revoked.response.body().size() == 65536, "revoked session sent another buffer");

    token = tokens.issue("reader", Role::Viewer);
    unsigned reads = 0, checks_after_read = 0;
    router.add_route(http::verb::get, "/data", [&](const auto&) {
        return data([&](auto) { ++reads; }, [&] {
            if (reads && ++checks_after_read == 2) { tokens.revoke(token); return false; }
            return true;
        });
    }, Role::Viewer);
    auto between = transfer(router, request(token));
    require(between.error && reads == 1 && between.response.body().size() == 65536,
            "revocation between buffers did not stop before next read");

    token = tokens.issue("reader", Role::Viewer, 1s);
    auto expiring = std::get<StreamingDownload>(router.dispatch(request(token)));
    std::this_thread::sleep_for(1100ms);
    require(!expiring.authorized(), "expired session remained authorized");

    token = tokens.issue("reader", Role::Viewer, 1s);
    router.add_route(http::verb::get, "/data", [](const auto&) {
        return data([](auto) { std::this_thread::sleep_for(1100ms); });
    }, Role::Viewer);
    auto expired = transfer(router, request(token));
    require(expired.error && expired.response.body().empty(), "session expiry during read sent bytes");

    token = tokens.issue("reader", Role::Viewer);
    bool permit = true;
    router.add_route(http::verb::get, "/data", [&](const auto&) {
        return data({}, [&] { return permit; });
    }, Role::Viewer);
    auto composed = std::get<StreamingDownload>(router.dispatch(request(token)));
    require(composed.authorized(), "handler/session composition denied active stream");
    permit = false;
    require(!composed.authorized(), "handler predicate was ignored");
    permit = true;
    tokens.revoke(token);
    require(!composed.authorized(), "handler predicate bypassed revoked session");

    token = tokens.issue("reader", Role::Viewer);
    tokens.set_revision_validator([](const auto&) -> std::uint64_t { throw std::runtime_error("unavailable"); });
    require(std::get<Response>(router.dispatch(request(token))).result() == http::status::service_unavailable,
            "authentication exception did not return fixed 503");
    tokens.set_revision_validator({});
    auto credential = std::get<StreamingDownload>(router.dispatch(request(token)));
    tokens.set_revision_validator([](const auto&) -> std::uint64_t { throw std::runtime_error("unavailable"); });
    require(!credential.authorized(), "credential exception did not fail closed");
    tokens.set_revision_validator({});

    bool handler_permit = true;
    router.add_route(http::verb::get, "/data", [&](const auto&) {
        return data({}, [&] { return handler_permit; });
    }, Role::Viewer);
    router.set_credential_policy([&](RequestContext& ctx) -> std::optional<Response> {
        ctx.user = UserInfo{"external", Role::Viewer};
        ctx.authorized = [&] { return permit; };
        return {};
    });
    auto policy = std::get<StreamingDownload>(router.dispatch(request()));
    require(policy.authorized(), "non-session policy credential rejected");
    handler_permit = false;
    require(!policy.authorized(), "policy credential bypassed handler predicate");
    handler_permit = true;
    permit = false;
    require(!policy.authorized(), "policy revocation ignored");
    router.set_credential_policy([](RequestContext& ctx) -> std::optional<Response> {
        ctx.user = UserInfo{"external", Role::Viewer};
        return {};
    });
    auto missing = std::get<Response>(router.dispatch(request()));
    require(missing.result() == http::status::service_unavailable &&
            missing.body() == R"({"error":"stream credential revalidation unavailable"})",
            "missing revalidator did not return fixed 503");
    // A valid session must not authorize a different principal installed by a
    // policy that supplies no validator, including a role-only replacement.
    token = tokens.issue("reader", Role::Viewer);
    auto replaced = std::get<Response>(router.dispatch(request(token)));
    require(replaced.result() == http::status::service_unavailable &&
            replaced.body() == missing.body(), "replacement principal inherited session validator");
    router.set_credential_policy([](RequestContext& ctx) -> std::optional<Response> {
        ctx.user = UserInfo{"reader", Role::Admin};
        return {};
    });
    auto elevated = std::get<Response>(router.dispatch(request(token)));
    require(elevated.result() == http::status::service_unavailable &&
            elevated.body() == missing.body(), "replacement role inherited session validator");
    router.set_credential_policy([&](RequestContext& ctx) -> std::optional<Response> {
        ctx.user = UserInfo{"reader", Role::Viewer};
        ctx.authorized = [&] { return permit; };
        return {};
    });
    auto constrained = std::get<StreamingDownload>(router.dispatch(request(token)));
    require(!constrained.authorized(), "same-principal policy validator was replaced");
    permit = true;
    require(constrained.authorized(), "same-principal policy validator did not recover");
    router.set_credential_policy({});

    bool did_read = false;
    router.add_route(http::verb::get, "/data", [&](const auto&) {
        return data([&](auto) { did_read = true; }, [&] {
            if (did_read) throw std::runtime_error("denied after read");
            return true;
        });
    }, std::nullopt);
    auto failed_after_read = transfer(router, request());
    require(failed_after_read.error && did_read && failed_after_read.response.body().empty(),
            "predicate exception after read sent bytes");
    router.add_route(http::verb::get, "/data", [](const auto&) {
        return data({}, []() -> bool { throw std::runtime_error("denied"); });
    }, std::nullopt);
    auto denied = transfer(router, request());
    require(denied.error && denied.response.body().empty(), "predicate exception sent bytes");
    router.add_route(http::verb::get, "/data", [](const auto&) { return data(); }, std::nullopt);
    require(!transfer(router, request()).error, "public streaming behavior changed");
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
