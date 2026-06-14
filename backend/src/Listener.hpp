#pragma once
#include <utility>                             // before boost — fixes std::exchange in Boost 1.74 awaitable.hpp
#include <boost/asio.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <memory>
#include "Router.hpp"

namespace webengine {

namespace asio = boost::asio;
using     uds  = asio::local::stream_protocol;

// Accepts UDS connections and spawns a Session for each.
class Listener : public std::enable_shared_from_this<Listener> {
public:
    Listener(asio::io_context& ioc, const uds::endpoint& ep, Router& router);
    void run();

private:
    void do_accept();

    uds::acceptor acceptor_;
    Router&       router_;
};

} // namespace webengine
