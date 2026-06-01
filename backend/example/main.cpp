// Example consumer of the webengine library.
//
// This is everything an application author has to write: pick an AuthProvider,
// declare endpoints and their required roles, and run(). All the HTTP, session
// and routing machinery lives in the library.

#include <iostream>
#include <string>

#include <webengine/TestAuthProvider.hpp>
#include <webengine/WebEngine.hpp>

using namespace webengine;

int main()
{
    try {
        // 1. Auth data via a pluggable provider. TestAuthProvider injects a few
        //    in-memory test accounts (admin/admin123, alice/user123, …). Swap in
        //    your own AuthProvider subclass to load real users.
        auto auth = std::make_shared<TestAuthProvider>();

        // 2. Build and configure the engine with a few friendly calls.
        WebEngine engine(auth);

        engine.set_socket_path("/tmp/backend.sock")
              .enable_auth_endpoints()                       // /api/login, /api/logout, /auth-check
              .protect_path("/protected/", Role::Admin);     // gate static files (nginx auth_request)

        // Public API — no authentication required.
        engine.add_api(http::verb::get, "/api/public", [](const RequestContext&) {
            return json(http::status::ok,
                R"({"message":"This is public data. No authentication required."})");
        });

        // Private API — requires an authenticated Admin. The authenticated user
        // is handed to the handler via ctx.user.
        engine.add_api(http::verb::get, "/api/private", [](const RequestContext& ctx) {
            const UserInfo& u = *ctx.user;   // guaranteed present by the min_role below
            return json(http::status::ok,
                R"({"message":"private data","user":")" + u.username +
                R"(","role":")" + std::string(role_name(u.role)) + R"("})");
        }, Role::Admin);

        // The same role can be set (or changed) after registration:
        //     engine.set_api_role("/api/private", Role::User);

        // Admin-only management endpoints for users and protected paths.
        engine.enable_admin_endpoints();

        std::cout << "backend listening on /tmp/backend.sock\n";
        engine.run();   // blocks
    }
    catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
