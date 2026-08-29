# WebEngine




# How to run

The image ships **both** reverse proxies (nginx and lighttpd); the backend drives
exactly one per container, chosen by `WEBENGINE_WEBSERVER`. Compose defines a
service for each, on non-overlapping port ranges so they can run side-by-side:

```bash
docker compose up --build                # both: nginx :8080-8085/:8443, lighttpd :9080-9085/:9443
docker compose up --build poc            # nginx only
docker compose up --build poc-lighttpd   # lighttpd only (the lighter proxy for older platforms)
```

# How to build in local

```bash
# Install the required system libraries
sudo apt-get install -y clangd cmake git libboost-dev libssl-dev
```

```bash
# Build it (run from the backend/ directory)
cd backend
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
# produces:  build/libwebengine.a  (the library)  and  build/backend  (the example app)
```

CMake uses an existing Glaze package when one is provided by the build
environment, as in Docker or Yocto. Otherwise, local builds automatically fetch
Glaze 8.0.0 into `backend/build/_deps`; nothing is installed into `/usr/local`.

Parent projects can supply `glaze::glaze` before adding `backend/` with
`add_subdirectory()`. Set `WEBENGINE_FETCH_DEPS=OFF` to prohibit network
fallbacks and `WEBENGINE_BUILD_EXAMPLE=OFF` when embedding only the reusable
`webengine::webengine` target.



# Backend: the `webengine` library

The C++ backend is structured as a reusable library (`webengine`) plus a thin
example app. Application code only touches the public headers in
`backend/include/webengine/`; all HTTP/session/routing machinery is internal.

```cpp
#include <webengine/TestAuthProvider.hpp>
#include <webengine/WebEngine.hpp>
using namespace webengine;

int main() {
    // 1. Auth data comes from a pluggable AuthProvider. TestAuthProvider is an
    //    in-memory override seeded with test accounts; subclass AuthProvider to
    //    load real users (config file, DB, …).
    auto auth = std::make_shared<TestAuthProvider>();

    // 2. Configure the engine with a few chained calls, then run().
    WebEngine engine(auth);
    engine.set_socket_path("/tmp/backend.sock")
          .enable_auth_endpoints()                    // /api/login, /api/logout, /auth-check
          .protect_path("/protected/", Role::Admin)   // gate nginx-served static files
          .add_api(http::verb::get, "/api/public", [](const RequestContext&) {
              return json(http::status::ok, R"({"message":"public"})");
          })
          .add_api(http::verb::get, "/api/private", my_handler, Role::Admin)
          .enable_admin_endpoints();                  // Admin-only user/ACL management
    engine.run();   // blocks
}
```

See the full working example in [`backend/example/main.cpp`](backend/example/main.cpp).

### Public API (`backend/include/webengine/`)

| Header | Purpose |
| --- | --- |
| `AuthProvider.hpp` | Abstract user store: implement `authenticate()`; optionally override the user-management hooks. |
| `TestAuthProvider.hpp` | Ready-made in-memory provider (PBKDF2-hashed), seeded with the test accounts below. |
| `WebEngine.hpp` | The engine: `add_api`, bounded file-transfer routes, `set_api_role`, `protect_path`, `serve_protected_files`, built-in endpoints, socket configuration, and lifecycle. |
| `Http.hpp` | `Request`/`Response`/`RequestContext`/`Handler` types and `json()`/`text()` response builders. |
| `Role.hpp` | The `Role` enum (`Guest < Viewer < User < Admin`). |
| `WebServerController.hpp` | Abstract interface for controlling the reverse proxy (`on`/`off`/`reset`/`reload`/`set_listen_port`/`is_running`/…), plus `make_web_server_controller()` and `web_server_from_string()`. |
| `NginxController.hpp` | `WebServerController` for nginx (graceful, zero-downtime reload). |
| `LighttpdController.hpp` | `WebServerController` for lighttpd — the lighter proxy for older/embedded platforms. |

### Authenticated binary file transfer

Applications can register bounded binary upload and download routes without
teaching WebEngine what the files represent:

```cpp
engine.add_file_upload(
    "/api/v1/certificates/ca",
    [](const RequestContext&, const FileUpload& upload) {
        install_certificate(upload.file_name, upload.contents);
        return json(http::status::ok, R"({"saved":true})");
    },
    Role::Admin,
    1024 * 1024,
    {"application/octet-stream", "application/x-pem-file"});

engine.add_file_download(
    "/api/v1/certificates/ca",
    [](const RequestContext&) -> std::optional<FileDownload> {
        return FileDownload{"ca.pem", "application/x-pem-file", load_ca()};
    },
    Role::Admin);

auto generated = std::make_shared<GeneratedFile>(make_generated_file());
engine.add_streaming_download(
    "/api/v1/captures/export",
    [generated](const RequestContext&) -> std::optional<StreamingDownload> {
        return StreamingDownload{
            "capture.mncwf", "application/x-mncwf", generated->size(),
            [generated](std::uint64_t offset, std::span<std::byte> destination) {
                return generated->read(offset, destination);
            }};
    },
    Role::Admin);
```

Upload requests use the raw request body and provide the original basename in
`X-File-Name`. WebEngine authenticates the request before calling the handler,
checks the per-route body limit while reading, rejects unsafe filenames and
unsupported content types, and never interprets or writes the file. Downloads
set attachment and no-cache headers. `add_file_download` is convenient for
small in-memory objects; `add_streaming_download` repeatedly invokes a bounded
reader with monotonic offsets and sends a fixed-length response without first
assembling the object in a Beast `string_body`. Product code remains responsible
for validating the file contents and forwarding them to its owning service.

### Reverse-proxy control (`WebServerController`)

The backend **owns its reverse proxy**: it starts, stops, restarts and re-ports
it at runtime through the `WebServerController` interface, so the same code drives
either server. Pick one with the factory (or construct a controller directly):

```cpp
#include <webengine/WebServerController.hpp>
using namespace webengine;

// Choose at runtime, e.g. from the WEBENGINE_WEBSERVER env var:
WebServer kind = web_server_from_string("lighttpd").value_or(WebServer::Nginx);
std::unique_ptr<WebServerController> srv = make_web_server_controller(kind);

srv->on();                 // start it (writes the listen snippet + validates first)
srv->set_listen_port(8081);// rewrite snippet, validate, apply
srv->is_running();         // pidfile present + process alive
srv->off();                // stop it
```

The example app exposes these as Admin-only endpoints (driving whichever server
`WEBENGINE_WEBSERVER` selected):

| Endpoint | Action |
| --- | --- |
| `POST /api/admin/server/on` | start the server |
| `POST /api/admin/server/off` | stop it (needs `{"force":true}` — it's the only ingress) |
| `POST /api/admin/server/reset` | hard restart |
| `POST /api/admin/server/port` | `{"http":<port>}` live HTTP port change |
| `GET  /api/admin/server/status` | `{"server","running","http_port","https_port"}` |

How it works: the stable `nginx.conf` / `lighttpd.conf` is left untouched; only the
volatile `listen` directives live in a small generated snippet (`conf.d/listen.conf`)
that the server `include`s. `set_listen_port()` rewrites that snippet, validates it
(`nginx -t` / `lighttpd -tt`) and applies it. Both the server and the backend run as
the **same unprivileged service user**, so the backend can signal/reload the server
without root.

#### nginx vs lighttpd — behavioural differences

lighttpd targets older/embedded platforms and differs from nginx in a few ways the
controller and config account for:

| | nginx | lighttpd |
| --- | --- | --- |
| Config reload | graceful SIGHUP — **zero downtime** | no graceful reload → `reload()`/`set_listen_port()` **validate then restart** (the old instance drains the in-flight request while the new one starts, so a port change still answers the triggering request) |
| `/protected/` static files | gated via `auth_request → /auth-check`, **served by nginx** | **proxied to the backend**, which gates (same session + ACL) **and** serves them via `serve_protected_files` — lighttpd has no `auth_request` (see below) |
| Bodyless `POST` to `/api/…` | forwarded | returns **411 Length Required** — send `Content-Length: 0` (e.g. `curl -d ''`) for `…/on` and `…/reset` |
| `/api/` + `/protected/` over the Unix socket | always | needs lighttpd **≥ 1.4.46**; on an older target proxy a TCP backend instead |

**Gating `/protected/` without `auth_request` (lighttpd).** lighttpd has no native
`auth_request`-to-upstream equivalent in any version — `mod_auth` is credential-based
and `mod_magnet`/Lua has no async HTTP client (and blocking I/O stalls the server).
So instead of gating in the proxy, lighttpd **proxies `/protected/` to the backend**,
which gates *and* serves those files (`WebEngine::serve_protected_files`) using the
**same** session token + `protect_path`/ACL as `/auth-check`. Behaviour matches nginx:
unauthenticated → `302 /?reason=unauthenticated`, under-privileged → `403`, otherwise
the file (or `404`). This is also the most portable option — it needs no `auth_request`
and no Lua, so it works on genuinely old lighttpd too. Public static files stay on the
proxy; only the gated subtree is proxied to the backend.

### Access-control model (two mechanisms, by design)

* **Per-API role** — set with `add_api(..., min_role)` or `set_api_role(path, role)`,
  enforced by the router on exact `(method, path)` routes. A route with no
  `min_role` is public; otherwise the caller needs a valid session whose role
  satisfies it (else 401/403).
* **Protected-path prefix** — set with `protect_path(prefix, role)` and managed at
  runtime via `/api/admin/acl`. Enforced for `/protected/` static files either by
  `/auth-check` (nginx's `auth_request`, which then serves the file itself) or by
  `serve_protected_files` (lighttpd, where the backend gates *and* serves the file) —
  both consult the same ACL.

These are independent: `/api/admin/acl` edits the prefix ACL only — it does not
change an API route's role (use `set_api_role` for that).


# How to login

```
admin / admin123 
or 
alice / user123
```
