#include "webengine/WebServerController.hpp"

#include <algorithm>
#include <cctype>
#include <memory>

#include "webengine/LighttpdController.hpp"
#include "webengine/NginxController.hpp"

namespace webengine {

const char* to_string(WebServer kind)
{
    switch (kind) {
        case WebServer::Lighttpd: return "lighttpd";
        case WebServer::Nginx:    return "nginx";
    }
    return "nginx";   // unreachable; keeps the compiler happy for a complete switch
}

std::optional<WebServer> web_server_from_string(const std::string& name)
{
    std::string s = name;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "nginx")    return WebServer::Nginx;
    if (s == "lighttpd") return WebServer::Lighttpd;
    return std::nullopt;
}

std::unique_ptr<WebServerController> make_web_server_controller(WebServer kind)
{
    return make_web_server_controller(kind, WebServerPaths{});
}

std::unique_ptr<WebServerController>
make_web_server_controller(WebServer kind, const WebServerPaths& paths)
{
    // Apply only the non-empty overrides; both controllers' Options expose the
    // same four path fields, so one generic lambda covers either.
    auto apply = [&paths](auto& o) {
        if (!paths.config.empty())      o.config      = paths.config;
        if (!paths.listen_file.empty()) o.listen_file = paths.listen_file;
        if (!paths.pidfile.empty())     o.pidfile     = paths.pidfile;
        if (!paths.temp_root.empty())   o.temp_root   = paths.temp_root;
    };
    switch (kind) {
        case WebServer::Lighttpd: {
            LighttpdController::Options o; apply(o);
            return std::make_unique<LighttpdController>(std::move(o));
        }
        case WebServer::Nginx:
        default: {
            NginxController::Options o; apply(o);
            return std::make_unique<NginxController>(std::move(o));
        }
    }
}

} // namespace webengine
