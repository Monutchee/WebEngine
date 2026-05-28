#pragma once
#include <utility>                             // before boost — fixes std::exchange in Boost 1.74 awaitable.hpp
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <memory>
#include "Router.hpp"

namespace beast = boost::beast;
namespace http  = beast::http;
namespace asio  = boost::asio;
using     uds   = asio::local::stream_protocol;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(uds::socket socket, Router& router);
    void run();

private:
    void do_read();
    void do_write(http::response<http::string_body> res, bool keep_alive);

    uds::socket                      socket_;
    beast::flat_buffer               buffer_;
    http::request<http::string_body> req_;
    Router&                          router_;
};
