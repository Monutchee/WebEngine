#include <utility>                             // before boost — fixes std::exchange in Boost 1.74 awaitable.hpp
#include <boost/asio.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

#include "AclStore.hpp"
#include "TokenStore.hpp"
#include "Router.hpp"
#include "Listener.hpp"

namespace asio = boost::asio;
using     uds  = asio::local::stream_protocol;

static constexpr const char* SOCKET_PATH = "/tmp/backend.sock";

int main()
{
    ::unlink(SOCKET_PATH);

    try {
        asio::io_context ioc;

        AclStore    acl;
        TokenStore  tokens;
        Router      router(acl, tokens);

        auto listener = std::make_shared<Listener>(
            ioc, uds::endpoint{SOCKET_PATH}, router);
        listener->run();

        std::cout << "backend listening on " << SOCKET_PATH << '\n';

        const unsigned n = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> threads;
        threads.reserve(n - 1);
        for (unsigned i = 1; i < n; ++i)
            threads.emplace_back([&ioc] { ioc.run(); });
        ioc.run();
        for (auto& t : threads) t.join();
    }
    catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
