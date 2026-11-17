#include "Listener.hpp"
#include "Session.hpp"

#include <sys/stat.h>
#include <cerrno>
#include <boost/system/system_error.hpp>

namespace webengine {

Listener::Listener(asio::io_context& ioc, const uds::endpoint& ep, Router& router)
    : acceptor_(ioc), router_(router)
{
    acceptor_.open(ep.protocol());
    acceptor_.bind(ep);
    // Restrict access before accepting connections. Permission failures must
    // never leave a usable listener with the bind-time mode.
    if (::chmod(ep.path().c_str(), 0600) != 0)
        throw boost::system::system_error(errno, boost::system::generic_category(),
                                         "restrict Unix listener socket permissions");
    acceptor_.listen(asio::socket_base::max_listen_connections);
}

void Listener::run() { do_accept(); }

void Listener::do_accept()
{
    acceptor_.async_accept(
        [self = shared_from_this()](boost::system::error_code ec, uds::socket socket)
        {
            if (!ec)
                std::make_shared<Session>(std::move(socket), self->router_)->run();
            self->do_accept();
        });
}

} // namespace webengine
