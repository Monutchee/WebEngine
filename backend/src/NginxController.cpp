#include "webengine/NginxController.hpp"

#include <unistd.h>   // usleep

#include "ProcessUtil.hpp"

namespace webengine {

// ── Process helpers ───────────────────────────────────────────────────────────

int NginxController::run(const std::vector<std::string>& args, bool capture) const
{
    // argv = nginx -c <config> <args...>
    std::vector<std::string> argv{opts_.nginx_bin, "-c", opts_.config};
    argv.insert(argv.end(), args.begin(), args.end());

    std::string output, err;
    int code = proc::spawn(argv, capture, output, err);
    if (code < 0) {
        last_error_ = err.empty() ? "nginx terminated abnormally" : err;
        return -1;
    }
    if (code != 0)
        last_error_ = output.empty()
                    ? ("nginx exited with code " + std::to_string(code))
                    : output;
    return code;
}

bool NginxController::is_running() const
{
    return proc::is_running(opts_.pidfile, "nginx");
}

// ── Config snippet ─────────────────────────────────────────────────────────────

bool NginxController::ensure_dirs()
{
    bool ok = proc::ensure_dir(opts_.temp_root);
    // nginx creates the leaf temp dirs itself, but only if the parent exists.
    for (const char* leaf : {"/client_body", "/proxy", "/fastcgi", "/uwsgi", "/scgi"})
        ok = proc::ensure_dir(opts_.temp_root + leaf) && ok;
    if (!ok) last_error_ = std::string("cannot create temp dirs under ") + opts_.temp_root;
    return ok;
}

bool NginxController::write_listen_snippet()
{
    std::string content = "listen " + std::to_string(opts_.http_port) + ";\n";
    if (opts_.https_enabled)
        content += "listen " + std::to_string(opts_.https_port) + " ssl;\n";

    return proc::write_file_atomic(opts_.listen_file, content, last_error_);
}

// ── Lifecycle ───────────────────────────────────────────────────────────────────

bool NginxController::test_config() const
{
    return run({"-t"}, /*capture=*/true) == 0;
}

bool NginxController::on()
{
    if (is_running()) return true;
    if (!ensure_dirs())          return false;
    if (!write_listen_snippet()) return false;
    if (!test_config())          return false;   // last_error_ holds the -t output
    if (run({}, /*capture=*/false) != 0) {
        last_error_ = "nginx failed to start (port in use? see stderr)";
        return false;
    }
    return true;
}

bool NginxController::off()
{
    if (!is_running()) return true;
    return run({"-s", "quit"}, /*capture=*/true) == 0;
}

bool NginxController::reload()
{
    if (!is_running()) { last_error_ = "nginx is not running"; return false; }
    if (!test_config()) return false;
    return run({"-s", "reload"}, /*capture=*/true) == 0;
}

bool NginxController::reset()
{
    if (is_running()) {
        if (!off()) return false;
        // Graceful quit drains connections; wait (up to ~5s) for the master to exit.
        for (int i = 0; i < 50 && is_running(); ++i)
            ::usleep(100 * 1000);
        if (is_running()) { last_error_ = "nginx did not stop within timeout"; return false; }
    }
    return on();
}

// ── Runtime configuration ─────────────────────────────────────────────────────

bool NginxController::set_listen_port(std::uint16_t http_port)
{
    return set_listen_ports(http_port, opts_.https_port);
}

bool NginxController::set_listen_ports(std::uint16_t http_port, std::uint16_t https_port)
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
        // Roll the snippet back so a failed reload doesn't leave a bad file behind
        // that would block the next start. nginx itself kept the old config.
        opts_.http_port = prev_http; opts_.https_port = prev_https;
        write_listen_snippet();
        return false;
    }
    return true;
}

} // namespace webengine
