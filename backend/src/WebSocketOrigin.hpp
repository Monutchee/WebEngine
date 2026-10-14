#pragma once
#include "webengine/Http.hpp"
#include <algorithm>
#include <cctype>
namespace webengine {
// Authority comparison includes ports. Only nginx on the owner-only UDS may
// supply forwarding headers; nginx must overwrite them, never pass client values.
inline bool websocket_same_origin(const Request& request) {
    const std::string scheme(request["X-Forwarded-Proto"]);
    if (scheme != "http" && scheme != "https") return false;
    const std::string origin(request[http::field::origin]);
    const std::string prefix = scheme + "://";
    if (!origin.starts_with(prefix)) return false;
    const auto normalize = [&](std::string authority) {
        if (authority.empty() || authority.find_first_of("/@?#\\ \t\r\n") != std::string::npos)
            return std::string{};
        std::ranges::transform(authority, authority.begin(), [](unsigned char c) { return std::tolower(c); });
        const auto suffix = scheme == "https" ? ":443" : ":80";
        if (authority.ends_with(suffix)) authority.resize(authority.size() - std::char_traits<char>::length(suffix));
        return authority;
    };
    const auto host = normalize(std::string(request[http::field::host]));
    const auto supplied = normalize(origin.substr(prefix.size()));
    return !host.empty() && host == supplied;
}
}
