#include "Session.hpp"

#include <algorithm>
#include <variant>

namespace webengine {

struct Session::StreamingState {
    static constexpr std::size_t buffer_size = 64u * 1024u;

    StreamingState(StreamingDownload value, unsigned version,
                   bool keep_connection_alive)
        : download(std::move(value)),
          response(http::status::ok, version),
          serializer(response),
          keep_alive(keep_connection_alive)
    {
        response.set(http::field::content_type, download.content_type);
        response.set("Content-Disposition",
            "attachment; filename=\"" + download.file_name + "\"");
        response.set(http::field::cache_control, "no-store");
        response.set(http::field::pragma, "no-cache");
        response.set(http::field::expires, "0");
        response.keep_alive(keep_alive);
        response.content_length(download.content_length);
        response.body().more = download.content_length != 0;
        serializer.split(true);
    }

    StreamingDownload download;
    http::response<http::buffer_body> response;
    http::response_serializer<http::buffer_body> serializer;
    std::array<std::byte, buffer_size> buffer{};
    std::uint64_t offset{};
    bool buffer_loaded{};
    bool keep_alive{};
};

Session::Session(uds::socket socket, Router& router)
    : socket_(std::move(socket)), router_(router) {}

void Session::run() { do_read(); }

void Session::do_read()
{
    parser_.emplace();
    auto self = shared_from_this();
    http::async_read_header(socket_, buffer_, *parser_,
        [self](beast::error_code ec, std::size_t)
        {
            if (ec == http::error::end_of_stream) {
                beast::error_code ec2;
                self->socket_.shutdown(uds::socket::shutdown_send, ec2);
                return;
            }
            if (ec) return;
            const auto& request = self->parser_->get();
            if (const auto limit = self->router_.body_limit(
                    request.method(), request.target()))
                self->parser_->body_limit(*limit);
            self->do_read_body();
        });
}

void Session::do_read_body()
{
    auto self = shared_from_this();
    http::async_read(socket_, buffer_, *parser_,
        [self](beast::error_code ec, std::size_t)
        {
            if (ec == http::error::body_limit) {
                self->do_write(text(http::status::payload_too_large,
                    "request body exceeds route limit"), false);
                return;
            }
            if (ec) return;
            auto request = self->parser_->release();
            bool keep_alive = request.keep_alive();
            auto result = self->router_.dispatch(request);
            if (auto* response = std::get_if<Response>(&result)) {
                response->keep_alive(keep_alive);
                response->prepare_payload();
                self->do_write(std::move(*response), keep_alive);
            } else {
                self->do_write(
                    std::get<StreamingDownload>(std::move(result)),
                    request.version(), keep_alive);
            }
        });
}

void Session::do_write(Response res, bool keep_alive)
{
    auto self = shared_from_this();
    auto sp   = std::make_shared<Response>(std::move(res));
    http::async_write(socket_, *sp,
        [self, sp, keep_alive](beast::error_code ec, std::size_t)
        {
            if (ec) return;
            self->finish_write(keep_alive);
        });
}

void Session::do_write(StreamingDownload download, unsigned version,
                       bool keep_alive)
{
    auto self = shared_from_this();
    auto state = std::make_shared<StreamingState>(
        std::move(download), version, keep_alive);
    http::async_write_header(socket_, state->serializer,
        [self, state](beast::error_code ec, std::size_t)
        {
            if (ec) return;
            if (state->serializer.is_done()) {
                self->finish_write(state->keep_alive);
                return;
            }
            self->do_write_stream(state);
        });
}

void Session::do_write_stream(
    const std::shared_ptr<StreamingState>& state)
{
    if (!state->buffer_loaded) {
        if (state->offset == state->download.content_length) {
            state->response.body().data = nullptr;
            state->response.body().size = 0;
            state->response.body().more = false;
        } else {
            const auto remaining = state->download.content_length - state->offset;
            const auto capacity = static_cast<std::size_t>(std::min<std::uint64_t>(
                remaining, state->buffer.size()));
            std::size_t produced{};
            try {
                produced = state->download.read(
                    state->offset,
                    std::span<std::byte>(state->buffer.data(), capacity));
            } catch (...) {
                return;
            }
            if (produced == 0 || produced > capacity)
                return;
            state->response.body().data = state->buffer.data();
            state->response.body().size = produced;
            state->offset += produced;
            state->response.body().more =
                state->offset < state->download.content_length;
        }
        state->buffer_loaded = true;
    }

    auto self = shared_from_this();
    http::async_write_some(socket_, state->serializer,
        [self, state](beast::error_code ec, std::size_t)
        {
            if (ec == http::error::need_buffer) {
                ec = {};
                state->buffer_loaded = false;
            }
            if (ec) return;
            if (state->serializer.is_done()) {
                self->finish_write(state->keep_alive);
                return;
            }
            self->do_write_stream(state);
        });
}

void Session::finish_write(bool keep_alive)
{
    if (keep_alive) {
        do_read();
        return;
    }
    beast::error_code ec;
    socket_.shutdown(uds::socket::shutdown_send, ec);
}

} // namespace webengine
