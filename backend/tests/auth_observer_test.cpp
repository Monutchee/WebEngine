#include <utility>
#include <boost/asio.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <vector>
#include <unistd.h>
#include "webengine/WebEngine.hpp"

namespace {
using namespace webengine;
class Provider final : public AuthProvider {
public:
    std::optional<Role> authenticate(const std::string& name, const std::string& password) override {
        if (password != "secret") return {};
        if (name == "admin") return Role::Admin;
        if (name == "guest") return Role::Guest;
        return {};
    }
    bool supports_management() const override { return true; }
    bool add_user(const std::string& name, const std::string&, Role) override { return name != "fail"; }
    bool remove_user(const std::string& name) override { return name == "existing"; }
    bool set_user_role(const std::string& name, Role) override {
        if (name == "throw") throw std::runtime_error("private provider detail");
        return name == "existing";
    }
};
void require(bool condition) { if (!condition) throw std::runtime_error("auth observer assertion failed"); }
}
int main() {
    using Socket = boost::asio::local::stream_protocol;
    const std::string path = "/tmp/webengine-auth-" + std::to_string(::getpid()) + ".sock";
    WebEngine engine(std::make_shared<Provider>());
    std::vector<AuthEvent> events;
    std::mutex event_mutex;
    auto last = [&] { std::lock_guard lock(event_mutex); return events.back(); };
    auto count_events = [&] { std::lock_guard lock(event_mutex); return events.size(); };
    engine.set_socket_path(path).set_threads(1).enable_auth_endpoints().enable_admin_endpoints()
        .set_auth_observer([&](const AuthEvent& event) { { std::lock_guard lock(event_mutex); events.push_back(event); } throw std::runtime_error("observer failure"); })
        .add_api(http::verb::get, "/events", [](const RequestContext& ctx) {
            require(ctx.user.has_value()); return json(http::status::ok, "{}");
        }, Role::Guest);
    std::exception_ptr server_error;
    std::thread server([&] { try { engine.run(); } catch (...) { server_error = std::current_exception(); } });
    auto cleanup = [&] { engine.stop(); server.join(); std::filesystem::remove(path); };
    try {
        for (int i = 0; i < 200 && !std::filesystem::exists(path); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auto request = [&](http::verb method, const char* target, std::string body = {}, std::string cookie = {}) {
            boost::asio::io_context ioc;
            Socket::socket socket(ioc); socket.connect(Socket::endpoint(path));
            Request req{method, target, 11}; req.keep_alive(false); req.body() = std::move(body);
            if (!cookie.empty()) req.set(http::field::cookie, cookie);
            req.prepare_payload(); http::write(socket, req);
            boost::beast::flat_buffer buffer; Response response; http::read(socket, buffer, response); return response;
        };
        auto login = [&](const char* name) {
            auto response = request(http::verb::post, "/api/login", std::string("{\"username\":\"") + name + "\",\"password\":\"secret\"}");
            require(response.result() == http::status::ok);
            return std::string(response[http::field::set_cookie]);
        };
        require(request(http::verb::get, "/events").result() == http::status::unauthorized);
        require(request(http::verb::post, "/api/login", "{\"username\":\"secret-user\",\"password\":\"bad\"}").result() == http::status::unauthorized);
        require(last().operation == AuthOperation::login && !last().succeeded && last().actor.empty() && last().subject.empty());
        require(request(http::verb::post, "/api/login", "{\"username\":\"admin\",\"password\":\"secret\",garbage").result() == http::status::unauthorized);
        auto guest = login("guest");
        require(request(http::verb::get, "/events", {}, guest).result() == http::status::ok);
        auto admin = login("admin");
        require(last().succeeded && last().actor == "admin");
        require(request(http::verb::post, "/api/admin/users", "{\"username\":\"fail\",\"password\":\"hidden\",\"role\":\"guest\"}", admin).result() == http::status::internal_server_error);
        require(!last().succeeded && last().actor == "admin" && last().subject == "fail");
        require(request(http::verb::post, "/api/admin/users", "{\"username\":\"new\",\"password\":\"hidden\",\"role\":\"guest\"}", admin).result() == http::status::ok);
        require(last().operation == AuthOperation::user_saved && last().succeeded);
        require(request(http::verb::delete_, "/api/admin/users", "{\"username\":\"absent\"}", admin).result() == http::status::not_found);
        require(last().operation == AuthOperation::user_removed && !last().succeeded);
        require(request(http::verb::patch, "/api/admin/users", "{\"username\":\"throw\",\"role\":\"guest\"}", admin).result() == http::status::internal_server_error);
        require(last().operation == AuthOperation::role_changed && !last().succeeded);
        auto count = count_events();
        request(http::verb::post, "/api/logout"); require(count_events() == count);
        request(http::verb::post, "/api/logout", {}, guest);
        require(last().operation == AuthOperation::logout && last().actor == "guest");
        require(request(http::verb::get, "/events", {}, guest).result() == http::status::unauthorized);
        engine.set_auth_observer({});
        count = count_events(); login("admin"); require(count_events() == count);
        cleanup(); if (server_error) std::rethrow_exception(server_error);
    } catch (...) { cleanup(); throw; }
}
