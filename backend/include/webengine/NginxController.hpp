#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "WebServerController.hpp"

namespace webengine {

// Controls a local nginx instance from C++: start/stop/restart it and change the
// listening port at runtime — without rewriting the whole nginx.conf.
//
// This is the nginx implementation of the WebServerController interface; hold it
// through that interface (or make_web_server_controller) for server-agnostic code.
//
// HOW IT WORKS
//   The stable nginx.conf is left untouched. Only the volatile `listen`
//   directives live in a small snippet (Options::listen_file) that nginx pulls
//   in with `include`. set_listen_port() rewrites that snippet, validates it with
//   `nginx -t`, then applies it with a graceful `nginx -s reload` (SIGHUP):
//   nginx opens the new port and drains the old workers with no downtime — no
//   restart, no dropped master process.
//
// PRIVILEGES
//   Control (reload/quit) signals the nginx master, so the controlling process
//   must be allowed to signal it — easiest when both run as the SAME user. In the
//   Docker/embedded setup here, nginx runs as the unprivileged service user on
//   high ports (>=1024), so no root or capabilities are needed. To bind ports
//   below 1024 on the embedded target, give nginx CAP_NET_BIND_SERVICE (or run it
//   as root and run this controller as root too).
//
// All methods are synchronous. On failure they return false and last_error()
// holds nginx's diagnostic output (e.g. the `nginx -t` error).
class NginxController : public WebServerController {
public:
    struct Options {
        // Absolute path: avoids a $PATH search (no binary-hijack window, and works
        // under systemd whose default PATH excludes /usr/sbin). A name without a
        // slash would fall back to a PATH search.
        std::string nginx_bin   = "/usr/sbin/nginx";
        std::string config      = "/etc/nginx/nginx.conf";       // passed as -c
        std::string pidfile     = "/tmp/nginx.pid";              // must match `pid` in config
        std::string listen_file = "/etc/nginx/conf.d/listen.conf";// generated snippet
        std::string temp_root   = "/tmp/nginx";                  // temp dirs ensured before start
        std::uint16_t http_port    = 8080;
        std::uint16_t https_port   = 8443;
        bool          https_enabled = true;
    };

    NginxController() = default;                                    // all Options defaults
    explicit NginxController(Options opts) : opts_(std::move(opts)) {}

    // ── Lifecycle ───────────────────────────────────────────────────────────────
    bool on()    override;  // start nginx if not already running (writes snippet + validates first)
    bool off()   override;  // graceful stop  (nginx -s quit)
    bool reset() override;  // hard restart   (off, wait for exit, on)
    // Graceful, zero-downtime config reload (nginx -s reload / SIGHUP): nginx
    // opens the new config and drains the old workers without dropping the master.
    bool reload() override;

    // ── Runtime configuration ─────────────────────────────────────────────────────
    // Rewrite the listen snippet and, if nginx is running, validate + reload so the
    // change takes effect immediately. If nginx is stopped, the new port is used at
    // the next on(). Returns false (and leaves nginx serving the old port) if the
    // new config fails validation.
    bool set_listen_port(std::uint16_t http_port) override;
    bool set_listen_ports(std::uint16_t http_port, std::uint16_t https_port) override;

    // ── Introspection ─────────────────────────────────────────────────────────────
    bool          is_running()  const override;   // pidfile present and process alive
    bool          test_config() const override;   // nginx -t
    std::uint16_t http_port()   const override { return opts_.http_port; }
    std::uint16_t https_port()  const override { return opts_.https_port; }

    const std::string& last_error() const override { return last_error_; }

private:
    bool ensure_dirs();
    bool write_listen_snippet();
    // Runs `nginx -e stderr -c <config> <args...>` via proc::spawn. When capture
    // is true the child's output is collected (used for short-lived commands like -t / -s);
    // when false the child inherits stderr (the daemonizing start). Returns the
    // exit code, or -1; sets last_error_ on failure.
    int  run(const std::vector<std::string>& args, bool capture) const;

    Options             opts_;
    mutable std::string last_error_;
};

} // namespace webengine
