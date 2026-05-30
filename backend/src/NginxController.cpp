#include "webengine/NginxController.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace webengine {

// ── Process helpers ───────────────────────────────────────────────────────────

int NginxController::run(const std::vector<std::string>& args, bool capture) const
{
    // argv = nginx -c <config> <args...>
    std::vector<std::string> argv{opts_.nginx_bin, "-c", opts_.config};
    argv.insert(argv.end(), args.begin(), args.end());

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (auto& s : argv) cargv.push_back(const_cast<char*>(s.c_str()));
    cargv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);

    int pipefd[2] = {-1, -1};
    int devnull   = -1;

    if (capture) {
        if (pipe(pipefd) != 0) {
            posix_spawn_file_actions_destroy(&fa);
            last_error_ = std::string("pipe() failed: ") + std::strerror(errno);
            return -1;
        }
        posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&fa, pipefd[0]);
        posix_spawn_file_actions_addclose(&fa, pipefd[1]);
    } else {
        // Daemonizing start: silence stdout, inherit stderr so genuine startup
        // errors (e.g. bind failures) still surface, but don't pipe — the
        // detached master would keep the pipe open and block our reader.
        devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            posix_spawn_file_actions_adddup2(&fa, devnull, STDOUT_FILENO);
            posix_spawn_file_actions_addclose(&fa, devnull);
        }
    }

    pid_t pid = -1;
    int rc = posix_spawnp(&pid, opts_.nginx_bin.c_str(), &fa, nullptr,
                          cargv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (devnull >= 0) ::close(devnull);

    if (rc != 0) {
        if (capture) { ::close(pipefd[0]); ::close(pipefd[1]); }
        last_error_ = std::string("posix_spawnp(") + opts_.nginx_bin
                    + ") failed: " + std::strerror(rc);
        return -1;
    }

    std::string output;
    if (capture) {
        ::close(pipefd[1]);               // parent keeps only the read end
        char buf[512];
        ssize_t n;
        while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0)
            output.append(buf, static_cast<size_t>(n));
        ::close(pipefd[0]);
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    if (exit_code != 0)
        last_error_ = output.empty()
                    ? ("nginx exited with code " + std::to_string(exit_code))
                    : output;
    return exit_code;
}

long NginxController::read_pid() const
{
    std::ifstream f(opts_.pidfile);
    if (!f) return -1;
    long pid = -1;
    f >> pid;
    return pid > 0 ? pid : -1;
}

bool NginxController::pid_is_nginx(long pid)
{
    // /proc/<pid>/comm holds the process name; confirm it's actually nginx so a
    // recycled PID from a stale pidfile isn't mistaken for a running server.
    std::ifstream f("/proc/" + std::to_string(pid) + "/comm");
    if (!f) return false;
    std::string name;
    std::getline(f, name);
    return name.find("nginx") != std::string::npos;
}

bool NginxController::is_running() const
{
    long pid = read_pid();
    if (pid <= 0) return false;
    // kill(pid, 0) probes existence without sending a signal:
    //   0     → alive and signalable by us
    //   EPERM → alive but owned by another user (not our nginx)
    //   ESRCH → no such process (stale pidfile)
    if (::kill(static_cast<pid_t>(pid), 0) != 0) return false;
    // Alive — but is it actually nginx, or an unrelated process that recycled
    // the PID after nginx crashed without clearing the pidfile?
    return pid_is_nginx(pid);
}

// ── Config snippet ─────────────────────────────────────────────────────────────

bool NginxController::ensure_dirs()
{
    auto mk = [](const std::string& d) {
        if (::mkdir(d.c_str(), 0755) != 0 && errno != EEXIST) return false;
        return true;
    };
    bool ok = mk(opts_.temp_root);
    // nginx creates the leaf temp dirs itself, but only if the parent exists.
    for (const char* leaf : {"/client_body", "/proxy", "/fastcgi", "/uwsgi", "/scgi"})
        ok = mk(opts_.temp_root + leaf) && ok;
    if (!ok) last_error_ = std::string("cannot create temp dirs under ") + opts_.temp_root;
    return ok;
}

bool NginxController::write_listen_snippet()
{
    std::string content = "listen " + std::to_string(opts_.http_port) + ";\n";
    if (opts_.https_enabled)
        content += "listen " + std::to_string(opts_.https_port) + " ssl;\n";

    // Write to a temp file then rename, so nginx never sees a half-written snippet.
    const std::string tmp = opts_.listen_file + ".tmp";
    {
        std::ofstream o(tmp, std::ios::trunc);
        if (!o) { last_error_ = "cannot write " + tmp + " (permission?)"; return false; }
        o << content;
        o.flush();
        if (!o) {   // write/flush failed (disk full, quota) — never promote a truncated file
            last_error_ = "failed writing " + tmp + " (disk full?)";
            std::remove(tmp.c_str());
            return false;
        }
    }
    if (std::rename(tmp.c_str(), opts_.listen_file.c_str()) != 0) {
        last_error_ = std::string("rename to ") + opts_.listen_file + " failed: "
                    + std::strerror(errno);
        std::remove(tmp.c_str());
        return false;
    }
    return true;
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
