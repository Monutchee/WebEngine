#include "ProcessUtil.hpp"

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

namespace webengine::proc {

int spawn(const std::vector<std::string>& argv, bool capture,
          std::string& output, std::string& errmsg)
{
    output.clear();

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
            errmsg = std::string("pipe() failed: ") + std::strerror(errno);
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
    int rc = posix_spawnp(&pid, cargv[0], &fa, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (devnull >= 0) ::close(devnull);

    if (rc != 0) {
        if (capture) { ::close(pipefd[0]); ::close(pipefd[1]); }
        errmsg = std::string("posix_spawnp(") + argv.front()
               + ") failed: " + std::strerror(rc);
        return -1;
    }

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
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

long read_pid(const std::string& pidfile)
{
    std::ifstream f(pidfile);
    if (!f) return -1;
    long pid = -1;
    f >> pid;
    return pid > 0 ? pid : -1;
}

bool pid_is(long pid, const std::string& name_substr)
{
    // /proc/<pid>/comm holds the process name; confirm it matches so a recycled
    // PID from a stale pidfile isn't mistaken for a running server.
    std::ifstream f("/proc/" + std::to_string(pid) + "/comm");
    if (!f) return false;
    std::string name;
    std::getline(f, name);
    return name.find(name_substr) != std::string::npos;
}

bool is_running(const std::string& pidfile, const std::string& name_substr)
{
    long pid = read_pid(pidfile);
    if (pid <= 0) return false;
    // kill(pid, 0) probes existence without sending a signal:
    //   0     → alive and signalable by us
    //   EPERM → alive but owned by another user (not ours)
    //   ESRCH → no such process (stale pidfile)
    if (::kill(static_cast<pid_t>(pid), 0) != 0) return false;
    // Alive — but is it actually our server, or an unrelated process that
    // recycled the PID after the server crashed without clearing the pidfile?
    return pid_is(pid, name_substr);
}

bool signal_pid(long pid, int sig, std::string& errmsg)
{
    if (::kill(static_cast<pid_t>(pid), sig) != 0) {
        errmsg = std::string("kill(") + std::to_string(pid) + ", "
               + std::to_string(sig) + ") failed: " + std::strerror(errno);
        return false;
    }
    return true;
}

bool write_file_atomic(const std::string& path, const std::string& content,
                       std::string& errmsg)
{
    const std::string tmp = path + ".tmp";
    {
        std::ofstream o(tmp, std::ios::trunc);
        if (!o) { errmsg = "cannot write " + tmp + " (permission?)"; return false; }
        o << content;
        o.flush();
        if (!o) {   // write/flush failed (disk full, quota) — never promote a truncated file
            errmsg = "failed writing " + tmp + " (disk full?)";
            std::remove(tmp.c_str());
            return false;
        }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        errmsg = std::string("rename to ") + path + " failed: " + std::strerror(errno);
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool ensure_dir(const std::string& path)
{
    return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

} // namespace webengine::proc
