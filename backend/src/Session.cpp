#include "Session.hpp"

namespace webengine {

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
            auto res = self->router_.dispatch(request);
            res.keep_alive(keep_alive);
            res.prepare_payload();
            self->do_write(std::move(res), keep_alive);
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
            if (keep_alive)
                self->do_read();
            else {
                beast::error_code ec2;
                self->socket_.shutdown(uds::socket::shutdown_send, ec2);
            }
        });
}

} // namespace webengine
