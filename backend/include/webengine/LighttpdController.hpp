#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "WebServerController.hpp"

namespace webengine {

// Controls a local lighttpd instance from C++: start/stop/restart it and change
// the listening port at runtime — without rewriting the whole lighttpd.conf.
// lighttpd is the lightweight reverse proxy used on older/embedded platforms;
// this is the lighttpd implementation of WebServerController.
//
// HOW IT WORKS
//   The stable lighttpd.conf is left untouched. Only the volatile listen
//   directives live in a small snippet (Options::listen_file) that lighttpd
//   pulls in with `include`: `server.port = <http>` plus a TLS `$SERVER["socket"]`
//   block when https is enabled. set_listen_port() rewrites that snippet,
//   validates it with `lighttpd -tt -f <config>`, then applies it.
//
// RELOAD SEMANTICS (differs from nginx — important)
//   lighttpd has no graceful, zero-downtime config reload via signal on the
//   older platforms this targets (SIGHUP only re-opens logs; it does NOT re-read
//   the config). So reload() here is a *validate-then-restart*: it first runs
//   `lighttpd -tt` and only restarts if the new config is valid (a bad snippet
//   leaves the running server untouched). The restart briefly drops in-flight
//   connections — unlike nginx's seamless SIGHUP reload. set_listen_ports()
//   therefore also briefly interrupts service while lighttpd is running.
//
// CONTROL SIGNALS
//   lighttpd has no `-s quit/reload` subcommand like nginx; the master is
//   controlled by POSIX signals. off() sends SIGINT (graceful shutdown) to the
//   pid from Options::pidfile (which must match `server.pid-file` in the config).
//
// PRIVILEGES
//   off()/restart signal the lighttpd master, so the controller must be allowed
//   to signal it — easiest when both run as the SAME user (the unprivileged
//   service user here). For ports < 1024 grant the binary CAP_NET_BIND_SERVICE.
//
// All methods are synchronous. On failure they return false and last_error()
// holds lighttpd's diagnostic output (e.g. the `-tt` error).
class LighttpdController : public WebServerController {
public:
    struct Options {
        // Absolute path: avoids a $PATH search and works under systemd (whose
        // default PATH excludes /usr/sbin). lighttpd usually lives in /usr/sbin.
        std::string lighttpd_bin = "/usr/sbin/lighttpd";
        std::string config       = "/etc/lighttpd/lighttpd.conf";        // passed as -f
        std::string pidfile      = "/tmp/lighttpd.pid";                  // must match server.pid-file
        std::string listen_file  = "/etc/lighttpd/conf.d/listen.conf";   // generated snippet
        std::string temp_root    = "/tmp/lighttpd";                      // upload/temp dir ensured before start
        // Combined PEM (certificate + private key) for TLS; referenced by
        // ssl.pemfile in the generated snippet. Empty disables nothing on its
        // own — set https_enabled=false to omit the TLS socket block entirely.
        std::string ssl_pemfile  = "/etc/lighttpd/ssl/server.pem";
        std::uint16_t http_port     = 8080;
        std::uint16_t https_port    = 8443;
        bool          https_enabled = true;
    };

    LighttpdController() = default;                                       // all Options defaults
    explicit LighttpdController(Options opts) : opts_(std::move(opts)) {}

    // ── Lifecycle ───────────────────────────────────────────────────────────────
    bool on()    override;  // start lighttpd if not already running (writes snippet + validates first)
    bool off()   override;  // graceful stop (SIGINT to the master)
    bool reset() override;  // hard restart (off, wait for exit, on)
    // Validate the config and, if valid, restart to apply it. NOT zero-downtime:
    // lighttpd has no graceful config reload on the targeted platforms — see the
    // class comment. Returns false (leaving the running server untouched) if the
    // config fails validation.
    bool reload() override;

    // ── Runtime configuration ─────────────────────────────────────────────────────
    // Rewrite the listen snippet and, if lighttpd is running, validate + restart so
    // the change takes effect (brief interruption). If stopped, the new port is used
    // at the next on(). Returns false (leaving the old port serving) on validation failure.
    bool set_listen_port(std::uint16_t http_port) override;
    bool set_listen_ports(std::uint16_t http_port, std::uint16_t https_port) override;

    // ── Introspection ─────────────────────────────────────────────────────────────
    bool          is_running()  const override;   // pidfile present and process alive
    bool          test_config() const override;   // lighttpd -tt -f <config>
    std::uint16_t http_port()   const override { return opts_.http_port; }
    std::uint16_t https_port()  const override { return opts_.https_port; }

    const std::string& last_error() const override { return last_error_; }

private:
    bool ensure_dirs();
    bool write_listen_snippet();
    // Runs `lighttpd <args...>` via proc::spawn. With capture the child's output is
    // collected (config test); without capture it's the daemonizing start. Returns
    // the exit code, or -1; sets last_error_ on failure.
    int  run(const std::vector<std::string>& args, bool capture) const;

    Options             opts_;
    mutable std::string last_error_;
};

} // namespace webengine
