#pragma once
#include <utility>                             // before boost — fixes std::exchange in Boost 1.74 awaitable.hpp
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <memory>
#include "Router.hpp"

namespace webengine {

namespace beast = boost::beast;
namespace asio  = boost::asio;
using     uds   = asio::local::stream_protocol;

// One HTTP keep-alive conversation over an accepted UDS connection.
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(uds::socket socket, Router& router);
    void run();

private:
    void do_read();
    void do_write(Response res, bool keep_alive);

    uds::socket        socket_;
    beast::flat_buffer buffer_;
    Request            req_;
    Router&            router_;
};

} // namespace webengine
