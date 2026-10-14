#include "webengine/WebEngine.hpp"
#include "WebSocketOrigin.hpp"
#include "Router.hpp"
#include "WebSocketSession.hpp"
#include <utility>
#include <boost/asio.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <unistd.h>
namespace {
using namespace webengine;
using namespace std::chrono_literals;
void require(bool condition, const char* what) { if (!condition) throw std::runtime_error(what); }
class Provider final : public AuthProvider {
public:
    std::optional<Role> authenticate(const std::string& user, const std::string& password) override {
        if (password != "secret") return {};
        return user == "viewer" ? Role::Viewer : Role::Guest;
    }
};
void established_expiry() {
    using Protocol = boost::asio::local::stream_protocol;
    boost::asio::io_context ioc;
    TokenStore tokens;
    const auto token = tokens.issue("viewer", Role::Viewer, 2s);
    const auto path = "/tmp/webengine-ws-expiry-" + std::to_string(::getpid()) + ".sock";
    Protocol::acceptor listener(ioc, Protocol::endpoint(path));
    listener.async_accept([&](auto error, auto socket) {
        require(!error, "expiry accept");
        auto peer = std::make_shared<Protocol::socket>(std::move(socket));
        auto buffer = std::make_shared<boost::beast::flat_buffer>();
        auto request = std::make_shared<Request>();
        http::async_read(*peer, *buffer, *request, [&, peer, buffer, request](auto read_error, std::size_t) {
            require(!read_error, "expiry handshake request");
            WebSocketUpgrade upgrade;
            upgrade.authorized = [&] { return tokens.validate(token).has_value(); };
            upgrade.attach = [](auto sink) { sink->send_latest("initial"); return std::shared_ptr<void>{}; };
            std::make_shared<WebSocketSession>(std::move(*peer), std::move(upgrade))->run(std::move(*request));
        });
    });
    std::thread server([&] { ioc.run(); });
    try {
        boost::asio::io_context client_ioc;
        boost::beast::websocket::stream<Protocol::socket> client(client_ioc);
        client.next_layer().connect(Protocol::endpoint(path));
        client.handshake("device", "/stream");
        boost::beast::flat_buffer buffer; client.read(buffer);
        require(boost::beast::buffers_to_string(buffer.data()) == "initial", "active before expiry");
        buffer.consume(buffer.size());
        boost::system::error_code error; client.read(buffer, error);
        require(error == boost::beast::websocket::error::closed, "expiry closes established socket");
        require(client.reason().code == boost::beast::websocket::close_code::policy_error, "expiry close1008");
    } catch (...) { ioc.stop(); server.join(); std::filesystem::remove(path); throw; }
    server.join(); std::filesystem::remove(path);
}

}
int main() {
    Request origin{http::verb::get, "/stream", 11};
    origin.set(http::field::host, "device:8443"); origin.set("X-Forwarded-Proto", "https");
    origin.set(http::field::origin, "https://device:8443");
    require(websocket_same_origin(origin), "same custom port");
    for (const auto* wrong : {"https://device", "https://evil:8443", "null", "https://device:8443/", "http://device:8443"}) {
        origin.set(http::field::origin, wrong); require(!websocket_same_origin(origin), "cross origin denied");
    }
    origin.set(http::field::host, "[::1]:8443"); origin.set(http::field::origin, "https://[::1]:8443");
    require(websocket_same_origin(origin), "IPv6 port");
    origin.set(http::field::host, "device:443"); origin.set(http::field::origin, "https://device");
    require(websocket_same_origin(origin), "default port equivalent");
    TokenStore tokens;
    Router router(tokens);
    router.add_route(http::verb::get, "/stream", [](const auto&) -> HandlerResult {
        WebSocketUpgrade upgrade; upgrade.attach = [](auto) { return std::shared_ptr<void>{}; }; return upgrade;
    }, Role::Viewer);
    Request authorization{http::verb::get, "/stream", 11};
    const auto token = tokens.issue("viewer", Role::Viewer);
    authorization.set(http::field::cookie, "session=" + token);
    auto upgrade = std::get<WebSocketUpgrade>(router.dispatch(authorization));
    require(upgrade.authorized(), "valid session"); tokens.revoke(token);
    require(!upgrade.authorized(), "revoked attachment fails");
    authorization.set(http::field::cookie, "session=" + tokens.issue("viewer", Role::Viewer, -1s));
    require(std::get<Response>(router.dispatch(authorization)).result() == http::status::unauthorized, "expired token denied");

    authorization.set(http::field::cookie, "session=" + tokens.issue("viewer", Role::Viewer, 1s));
    auto expiring = std::get<WebSocketUpgrade>(router.dispatch(authorization));
    require(expiring.authorized(), "attached session before expiry");
    std::this_thread::sleep_for(1100ms);
    require(!expiring.authorized(), "attached session after expiry");

    using Protocol = boost::asio::local::stream_protocol;
    const std::string path = "/tmp/webengine-ws-" + std::to_string(::getpid()) + ".sock";
    WebEngine engine(std::make_shared<Provider>());
    std::atomic<int> attachments{0};
    engine.set_socket_path(path).set_threads(2).enable_auth_endpoints()
        .add_websocket("/stream", [&](const auto&) -> std::variant<Response, WebSocketUpgrade> {
            WebSocketUpgrade value;
            value.attach = [&](auto sink) {
                ++attachments;
                // A burst from one source must not build an unbounded transport queue.
                for (int i = 0; i < 10000; ++i) sink->send_latest(std::to_string(i));
                sink->send_if_idle("heartbeat");
                return std::shared_ptr<void>{};
            };
            return value;
        }, Role::Viewer, 1);
    std::thread server([&] { engine.run(); });
    auto cleanup = [&] { engine.stop(); server.join(); std::filesystem::remove(path); };
    try {
        for (int i = 0; i < 200 && !std::filesystem::exists(path); ++i) std::this_thread::sleep_for(5ms);
        auto request = [&](http::verb method, std::string target, std::string cookie = {}, bool ws = false, std::string supplied_origin = "https://device:8443", std::string body = "") {
            boost::asio::io_context ioc;
            Protocol::socket socket(ioc); socket.connect(Protocol::endpoint(path));
            Request req{method, target, 11}; req.keep_alive(false); req.body() = body;
            req.set(http::field::host, "device:8443"); req.set("X-Forwarded-Proto", "https"); req.set(http::field::origin, supplied_origin);
            if (!cookie.empty()) req.set(http::field::cookie, cookie);
            if (ws) { req.set(http::field::upgrade, "websocket"); req.set(http::field::connection, "upgrade"); req.set(http::field::sec_websocket_version, "13"); req.set(http::field::sec_websocket_key, "dGhlIHNhbXBsZSBub25jZQ=="); }
            req.prepare_payload(); http::write(socket, req);
            boost::beast::flat_buffer buffer; Response result; http::read(socket, buffer, result); return result;
        };
        auto login = [&](std::string user) {
            auto response = request(http::verb::post, "/api/login", {}, false, "", "{\"username\":\"" + user + "\",\"password\":\"secret\"}");
            require(response.result() == http::status::ok, "login"); return std::string(response[http::field::set_cookie]);
        };
        require(request(http::verb::get, "/stream", {}, true).result() == http::status::unauthorized, "anonymous upgrade denied");
        const auto cookie = login("viewer");
        require(request(http::verb::get, "/stream", login("guest"), true).result() == http::status::forbidden, "guest upgrade denied");
        require(request(http::verb::get, "/stream", cookie, false).result() == http::status::upgrade_required, "ordinary GET rejected");
        require(request(http::verb::get, "/stream", cookie, true, "https://evil:8443").result() == http::status::forbidden, "bad origin upgrade denied");
        require(request(http::verb::get, "/stream?token=forbidden", cookie, true).result() == http::status::forbidden, "query forbidden");
        boost::asio::io_context ioc;
        boost::beast::websocket::stream<Protocol::socket> socket(ioc);
        socket.next_layer().connect(Protocol::endpoint(path));
        socket.set_option(boost::beast::websocket::stream_base::decorator([&](auto& req) {
            req.set(http::field::cookie, cookie); req.set(http::field::origin, "https://device:8443"); req.set("X-Forwarded-Proto", "https");
        }));
        socket.handshake("device:8443", "/stream");
        boost::beast::flat_buffer buffer; socket.read(buffer);
        require(boost::beast::buffers_to_string(buffer.data()) == "9999", "latest-only burst"); buffer.consume(buffer.size());
        require(attachments == 1, "one attachment");
        require(request(http::verb::get, "/stream", cookie, true).result() == http::status::service_unavailable, "connection cap");
        require(request(http::verb::post, "/api/logout", cookie).result() == http::status::ok, "logout");
        boost::system::error_code error; socket.read(buffer, error);
        require(error == boost::beast::websocket::error::closed, "revocation closes socket");
        require(socket.reason().code == boost::beast::websocket::close_code::policy_error, "auth close1008");
        // Stop with a live attachment: producer calls must be disabled before ioc destruction.
        const auto second_cookie = login("viewer");
        boost::beast::websocket::stream<Protocol::socket> second(ioc); second.next_layer().connect(Protocol::endpoint(path));
        second.set_option(boost::beast::websocket::stream_base::decorator([&](auto& req) {
            req.set(http::field::cookie, second_cookie); req.set(http::field::origin, "https://device:8443"); req.set("X-Forwarded-Proto", "https");
        }));
        second.handshake("device:8443", "/stream");
        second.read(buffer);
        engine.stop(); server.join();
        std::filesystem::remove(path);
        server = std::thread([&] { engine.run(); });
        for (int i = 0; i < 200 && !std::filesystem::exists(path); ++i) std::this_thread::sleep_for(5ms);
        const auto restart_cookie = login("viewer");
        boost::beast::websocket::stream<Protocol::socket> restarted(ioc);
        restarted.next_layer().connect(Protocol::endpoint(path));
        restarted.set_option(boost::beast::websocket::stream_base::decorator([&](auto& req) {
            req.set(http::field::cookie, restart_cookie); req.set(http::field::origin, "https://device:8443"); req.set("X-Forwarded-Proto", "https");
        }));
        restarted.handshake("device:8443", "/stream"); buffer.consume(buffer.size());
        restarted.read(buffer);
        require(boost::beast::buffers_to_string(buffer.data()) == "9999", "run-stop-run stream works");
    } catch (...) { cleanup(); throw; }
    cleanup();
    established_expiry();
}
