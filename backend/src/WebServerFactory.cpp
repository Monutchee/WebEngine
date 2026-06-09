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
    switch (kind) {
        case WebServer::Lighttpd: return std::make_unique<LighttpdController>();
        case WebServer::Nginx:    return std::make_unique<NginxController>();
    }
    return std::make_unique<NginxController>();   // unreachable for a valid enum
}

} // namespace webengine
