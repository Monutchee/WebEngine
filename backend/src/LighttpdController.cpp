#include "webengine/LighttpdController.hpp"

#include <csignal>    // SIGINT
#include <unistd.h>   // usleep

#include "ProcessUtil.hpp"

namespace webengine {

// ── Process helpers ───────────────────────────────────────────────────────────

int LighttpdController::run(const std::vector<std::string>& args, bool capture) const
{
    // argv = lighttpd <args...>. Unlike nginx, lighttpd's flag order varies per
    // command (`-tt -f <config>` to test, `-f <config>` to start), so callers
    // pass the full argument list rather than a fixed `-f <config>` prefix.
    std::vector<std::string> argv{opts_.lighttpd_bin};
    argv.insert(argv.end(), args.begin(), args.end());

    std::string output, err;
    int code = proc::spawn(argv, capture, output, err);
    if (code < 0) {
        last_error_ = err.empty() ? "lighttpd terminated abnormally" : err;
        return -1;
    }
    if (code != 0)
        last_error_ = output.empty()
                    ? ("lighttpd exited with code " + std::to_string(code))
                    : output;
    return code;
}

bool LighttpdController::is_running() const
{
    return proc::is_running(opts_.pidfile, "lighttpd");
}

// ── Config snippet ─────────────────────────────────────────────────────────────

bool LighttpdController::ensure_dirs()
{
    if (!proc::ensure_dir(opts_.temp_root)) {
        last_error_ = std::string("cannot create temp dir ") + opts_.temp_root;
        return false;
    }
    return true;
}

bool LighttpdController::write_listen_snippet()
{
    // lighttpd listen config: the plain HTTP port plus an optional TLS socket.
    std::string content = "server.port = " + std::to_string(opts_.http_port) + "\n";
    if (opts_.https_enabled) {
        content += "$SERVER[\"socket\"] == \"0.0.0.0:"
                 + std::to_string(opts_.https_port) + "\" {\n"
                   "    ssl.engine  = \"enable\"\n"
                   "    ssl.pemfile = \"" + opts_.ssl_pemfile + "\"\n"
                   "}\n";
    }
    return proc::write_file_atomic(opts_.listen_file, content, last_error_);
}

// ── Lifecycle ───────────────────────────────────────────────────────────────────

bool LighttpdController::test_config() const
{
    // -tt: thorough preflight (loads modules) without binding any port.
    return run({"-tt", "-f", opts_.config}, /*capture=*/true) == 0;
}

bool LighttpdController::on()
{
    if (is_running()) return true;
    if (!ensure_dirs())          return false;
    if (!write_listen_snippet()) return false;
    if (!test_config())          return false;   // last_error_ holds the -tt output

    // lighttpd daemonizes by default: the parent forks the master and exits, so a
    // 0 exit here only means "fork succeeded". Wait for the master to write its
    // pidfile and prove it is actually up (a bind failure surfaces as never-up).
    if (run({"-f", opts_.config}, /*capture=*/false) != 0) {
        last_error_ = "lighttpd failed to start (see stderr)";
        return false;
    }
    for (int i = 0; i < 30 && !is_running(); ++i)
        ::usleep(100 * 1000);
    if (!is_running()) {
        last_error_ = "lighttpd did not come up (port in use? see stderr)";
        return false;
    }
    return true;
}

bool LighttpdController::off()
{
    if (!is_running()) return true;
    // lighttpd has no `-s quit`; signal the master directly. SIGINT = graceful.
    long pid = proc::read_pid(opts_.pidfile);
    if (pid <= 0) return true;
    return proc::signal_pid(pid, SIGINT, last_error_);
}

bool LighttpdController::reset()
{
    if (is_running()) {
        if (!off()) return false;
        // Wait (up to ~5s) for the master to exit before starting a new one.
        for (int i = 0; i < 50 && is_running(); ++i)
            ::usleep(100 * 1000);
        if (is_running()) { last_error_ = "lighttpd did not stop within timeout"; return false; }
    }
    return on();
}

bool LighttpdController::reload()
{
    // No graceful reload on the targeted platforms: validate, then restart. Bail
    // before touching the running server if the new config is invalid.
    if (!is_running()) { last_error_ = "lighttpd is not running"; return false; }
    if (!test_config()) return false;
    return reset();
}

// ── Runtime configuration ─────────────────────────────────────────────────────

bool LighttpdController::set_listen_port(std::uint16_t http_port)
{
    return set_listen_ports(http_port, opts_.https_port);
}

bool LighttpdController::set_listen_ports(std::uint16_t http_port, std::uint16_t https_port)
{
    const std::uint16_t prev_http  = opts_.http_port;
    const std::uint16_t prev_https = opts_.https_port;

    opts_.http_port  = http_port;
    opts_.https_port = https_port;

    if (!write_listen_snippet()) {
        opts_.http_port = prev_http; opts_.https_port = prev_https;
        return false;
    }
    if (!is_running()) return true;   // takes effect at next on()

    if (!reload()) {
        // Roll the snippet back so a failed restart doesn't leave a bad file behind
        // that would block the next start.
        opts_.http_port = prev_http; opts_.https_port = prev_https;
        write_listen_snippet();
        return false;
    }
    return true;
}

} // namespace webengine
