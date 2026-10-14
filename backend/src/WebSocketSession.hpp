#pragma once
#include "webengine/Http.hpp"
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/core.hpp>
#include <atomic>
#include <mutex>
namespace webengine {
class WebSocketSession final : public WebSocketSink,
                               public std::enable_shared_from_this<WebSocketSession> {
public:
    using Socket = boost::asio::local::stream_protocol::socket;
    WebSocketSession(Socket socket, WebSocketUpgrade upgrade);
    ~WebSocketSession() override;
    void run(Request request);
    void send_latest(std::string message) override;
    bool active() const override { return active_.load(); }
    void deactivate() override;
    void send_if_idle(std::string message) override;
private:
    void read();
    void flush();
    void check_authority();
    void close(boost::beast::websocket::close_code code);
    void stop();
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    boost::beast::websocket::stream<Socket> stream_;
    boost::asio::steady_timer timer_;
    boost::asio::steady_timer close_timer_;
    boost::beast::flat_buffer incoming_;
    WebSocketUpgrade upgrade_;
    std::shared_ptr<void> attachment_;
    std::atomic<bool> active_{false};
    std::mutex mutex_;
    std::optional<std::string> pending_;
    bool scheduled_ = false; // under mutex: at most one drain posted
    bool writing_ = false; // strand-only
    bool closing_ = false;
    bool close_started_ = false;
    std::atomic<bool> shutdown_requested_{false};
    boost::beast::websocket::close_code close_code_{};
};
}
