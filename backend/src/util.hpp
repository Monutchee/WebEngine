#pragma once
#include <utility>               // must precede boost — Boost 1.74 awaitable.hpp uses std::exchange without including <utility>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <string>

namespace beast = boost::beast;
namespace http  = beast::http;

// Boost 1.74 uses boost::beast::string_view (boost::basic_string_view<char>),
// not std::string_view. Use beast::string_view throughout to stay compatible.

inline http::response<http::string_body>
make_response(http::status status, std::string body,
              beast::string_view content_type = "text/plain")
{
    http::response<http::string_body> res{status, 11};
    res.set(http::field::content_type, content_type);
    res.body() = std::move(body);
    res.prepare_payload();
    return res;
}

inline http::response<http::string_body>
make_json_response(http::status status, std::string body) {
    return make_response(status, std::move(body), "application/json");
}

inline http::response<http::string_body>
make_status_response(http::status status) {
    http::response<http::string_body> res{status, 11};
    res.prepare_payload();
    return res;
}

inline std::string extract_json_field(const std::string& json,
                                       const std::string& field)
{
    std::string key = "\"" + field + "\"";
    auto pos = json.find(key);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + key.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    ++pos;
    auto end = json.find('"', pos);
    if (end == std::string::npos) return {};
    return json.substr(pos, end - pos);
}

inline std::string extract_cookie(beast::string_view cookie_header,
                                   beast::string_view name)
{
    std::string search = std::string(name.data(), name.size()) + "=";
    auto pos = cookie_header.find(search);
    if (pos == beast::string_view::npos) return {};
    pos += search.size();
    auto end = cookie_header.find(';', pos);
    beast::string_view val = (end == beast::string_view::npos)
        ? cookie_header.substr(pos)
        : cookie_header.substr(pos, end - pos);
    while (!val.empty() && val.front() == ' ') val.remove_prefix(1);
    while (!val.empty() && val.back()  == ' ') val.remove_suffix(1);
    return std::string(val.data(), val.size());
}
