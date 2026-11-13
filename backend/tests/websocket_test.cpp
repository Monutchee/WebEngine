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

// The attachment runs on the strand. Revoking before it returns makes queued
// delivery deterministic: the posted flush cannot run until authority is false.
void delivery_cutoff(int mode) {
    using Protocol = boost::asio::local::stream_protocol;
    boost::asio::io_context ioc;
    std::atomic<bool> valid{true};
    std::atomic<int> received{0};
    const auto path = "/tmp/webengine-ws-cutoff-" + std::to_string(::getpid()) + ".sock";
    Protocol::acceptor listener(ioc, Protocol::endpoint(path));
    listener.async_accept([&](auto error, auto socket) {
        require(!error, "cutoff accept");
        auto peer = std::make_shared<Protocol::socket>(std::move(socket));
        auto buffer = std::make_shared<boost::beast::flat_buffer>();
        auto request = std::make_shared<Request>();
        http::async_read(*peer, *buffer, *request, [&, peer, buffer, request](auto error, std::size_t) {
            require(!error, "cutoff handshake request");
            WebSocketUpgrade upgrade;
            upgrade.authorized = [&] { return valid.load(); };
            upgrade.receive = [&](auto, auto) { ++received; };
            upgrade.attach = [&](auto sink) {
                if (mode == 0) {
                    sink->send_control("queued-control");
                    sink->send_keyed("topic", "queued-snapshot");
                    valid = false;
                } else if (mode == 4) {
                    sink->send_latest("initial");
                } else {
                    valid = false;
                    if (mode == 1) sink->send_latest("denied-snapshot");
                    if (mode == 2) sink->send_control("denied-control");
                    if (mode == 3) sink->send_if_idle("denied-heartbeat");
                }
                return std::shared_ptr<void>{};
            };
            std::make_shared<WebSocketSession>(std::move(*peer), std::move(upgrade))->run(std::move(*request));
        });
    });
    std::thread server([&] { ioc.run(); });
    try {
        boost::asio::io_context client_ioc;
        boost::beast::websocket::stream<Protocol::socket> client(client_ioc);
        client.next_layer().connect(Protocol::endpoint(path));
        client.handshake("device", "/stream");
        boost::beast::flat_buffer buffer;
        if (mode == 4) {
            client.read(buffer);
            require(boost::beast::buffers_to_string(buffer.data()) == "initial", "receive initial");
            buffer.consume(buffer.size());
            valid = false;
            client.write(boost::asio::buffer(std::string("denied-command")));
        }
        boost::system::error_code error; client.read(buffer, error);
        require(error == boost::beast::websocket::error::closed, "cutoff drops pending/new output");
        require(client.reason().code == boost::beast::websocket::close_code::policy_error, "cutoff close1008");
        require(received == 0, "cutoff prevents command dispatch");
    } catch (...) { ioc.stop(); server.join(); std::filesystem::remove(path); throw; }
    server.join(); std::filesystem::remove(path);
}

void policy_composition() {
    TokenStore tokens;
    Router router(tokens);
    bool credential = true, product = true, throws = false;
    router.set_credential_policy([&](auto& context) -> std::optional<Response> {
        context.user = UserInfo{"integration", Role::Viewer};
        context.authorized = [&] { return credential; };
        return {};
    });
    router.add_route(http::verb::get, "/stream", [&](const auto&) -> HandlerResult {
        WebSocketUpgrade value;
        value.attach = [](auto) { return std::shared_ptr<void>{}; };
        value.authorized = [&] { if (throws) throw std::runtime_error("authority unavailable"); return product; };
        return value;
    }, Role::Viewer);
    Request request{http::verb::get, "/stream", 11};
    request.set(http::field::cookie, "session=" + tokens.issue("viewer", Role::Viewer));
    auto upgrade = std::get<WebSocketUpgrade>(router.dispatch(request));
    require(upgrade.authorized(), "policy credential admitted without session token");
    credential = false; require(!upgrade.authorized(), "policy credential revoked");
    credential = true; product = false; require(!upgrade.authorized(), "product gate revoked");
    product = true; require(upgrade.authorized(), "both gates required");
    throws = true; require(!upgrade.authorized(), "authority exception fails closed");
    throws = false;
    router.set_credential_policy([](auto& context) -> std::optional<Response> {
        context.user = UserInfo{"integration", Role::Viewer};
        return {};
    });
    require(std::get<Response>(router.dispatch(request)).result() == http::status::service_unavailable,
        "replacement principal without revalidator fails closed");
    router.add_route(http::verb::get, "/public", [](const auto&) -> HandlerResult {
        return WebSocketUpgrade{};
    }, std::nullopt);
    require(std::get<Response>(router.dispatch(Request{http::verb::get, "/public", 11})).result() == http::status::forbidden,
        "unauthenticated websocket policy preserved");
}

void dispatch_shutdown() {
    TokenStore tokens;
    auto router = std::make_unique<Router>(tokens);
    std::atomic<bool> entered{false}, release{false}, completed{false}, destroyed{false};
    router->add_route(http::verb::get, "/slow", [&](const auto&) -> HandlerResult {
        entered = true;
        while (!release) std::this_thread::sleep_for(1ms);
        return json(http::status::ok, "{}");
    }, std::nullopt);
    router->async_dispatch(Request{http::verb::get,"/slow",11}, [&](auto,auto) { completed = true; });
    for(int i=0;i<200&&!entered;++i) std::this_thread::sleep_for(1ms);
    require(entered,"dispatch starts");
    std::thread destroy([&] { router.reset(); destroyed = true; });
    std::this_thread::sleep_for(20ms);
    require(!destroyed,"router waits for in-flight handler");
    release = true;
    destroy.join();
    require(completed && destroyed,"handler completes before routing state destruction");
}

}
int main() {
    policy_composition();
    for (int mode = 0; mode < 5; ++mode) delivery_cutoff(mode);
    dispatch_shutdown();
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
    engine.add_websocket("/duplex", [](const auto&) -> std::variant<Response,WebSocketUpgrade> {
        WebSocketUpgrade value;
        value.attach=[](auto) {return std::shared_ptr<void>{};};
        value.receive=[](auto sink,std::string message) {
            if(message=="burst") {
                sink->send_control("ack-a"); sink->send_control("ack-b");
                sink->send_keyed("a","old-a"); sink->send_keyed("b","new-b");
                sink->send_keyed("a","new-a"); sink->send_keyed("gone","gone");
                sink->discard_key("gone"); sink->send_if_idle("heartbeat");
            } else if(message=="overload") for(int i=0;i<65;++i) sink->send_control("control");
        };
        return value;
    },Role::Viewer,4);
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
        auto duplex = [&](bool binary,std::string message,std::uint16_t expected) {
            boost::beast::websocket::stream<Protocol::socket> peer(ioc);
            peer.next_layer().connect(Protocol::endpoint(path));
            peer.set_option(boost::beast::websocket::stream_base::decorator([&](auto& req) {
                req.set(http::field::cookie,login("viewer")); req.set(http::field::origin,"https://device:8443"); req.set("X-Forwarded-Proto","https");
            }));
            peer.handshake("device:8443","/duplex"); peer.binary(binary); peer.write(boost::asio::buffer(message));
            if(!expected) {
                for(const auto* item:{"ack-a","ack-b","new-a","new-b"}) {
                    buffer.consume(buffer.size()); peer.read(buffer);
                    require(boost::beast::buffers_to_string(buffer.data())==item,"keyed latest/control priority/discard");
                }
                peer.close(boost::beast::websocket::close_code::normal);
            } else {
                boost::system::error_code closed;
                do {buffer.consume(buffer.size());peer.read(buffer,closed);} while(!closed);
                require(closed==boost::beast::websocket::error::closed,"bounded input/queue closes");
                require(peer.reason().code==expected,"bounded input/queue close code");
            }
        };
        duplex(false,"burst",0);
        duplex(true,"binary",1003);
        duplex(false,std::string(4097,'x'),1009);
        duplex(false,"overload",1013);
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
