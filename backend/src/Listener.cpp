#include "Listener.hpp"
#include "Session.hpp"

#include <sys/stat.h>

Listener::Listener(asio::io_context& ioc, const uds::endpoint& ep, Router& router)
    : acceptor_(ioc), router_(router)
{
    acceptor_.open(ep.protocol());
    acceptor_.bind(ep);
    // Tighten socket to owner-only; both nginx workers and the backend run as
    // www-data so uid match is sufficient.
    ::chmod(ep.path().c_str(), 0600);
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
