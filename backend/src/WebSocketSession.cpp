#include "WebSocketSession.hpp"
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/post.hpp>
namespace webengine {
namespace asio = boost::asio;
namespace ws = boost::beast::websocket;
using namespace std::chrono_literals;
WebSocketSession::WebSocketSession(Socket socket, WebSocketUpgrade upgrade)
    : strand_(asio::make_strand(static_cast<asio::io_context&>(socket.get_executor().context()))), stream_(std::move(socket)),
      timer_(strand_), close_timer_(strand_), upgrade_(std::move(upgrade)) {}
WebSocketSession::~WebSocketSession() { active_ = false; }
void WebSocketSession::deactivate() {
    std::lock_guard lock(mutex_);
    active_ = false;
    pending_.reset();
    if (!shutdown_requested_.exchange(true))
        asio::post(strand_, [self = shared_from_this()] { self->stop(); });
}
void WebSocketSession::send_if_idle(std::string message) {
    std::lock_guard lock(mutex_);
    if (!active() || scheduled_ || message.size() > 256u * 1024u) return;
    pending_ = std::move(message);
    scheduled_ = true;
    asio::post(strand_, [self = shared_from_this()] { self->flush(); });
}
void WebSocketSession::run(Request request) {
    auto self = shared_from_this();
    asio::dispatch(strand_, [self, request = std::move(request)]() mutable {
        self->stream_.read_message_max(1024);
        self->stream_.set_option(ws::stream_base::timeout::suggested(boost::beast::role_type::server));
        auto retained_request = std::make_shared<Request>(std::move(request));
        self->stream_.async_accept(*retained_request, asio::bind_executor(self->strand_, [self, retained_request](auto error) {
            if (error) { self->stop(); return; }
            if (self->shutdown_requested_) { self->stop(); return; }
            self->active_ = true;
            self->check_authority();
            if (!self->active()) return;
            try { self->attachment_ = self->upgrade_.attach(self); }
            catch (...) { self->close(ws::close_code::internal_error); return; }
            if (self->active()) self->read();
        }));
    });
}
void WebSocketSession::send_latest(std::string message) {
    if (!active()) return;
    std::lock_guard lock(mutex_);
    if (!active()) return;
    if (message.size() > 256u * 1024u) {
        asio::post(strand_, [self = shared_from_this()] { self->close(ws::close_code::too_big); });
        return;
    }
    pending_ = std::move(message);
    if (scheduled_) return;
    scheduled_ = true;
    asio::post(strand_, [self = shared_from_this()] { self->flush(); });
}
void WebSocketSession::flush() {
    if (!active() || writing_ || closing_) return;
    auto message = std::make_shared<std::string>();
    {
        std::lock_guard lock(mutex_);
        if (!pending_) { scheduled_ = false; return; }
        *message = std::move(*pending_);
        pending_.reset();
    }
    writing_ = true;
    stream_.text(true);
    stream_.async_write(asio::buffer(*message), asio::bind_executor(strand_,
        [self = shared_from_this(), message](auto error, std::size_t) {
            self->writing_ = false;
            if (error) { self->stop(); return; }
            if (self->closing_) self->close(self->close_code_);
            else self->flush();
        }));
}
void WebSocketSession::read() {
    stream_.async_read(incoming_, asio::bind_executor(strand_,
        [self = shared_from_this()](auto error, std::size_t) {
            if (error) { self->stop(); return; }
            self->incoming_.consume(self->incoming_.size());
            // This read-only stream has no application commands. Ping/pong and
            // close frames remain handled by Beast; data frames are unsupported.
            self->close(ws::close_code::unknown_data);
        }));
}
void WebSocketSession::check_authority() {
    if (!active() || closing_) return;
    bool valid = false;
    try { valid = upgrade_.authorized && upgrade_.authorized(); } catch (...) {}
    if (!valid) { close(ws::close_code::policy_error); return; }
    timer_.expires_after(1s);
    timer_.async_wait(asio::bind_executor(strand_, [self = shared_from_this()](auto error) {
        if (!error) self->check_authority();
    }));
}
void WebSocketSession::close(ws::close_code code) {
    if (!active() && !closing_) return;
    if (!closing_) {
        closing_ = true;
        close_code_ = code;
        active_ = false;
        attachment_.reset();
        timer_.cancel();
        close_timer_.expires_after(2s);
        close_timer_.async_wait(asio::bind_executor(strand_, [self = shared_from_this()](auto error) {
            if (!error) self->stop();
        }));
    }
    if (writing_ || close_started_) return;
    close_started_ = true;
    stream_.async_close(close_code_, asio::bind_executor(strand_, [self = shared_from_this()](auto) { self->stop(); }));
}
void WebSocketSession::stop() {
    active_ = false;
    attachment_.reset();
    upgrade_.reservation.reset();
    timer_.cancel();
    close_timer_.cancel();
    boost::system::error_code ignored;
    stream_.next_layer().cancel(ignored);
    stream_.next_layer().close(ignored);
    std::lock_guard lock(mutex_);
    pending_.reset();
}
}
