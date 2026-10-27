#include "webengine/NginxController.hpp"

#include <fstream>
#include <filesystem>
#include <iterator>
#include <stdexcept>
#include <unistd.h>   // usleep

#include "ProcessUtil.hpp"

namespace webengine {

// ── Process helpers ───────────────────────────────────────────────────────────

int NginxController::run(const std::vector<std::string>& args, bool capture) const
{
    // Route even pre-configuration diagnostics to the inherited stderr. This
    // avoids nginx trying its compiled-in /var/log/nginx/error.log path first.
    std::vector<std::string> argv{
        opts_.nginx_bin, "-e", "stderr", "-c", opts_.config
    };
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
    std::lock_guard lock(lifecycle_mutex_);
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
    content += "set $https_redirect_port " + std::to_string(opts_.https_port) + ";\n";
    if (opts_.https_enabled)
        content += "listen " + std::to_string(opts_.https_port) + " ssl;\n";

    return proc::write_file_atomic(opts_.listen_file, content, last_error_);
}

// ── Lifecycle ───────────────────────────────────────────────────────────────────

bool NginxController::test_config() const
{
    std::lock_guard lock(lifecycle_mutex_);
    return run({"-t"}, /*capture=*/true) == 0;
}

bool NginxController::on()
{
    std::lock_guard lock(lifecycle_mutex_);
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
    std::lock_guard lock(lifecycle_mutex_);
    if (!is_running()) return true;
    return run({"-s", "quit"}, /*capture=*/true) == 0;
}

bool NginxController::reload()
{
    std::lock_guard lock(lifecycle_mutex_);
    if (!is_running()) { last_error_ = "nginx is not running"; return false; }
    if (!test_config()) return false;
    return run({"-s", "reload"}, /*capture=*/true) == 0;
}

bool NginxController::reset()
{
    std::lock_guard lock(lifecycle_mutex_);
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
    std::lock_guard lock(lifecycle_mutex_);
    return set_listen_ports(http_port, opts_.https_port);
}

bool NginxController::set_listen_ports(std::uint16_t http_port, std::uint16_t https_port)
{
    std::lock_guard lock(lifecycle_mutex_);
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

namespace {
std::string quoted_directive_value(const std::string& value) {
    if (value.empty()) throw std::invalid_argument("empty TLS identity value");
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c < 32 || c == 127 || c == '$') throw std::invalid_argument("invalid TLS identity value");
        if (c == '\\' || c == '\"') result += '\\';
        result += static_cast<char>(c);
    }
    return result + "\"";
}
}
bool NginxController::set_tls_identity(const TlsIdentity& identity, const std::function<bool()>& verify)
{
    std::lock_guard lock(lifecycle_mutex_);
    if (opts_.tls_file.empty()) { last_error_ = "TLS snippet path is not configured"; return false; }
    std::string content;
    try {
        const auto certificate = quoted_directive_value(identity.certificate);
        const auto key = quoted_directive_value((identity.store_key ? "store:" : "") + identity.private_key);
        if (identity.store_key && opts_.defer_store_keys) {
            // A constant map is available before the HTTP rewrite phase and
            // cannot be influenced by the client. A variable key also defers
            // certificate loading to nginx's worker certificate callback.
            content = "map \"\" $webengine_tls_private_key {\n    default " + key + ";\n}\n"
                "ssl_certificate " + certificate + ";\n"
                "ssl_certificate_key $webengine_tls_private_key;\n"
                "ssl_certificate_cache max=2 inactive=10m valid=10m;\n";
        } else {
            content = "ssl_certificate " + certificate + ";\nssl_certificate_key " + key + ";\n";
        }
    } catch (const std::invalid_argument& error) { last_error_ = error.what(); return false; }
    std::error_code error;
    const bool existed = std::filesystem::exists(opts_.tls_file, error);
    if (error) { last_error_ = "cannot inspect previous TLS configuration"; return false; }
    if (!existed && is_running()) {
        last_error_ = "running proxy has no previous TLS snippet; restore configuration before activation";
        return false;
    }
    std::string previous;
    if (existed) {
        std::ifstream file(opts_.tls_file, std::ios::binary);
        if (!file) { last_error_ = "cannot read previous TLS configuration"; return false; }
        previous.assign(std::istreambuf_iterator<char>(file), {});
        if (file.bad()) { last_error_ = "cannot read previous TLS configuration"; return false; }
    }
    if (!ensure_dirs() || !write_listen_snippet() || !proc::write_file_atomic(opts_.tls_file, content, last_error_)) return false;
    bool accepted = false;
    last_error_.clear();
    const bool running = is_running();
    try { accepted = test_config() && (!running || (reload() && (!verify || verify()))); }
    catch (...) { accepted = false; last_error_ = "TLS verification failed"; }
    if (accepted) return true;
    if (last_error_.empty()) last_error_ = "TLS verification failed";
    const auto diagnostic = last_error_;
    bool restored = existed ? proc::write_file_atomic(opts_.tls_file, previous, last_error_) : std::filesystem::remove(opts_.tls_file, error);
    if (running && existed) restored = reload() && restored;
    last_error_ = restored ? diagnostic : "TLS activation and rollback failed";
    return false;
}

} // namespace webengine
