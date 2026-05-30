#include "Session.hpp"

namespace webengine {

Session::Session(uds::socket socket, Router& router)
    : socket_(std::move(socket)), router_(router) {}

void Session::run() { do_read(); }

void Session::do_read()
{
    req_ = {};
    auto self = shared_from_this();
    http::async_read(socket_, buffer_, req_,
        [self](beast::error_code ec, std::size_t)
        {
            if (ec == http::error::end_of_stream) {
                beast::error_code ec2;
                self->socket_.shutdown(uds::socket::shutdown_send, ec2);
                return;
            }
            if (ec) return;

            bool keep_alive = self->req_.keep_alive();
            auto res = self->router_.dispatch(self->req_);
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
