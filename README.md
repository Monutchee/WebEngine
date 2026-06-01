# Nginx-front-back-end-interaction-demo




# How to run
```bash
docker compose up --build
```

# How to build in local

```bash
# Install the required library
sudo apt-get install -y clangd libboost-dev libssl-dev
```

```bash
# Build it (run from the backend/ directory)
cd backend
cmake -B build && cmake --build build --config Release
# produces:  build/libwebengine.a  (the library)  and  build/backend  (the example app)
```



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
| `WebEngine.hpp` | The engine: `add_api`, `set_api_role`, `protect_path`, `enable_auth_endpoints`, `enable_admin_endpoints`, `set_socket_path`, `run`/`stop`. |
| `Http.hpp` | `Request`/`Response`/`RequestContext`/`Handler` types and `json()`/`text()` response builders. |
| `Role.hpp` | The `Role` enum (`Guest < Viewer < User < Admin`). |

### Access-control model (two mechanisms, by design)

* **Per-API role** — set with `add_api(..., min_role)` or `set_api_role(path, role)`,
  enforced by the router on exact `(method, path)` routes. A route with no
  `min_role` is public; otherwise the caller needs a valid session whose role
  satisfies it (else 401/403).
* **Protected-path prefix** — set with `protect_path(prefix, role)` and managed at
  runtime via `/api/admin/acl`; enforced by `/auth-check` for the static files
  nginx fronts under `/protected/`.

These are independent: `/api/admin/acl` edits the prefix ACL only — it does not
change an API route's role (use `set_api_role` for that).


# How to login

```
admin / admin123 
or 
alice / user123
```