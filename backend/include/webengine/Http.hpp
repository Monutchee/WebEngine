#pragma once
#include <utility>               // must precede boost — Boost 1.74 awaitable.hpp uses std::exchange without including <utility>
#include <boost/beast/http.hpp>
#include <functional>
#include <optional>
#include <string>

#include "AuthProvider.hpp"      // UserInfo
#include "Role.hpp"

namespace webengine {

namespace http = boost::beast::http;

// HTTP message types handlers work with. These are Boost.Beast types aliased
// into the webengine namespace so callers needn't spell out the full paths.
using Request  = http::request<http::string_body>;
using Response = http::response<http::string_body>;

// What a handler receives for each matched request.
struct RequestContext {
    const Request& request;          // the raw incoming request
    std::optional<UserInfo> user;    // the authenticated user, set iff the
                                     // route declared a minimum role
};

// A request handler: maps a context to a response. Register one with
// WebEngine::add_api().
using Handler = std::function<Response(const RequestContext&)>;

// ── Response builders ─────────────────────────────────────────────────────────

inline Response make_response(http::status status, std::string body,
                              std::string content_type = "text/plain")
{
    Response res{status, 11};
    res.set(http::field::content_type, std::move(content_type));
    res.body() = std::move(body);
    res.prepare_payload();
    return res;
}

inline Response json(http::status status, std::string body) {
    return make_response(status, std::move(body), "application/json");
}

inline Response text(http::status status, std::string body) {
    return make_response(status, std::move(body), "text/plain");
}

// A bodiless response carrying only a status code (used for auth sub-requests).
inline Response status_response(http::status status) {
    Response res{status, 11};
    res.prepare_payload();
    return res;
}

// ── Request parsing helpers ────────────────────────────────────────────────────

// Extracts a top-level string field from a flat JSON object body. Returns "" if
// absent. Deliberately minimal — adequate for the demo's small payloads, not a
// general-purpose JSON parser.
inline std::string json_field(const std::string& json_body, const std::string& field) {
    std::string key = "\"" + field + "\"";
    auto pos = json_body.find(key);
    if (pos == std::string::npos) return {};
    pos = json_body.find(':', pos + key.size());
    if (pos == std::string::npos) return {};
    pos = json_body.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    ++pos;
    auto end = json_body.find('"', pos);
    if (end == std::string::npos) return {};
    return json_body.substr(pos, end - pos);
}

} // namespace webengine
