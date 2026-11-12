#include "Listener.hpp"
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

namespace { bool reject_permissions = false; }
extern "C" int __real_chmod(const char*, mode_t);
extern "C" int __wrap_chmod(const char* path, mode_t mode) {
    if (reject_permissions) { errno = EPERM; return -1; }
    return __real_chmod(path, mode);
}
int main() {
    using namespace webengine;
    char pattern[] = "/tmp/listener-permissions-XXXXXX";
    auto* directory = ::mkdtemp(pattern); assert(directory);
    const auto path = std::filesystem::path(directory) / "http.sock";
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{directory};
    asio::io_context io;
    TokenStore tokens;
    Router router(tokens);
    reject_permissions = true;
    bool rejected = false;
    try { Listener listener(io, uds::endpoint(path.string()), router); }
    catch (const boost::system::system_error& error) {
        rejected = error.code().value() == EPERM;
    }
    assert(rejected);
    uds::socket client(io);
    boost::system::error_code error;
    client.connect(uds::endpoint(path.string()), error);
    assert(error);
    std::filesystem::remove(path);
    reject_permissions = false;
    Listener listener(io, uds::endpoint(path.string()), router);
    struct stat info{}; assert(::stat(path.c_str(), &info) == 0);
    assert((info.st_mode & 0777) == 0600);
    client.connect(uds::endpoint(path.string()), error);
    assert(!error);
}
