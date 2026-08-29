#pragma once
#include <utility>               // must precede boost — Boost 1.74 awaitable.hpp uses std::exchange without including <utility>
#include <boost/beast/http.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <variant>

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

/** Validated binary upload delivered to product code. */
struct FileUpload {
    std::string file_name;
    std::string content_type;
    std::string contents;
};

/** Binary file returned by a product download handler. */
struct FileDownload {
    std::string file_name;
    std::string content_type{"application/octet-stream"};
    std::string contents;
};

/**
 * Binary file generated or read incrementally by product code.
 *
 * WebEngine calls read() with monotonically increasing offsets and a bounded
 * destination buffer until exactly content_length bytes have been returned.
 * The callback must return at least one byte while data remains and must never
 * return more than the supplied destination span. It may retain shared state
 * in its closure; WebEngine keeps the callback alive for the whole response.
 */
struct StreamingDownload {
    std::string file_name;
    std::string content_type{"application/octet-stream"};
    std::uint64_t content_length{};
    std::function<std::size_t(std::uint64_t, std::span<std::byte>)> read;
};

using FileUploadHandler = std::function<Response(
    const RequestContext&, const FileUpload&)>;
using FileDownloadHandler = std::function<std::optional<FileDownload>(
    const RequestContext&)>;
using StreamingDownloadHandler = std::function<std::optional<StreamingDownload>(
    const RequestContext&)>;

// Internal transport result. Handler remains Response-only for source
// compatibility; the router also accepts streaming-download factories.
using HandlerResult = std::variant<Response, StreamingDownload>;

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

// JSON bodies are (de)serialised with glaze directly in the handlers
// (glz::read / glz::write_json); json() above just wraps the result in a Response.

} // namespace webengine
