#include <utility> // before Boost for older awaitable.hpp

#include <boost/asio.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "webengine/AuthProvider.hpp"
#include "webengine/Http.hpp"
#include "webengine/WebEngine.hpp"

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using uds = asio::local::stream_protocol;

class NullAuthProvider final : public webengine::AuthProvider {
public:
    std::optional<webengine::Role> authenticate(
        const std::string&, const std::string&) override
    {
        return std::nullopt;
    }
};

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct ReadTrace {
    std::vector<std::uint64_t> offsets;
    std::size_t largest_destination{};
};

} // namespace

int main()
{
    const auto socket_path = "/tmp/webengine-streaming-" +
        std::to_string(static_cast<long long>(::getpid())) + ".sock";
    std::filesystem::remove(socket_path);

    auto source = std::make_shared<std::string>(200'123, '\0');
    for (std::size_t index = 0; index < source->size(); ++index)
        (*source)[index] = static_cast<char>((index * 37u + 11u) & 0xffu);
    auto trace = std::make_shared<ReadTrace>();

    webengine::WebEngine engine(std::make_shared<NullAuthProvider>());
    engine.set_socket_path(socket_path)
        .set_threads(1)
        .add_streaming_download(
            "/capture",
            [source, trace](const webengine::RequestContext&)
                -> webengine::HandlerResult {
                return webengine::StreamingDownload{
                    "capture.mncwf",
                    "application/x-mncwf",
                    source->size(),
                    [source, trace](std::uint64_t offset,
                                    std::span<std::byte> destination) {
                        trace->offsets.push_back(offset);
                        trace->largest_destination = std::max(
                            trace->largest_destination, destination.size());
                        const auto available = source->size() -
                            static_cast<std::size_t>(offset);
                        const auto count = std::min(
                            available, destination.size());
                        std::memcpy(destination.data(),
                            source->data() + offset, count);
                        return count;
                    }};
            },
            std::nullopt);

    std::exception_ptr server_error;
    std::thread server([&] {
        try {
            engine.run();
        } catch (...) {
            server_error = std::current_exception();
        }
    });

    auto cleanup = [&] {
        engine.stop();
        if (server.joinable())
            server.join();
        std::filesystem::remove(socket_path);
    };

    try {
        asio::io_context ioc;
        uds::socket socket(ioc);
        bool connected = false;
        for (unsigned attempt = 0; attempt < 200 && !connected; ++attempt) {
            beast::error_code ec;
            uds::socket candidate(ioc);
            candidate.connect(uds::endpoint(socket_path), ec);
            if (!ec) {
                socket = std::move(candidate);
                connected = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(connected, "could not connect to WebEngine test socket");

        http::request<http::empty_body> request{
            http::verb::get, "/capture", 11};
        request.set(http::field::host, "localhost");
        request.keep_alive(false);
        http::write(socket, request);

        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        http::read(socket, buffer, response);

        require(response.result() == http::status::ok,
            "streaming response did not return 200");
        require(response[http::field::content_type] ==
                "application/x-mncwf",
            "streaming response content type changed");
        require(response[http::field::content_disposition] ==
                "attachment; filename=\"capture.mncwf\"",
            "streaming response disposition changed");
        require(response[http::field::cache_control] == "no-store",
            "streaming response cache policy changed");
        require(response.body() == *source,
            "streaming response bytes changed");
        require(trace->offsets.size() >= 4,
            "large response was not split into bounded reads");
        require(trace->largest_destination <= 64u * 1024u,
            "streaming reader received an oversized destination");
        for (std::size_t index = 1; index < trace->offsets.size(); ++index)
            require(trace->offsets[index] > trace->offsets[index - 1],
                "streaming reader offsets were not monotonic");

        cleanup();
        if (server_error)
            std::rethrow_exception(server_error);
    } catch (...) {
        cleanup();
        throw;
    }
}
