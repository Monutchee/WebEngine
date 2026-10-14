#include "WebSocketSession.hpp"
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <algorithm>
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
    pending_.clear(); ready_.clear(); controls_.clear(); pending_bytes_ = 0;
    if (!shutdown_requested_.exchange(true))
        asio::post(strand_, [self = shared_from_this()] { self->stop(); });
}
void WebSocketSession::send_if_idle(std::string message) {
    std::lock_guard lock(mutex_);
    if (!active() || scheduled_ || message.size() > 256u * 1024u) return;
    pending_bytes_ += message.size();
    pending_[""] = std::move(message);
    ready_.push_back("");
    scheduled_ = true;
    asio::post(strand_, [self = shared_from_this()] { self->flush(); });
}
void WebSocketSession::run(Request request) {
    auto self = shared_from_this();
    asio::dispatch(strand_, [self, request = std::move(request)]() mutable {
        self->stream_.read_message_max(self->upgrade_.receive ? 4096 : 1024);
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
void WebSocketSession::terminate(std::uint16_t code) {
    std::lock_guard lock(mutex_);
    if (!active() || overload_posted_) return;
    overload_posted_ = true;
    asio::post(strand_, [self = shared_from_this(), code] { self->close(static_cast<ws::close_code>(code)); });
}
void WebSocketSession::send_latest(std::string message) { send_keyed("", std::move(message)); }
void WebSocketSession::discard_key(std::string_view key) {
    std::lock_guard lock(mutex_);
    const auto found = pending_.find(std::string(key));
    if (found == pending_.end()) return;
    pending_bytes_ -= found->second.size(); pending_.erase(found);
    std::erase(ready_, std::string(key));
}
void WebSocketSession::send_keyed(std::string key, std::string message) {
    std::unique_lock lock(mutex_);
    if (!active()) return;
    if (message.size() > 256u * 1024u) { lock.unlock(); terminate(1009); return; }
    const auto found = pending_.find(key);
    const auto previous = found == pending_.end() ? 0 : found->second.size();
    if (key.size() > 4096 ||
        (found == pending_.end() && !key.empty() && pending_.size() - pending_.count("") >= 32) ||
        pending_bytes_ - previous + message.size() > 2u * 1024u * 1024u) {
        lock.unlock(); terminate(1013); return;
    }
    pending_bytes_ = pending_bytes_ - previous + message.size();
    if (found == pending_.end()) ready_.push_back(key);
    pending_[std::move(key)] = std::move(message);
    if (scheduled_) return;
    scheduled_ = true;
    asio::post(strand_, [self = shared_from_this()] { self->flush(); });
}
void WebSocketSession::send_control(std::string message) {
    std::unique_lock lock(mutex_);
    if (!active()) return;
    if (controls_.size() >= 64 || message.size() > 256u * 1024u ||
        pending_bytes_ + message.size() > 2u * 1024u * 1024u) {
        lock.unlock(); terminate(1013); return;
    }
    pending_bytes_ += message.size(); controls_.push_back(std::move(message));
    if (scheduled_) return;
    scheduled_ = true;
    asio::post(strand_, [self = shared_from_this()] { self->flush(); });
}
void WebSocketSession::flush() {
    if (!active() || writing_ || closing_) return;
    auto message = std::make_shared<std::string>();
    {
        std::lock_guard lock(mutex_);
        if (!controls_.empty()) {
            *message = std::move(controls_.front()); controls_.pop_front();
        } else if (!ready_.empty()) {
            auto key = std::move(ready_.front()); ready_.pop_front();
            *message = std::move(pending_.at(key)); pending_.erase(key);
        } else { scheduled_ = false; return; }
        pending_bytes_ -= message->size();
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
            if (!self->stream_.got_text() || !self->upgrade_.receive) {
                self->close(ws::close_code::unknown_data); return;
            }
            auto message = boost::beast::buffers_to_string(self->incoming_.data());
            self->incoming_.consume(self->incoming_.size());
            try { self->upgrade_.receive(self, std::move(message)); }
            catch (...) { self->close(ws::close_code::internal_error); return; }
            if (self->active()) self->read();
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
    pending_.clear(); ready_.clear(); controls_.clear(); pending_bytes_ = 0;
}
}
