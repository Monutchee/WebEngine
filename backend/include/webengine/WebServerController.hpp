#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace webengine {

// Abstract interface for controlling a local web server / reverse proxy from
// C++: start/stop/restart it, apply configuration changes, and change the
// listening port at runtime.
//
// Concrete implementations drive a specific server binary:
//   • NginxController    — nginx
//   • LighttpdController — lighttpd (lighter; for older/embedded platforms)
//
// Hold one polymorphically (e.g. via make_web_server_controller) to write code
// that does not care which server is behind it:
//
//     std::unique_ptr<WebServerController> srv =
//         make_web_server_controller(WebServer::Lighttpd);
//     srv->on();
//     srv->set_listen_port(8081);
//     if (!srv->reload()) std::cerr << srv->last_error();
//
// CONTRACT
//   All methods are synchronous. Lifecycle/config calls return true on success
//   and false on failure; on failure last_error() holds the server's own
//   diagnostic output (e.g. the config-test error). The introspection getters
//   never fail.
//
// PRIVILEGES (shared by all implementations)
//   Controlling a running server signals its master process, so the controlling
//   process must be allowed to signal it — easiest when both run as the SAME
//   user. In the deployments here the server runs as the unprivileged service
//   user on high ports (>=1024), so no root or capabilities are needed.
class WebServerController {
public:
    virtual ~WebServerController() = default;

    // ── Lifecycle ───────────────────────────────────────────────────────────
    virtual bool on()    = 0;   // start the server if it is not already running
    virtual bool off()   = 0;   // stop the server (no-op if already stopped)
    virtual bool reset() = 0;   // hard restart: off, wait for exit, on

    // Apply configuration changes (e.g. a rewritten listen snippet) with the
    // least disruption the backend supports. NOTE the guarantee is backend-
    // dependent: nginx reloads gracefully with zero downtime (SIGHUP); lighttpd
    // has no graceful config reload on older platforms, so its reload() is a
    // validate-then-restart and briefly interrupts service. See each subclass.
    virtual bool reload() = 0;

    // ── Runtime configuration ─────────────────────────────────────────────────
    // Rewrite the listen snippet and, if the server is running, validate + apply
    // it so the change takes effect immediately. If the server is stopped, the
    // new port is used at the next on(). Returns false (leaving the server on the
    // old port) if the new config fails validation.
    virtual bool set_listen_port(std::uint16_t http_port) = 0;
    virtual bool set_listen_ports(std::uint16_t http_port, std::uint16_t https_port) = 0;

    // ── Introspection ─────────────────────────────────────────────────────────
    virtual bool          is_running()  const = 0;   // pidfile present and process alive
    virtual bool          test_config() const = 0;   // server's own config validation
    virtual std::uint16_t http_port()   const = 0;
    virtual std::uint16_t https_port()  const = 0;
    virtual const std::string& last_error() const = 0;
};

// ── Factory ─────────────────────────────────────────────────────────────────

// The web servers this build can control. Keep in sync with the parser below.
enum class WebServer { Nginx, Lighttpd };

// Canonical lowercase name ("nginx" / "lighttpd"). Never returns nullptr.
const char* to_string(WebServer kind);

// Parse a server name (case-insensitive: "nginx", "lighttpd"). Returns
// std::nullopt for anything else — handy for reading a WEBENGINE_WEBSERVER env
// var and falling back to a default.
std::optional<WebServer> web_server_from_string(const std::string& name);

// Construct a controller for `kind`, configured with that server's defaults
// (binary path, config path, pidfile, listen snippet, ports). Never returns
// nullptr for a valid enum value.
std::unique_ptr<WebServerController> make_web_server_controller(WebServer kind);

// Optional filesystem path overrides for non-default deployments — e.g. the
// embedded target, where config/state live under /opt/monutchee/msys instead of
// /etc. An empty field keeps that server's built-in default. The same struct maps
// onto whichever server make_web_server_controller() builds (both NginxController
// and LighttpdController share these four fields).
struct WebServerPaths {
    std::string config;       // main config file (nginx -c / lighttpd -f)
    std::string listen_file;  // generated listen snippet (must match the config's `include`)
    std::string pidfile;      // must match the pid directive in the config
    std::string temp_root;    // parent of the server's temp dirs
};

// As make_web_server_controller(kind), but applies `paths` over the server's
// defaults (non-empty fields win). Lets the deployment relocate config/state
// without the caller knowing which concrete server it drives.
std::unique_ptr<WebServerController>
make_web_server_controller(WebServer kind, const WebServerPaths& paths);

} // namespace webengine
