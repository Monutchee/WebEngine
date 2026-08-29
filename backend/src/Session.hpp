#pragma once
#include <utility>                             // before boost — fixes std::exchange in Boost 1.74 awaitable.hpp
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
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
    struct StreamingState;

    void do_read();
    void do_read_body();
    void do_write(Response res, bool keep_alive);
    void do_write(StreamingDownload download, unsigned version,
                  bool keep_alive);
    void do_write_stream(const std::shared_ptr<StreamingState>& state);
    void finish_write(bool keep_alive);

    uds::socket        socket_;
    beast::flat_buffer buffer_;
    std::optional<http::request_parser<http::string_body>> parser_;
    Router&            router_;
};

} // namespace webengine
