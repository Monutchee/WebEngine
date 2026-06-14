#pragma once
#include <string>
#include <vector>

// Process / pidfile / file plumbing shared by the web-server controllers
// (NginxController, LighttpdController). Both servers are daemons driven the
// same way — spawn a binary, track a pidfile, rewrite a config snippet — so the
// low-level mechanics live here once instead of being copy-pasted per server.
//
// Internal to the library (src/ only): not shipped in include/webengine/.
namespace webengine::proc {

// Spawn argv[0] with the given (non-NULL-terminated) argv via posix_spawnp.
//
//   capture == true : collect the child's stdout+stderr into `output`, wait for
//                     it, and return its exit code. Use for short-lived commands
//                     (config test, signalling helpers).
//   capture == false: daemonizing start — silence stdout, INHERIT stderr (so a
//                     bind failure still surfaces) and do NOT pipe; a detached
//                     master would otherwise hold a capture pipe open forever.
//                     Returns the exit code of the (quickly-exiting) parent.
//
// Returns the child exit code (>=0), or -1 on a spawn/pipe failure or abnormal
// termination. On a spawn/pipe failure `errmsg` is set; on a non-zero exit the
// captured text is in `output`.
int spawn(const std::vector<std::string>& argv, bool capture,
          std::string& output, std::string& errmsg);

// Read a numeric PID from `pidfile`. Returns the PID, or -1 if the file is
// missing/empty/non-positive.
long read_pid(const std::string& pidfile);

// True iff `pid` names a live process whose /proc/<pid>/comm contains
// `name_substr` — guards against a stale pidfile whose PID was recycled by an
// unrelated process.
bool pid_is(long pid, const std::string& name_substr);

// pidfile present + process alive + process name matches `name_substr`.
bool is_running(const std::string& pidfile, const std::string& name_substr);

// Send `sig` to `pid`. Returns true if the signal was delivered; on failure
// `errmsg` is set.
bool signal_pid(long pid, int sig, std::string& errmsg);

// Atomically write `content` to `path` (write to path + ".tmp", fsync-less
// flush, then rename) so a reader never sees a half-written file. Returns false
// and sets `errmsg` on any failure, never leaving a truncated target behind.
bool write_file_atomic(const std::string& path, const std::string& content,
                       std::string& errmsg);

// mkdir(path) one level; success if created or already present. Returns false
// on any other error.
bool ensure_dir(const std::string& path);

} // namespace webengine::proc
