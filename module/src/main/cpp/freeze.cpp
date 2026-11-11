//
// Zygisk-Il2CppDumper v2 - process freeze barrier.
//
// How it works
// ------------
// SIGSTOP cannot be used: on Linux a job-control stop is a *group* stop, so
// sending SIGSTOP to one thread stops the whole process, including the thread
// that is supposed to write the dump.
//
// Instead we install a handler for an unused real time signal. Every thread
// except the dumper receives that signal. The handler increments a counter and
// then blocks in a private futex. Once all threads are parked, the dumper walks
// the il2cpp metadata while the rest of the process is frozen, then releases
// the futex and every thread resumes exactly where it was interrupted.
//
// The handler stays installed (disarmed) after the first use. If a thread had
// blocked the signal during the freeze, the signal would remain pending; with
// the handler still installed it becomes a harmless no-op instead of the
// default action (terminate) once it is unblocked.
//
// Deadlock safety
// ---------------
// Once the barrier is armed, a parked thread may hold the libc allocator lock
// or any application lock. The dumper thread must therefore not call malloc
// (or any wrapper that may). Everything in this file that runs while armed
// uses only raw syscalls, stack/static buffers and lock-free atomics. Logging
// and the summary are emitted *after* the threads are released.
//

#include "freeze.h"

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <android/log.h>
#include <vector>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "log.h"

#ifndef FUTEX_WAIT_PRIVATE
#define FUTEX_WAIT_PRIVATE (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#endif

#ifndef FUTEX_WAKE_PRIVATE
#define FUTEX_WAKE_PRIVATE (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)
#endif

namespace freezer {
namespace {

// Generous hard limit for the number of threads that can be tracked without
// allocating. Tracked in static storage so the dumper thread stack stays small.
constexpr int kMaxThreads = 4096;

// Shared with the signal handler. Only atomic builtins and raw syscalls are
// used in the handler, so it is async-signal-safe.
int g_armed = 0;
int g_release = 0;
int g_parked = 0;
int g_resumed = 0;
int g_signal = 0;
bool g_handler_installed = false;

// Filled by freeze_process() and reported by thaw_process() once threads are
// running again (logging may allocate).
int g_last_signal = 0;
int g_last_parked = 0;
int g_last_expected = 0;
int g_last_skipped = 0;
int g_last_failed = 0;
int g_last_attempted = 0;
int g_last_valid = 0;

Control g_local_control{};
Control *g_control = nullptr;
bool g_native_bridge = false;
const char *g_last_method = "none";

// Watchdog: 0 idle, 1 counting, 2 done.
int g_watch_state = 0;
int g_watch_tid = 0;
int g_watch_fired = 0;
pthread_t g_watch_thread{};
bool g_watch_running = false;

pid_t current_tid() {
    return static_cast<pid_t>(syscall(SYS_gettid));
}

long long monotonic_ms() {
    struct timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return static_cast<long long>(ts.tv_sec) * 1000 +
           static_cast<long long>(ts.tv_nsec) / 1000000;
}

void sleep_ms(long ms) {
    if (ms <= 0) {
        return;
    }
    struct timespec ts{};
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
        // Restart with the remaining time.
    }
}

int open_task_dir() {
    return static_cast<int>(syscall(SYS_openat, AT_FDCWD, "/proc/self/task",
                                    O_RDONLY | O_DIRECTORY | O_CLOEXEC));
}

// Parse a decimal TID (the /proc/self/task entry names are pure digits).
bool parse_tid(const char *name, pid_t *out) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    int value = 0;
    for (const char *p = name; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        int digit = *p - '0';
        if (value > (INT_MAX - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    if (value <= 0) {
        return false;
    }
    *out = static_cast<pid_t>(value);
    return true;
}

// Rewind and enumerate /proc/self/task using getdents64 only. Returns the
// number of entries written to `out`, or -1 on error.
int read_thread_ids(int dirfd, pid_t *out, int max) {
    if (dirfd < 0) {
        return -1;
    }
    if (syscall(SYS_lseek, dirfd, 0, SEEK_SET) < 0) {
        return -1;
    }

    char buffer[4096];
    int count = 0;
    for (;;) {
        long nread = syscall(SYS_getdents64, dirfd, buffer, sizeof(buffer));
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (nread == 0) {
            break;
        }

        long offset = 0;
        while (offset < nread) {
            auto *entry = reinterpret_cast<struct dirent *>(buffer + offset);
            offset += entry->d_reclen;
            if (entry->d_name[0] == '.') {
                continue;
            }
            pid_t tid = 0;
            if (parse_tid(entry->d_name, &tid) && count < max) {
                out[count++] = tid;
            }
        }
    }
    return count;
}

// Build "/proc/self/task/<tid>/status" without printf.
bool make_status_path(char *buffer, size_t capacity, pid_t tid) {
    static constexpr char prefix[] = "/proc/self/task/";
    static constexpr char suffix[] = "/status";

    size_t pos = 0;
    for (const char *p = prefix; *p != '\0'; ++p) {
        if (pos + 1 >= capacity) {
            return false;
        }
        buffer[pos++] = *p;
    }

    unsigned int value = static_cast<unsigned int>(tid);
    char digits[16];
    int digit_count = 0;
    while (value > 0 && digit_count < static_cast<int>(sizeof(digits))) {
        digits[digit_count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    while (digit_count > 0) {
        if (pos + 1 >= capacity) {
            return false;
        }
        buffer[pos++] = digits[--digit_count];
    }

    for (const char *p = suffix; *p != '\0'; ++p) {
        if (pos + 1 >= capacity) {
            return false;
        }
        buffer[pos++] = *p;
    }
    buffer[pos] = '\0';
    return true;
}

// Reads SigBlk from /proc/self/task/<tid>/status with raw file syscalls.
bool read_blocked_signals(pid_t tid, unsigned long long *mask) {
    char path[64];
    if (!make_status_path(path, sizeof(path), tid)) {
        return false;
    }

    int fd = static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, O_RDONLY | O_CLOEXEC));
    if (fd < 0) {
        return false;
    }

    // /proc/<tid>/status is ~1.5 KB and SigBlk sits well past the first
    // 512 bytes (the Groups line alone can be long on Android).
    char buffer[4096];
    long total = 0;
    for (;;) {
        long nread = syscall(SYS_read, fd, buffer + total, sizeof(buffer) - 1 - total);
        if (nread < 0 && errno == EINTR) {
            continue;
        }
        if (nread <= 0) {
            break;
        }
        total += nread;
        if (total >= static_cast<long>(sizeof(buffer)) - 1) {
            break;
        }
    }
    syscall(SYS_close, fd);
    if (total <= 0) {
        return false;
    }
    buffer[total] = '\0';

    const char *line = strstr(buffer, "SigBlk:");
    if (line == nullptr) {
        return false;
    }
    const char *p = line + 7;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }

    unsigned long long value = 0;
    bool any = false;
    for (;; ++p) {
        int digit;
        if (*p >= '0' && *p <= '9') {
            digit = *p - '0';
        } else if (*p >= 'a' && *p <= 'f') {
            digit = *p - 'a' + 10;
        } else if (*p >= 'A' && *p <= 'F') {
            digit = *p - 'A' + 10;
        } else {
            break;
        }
        value = (value << 4) | static_cast<unsigned long long>(digit);
        any = true;
    }

    if (!any) {
        return false;
    }
    *mask = value;
    return true;
}

bool signal_is_blocked(pid_t tid, int sig) {
    unsigned long long mask = 0;
    if (!read_blocked_signals(tid, &mask)) {
        // If we cannot read the mask, try to deliver the signal anyway.
        return false;
    }
    return ((mask >> (sig - 1)) & 1ULL) != 0;
}

bool contains(const pid_t *ids, int count, pid_t tid) {
    for (int i = 0; i < count; ++i) {
        if (ids[i] == tid) {
            return true;
        }
    }
    return false;
}

// SIG_IGN is a valid candidate, but SIG_DFL is preferred because an ignored
// signal may be used by the application for another purpose. A non default
// handler means the signal already belongs to the application, so we skip it.
int signal_preference(int sig) {
    struct sigaction current{};
    if (sigaction(sig, nullptr, &current) != 0) {
        return -1;
    }
    if (current.sa_handler == SIG_DFL) {
        return 0;
    }
    if (current.sa_handler == SIG_IGN) {
        return 1;
    }
    return -1;
}

extern "C" void freezer_signal_handler(int) {
    if (!__atomic_load_n(&g_armed, __ATOMIC_ACQUIRE)) {
        return;
    }

    __atomic_add_fetch(&g_parked, 1, __ATOMIC_ACQ_REL);
    while (!__atomic_load_n(&g_release, __ATOMIC_ACQUIRE)) {
        syscall(SYS_futex, &g_release, FUTEX_WAIT_PRIVATE, 0, nullptr, nullptr, 0);
    }
    __atomic_add_fetch(&g_resumed, 1, __ATOMIC_ACQ_REL);
}

void release_parked_threads() {
    __atomic_store_n(&g_release, 1, __ATOMIC_RELEASE);
    syscall(SYS_futex, &g_release, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
}

void futex_wake_all(int *addr) {
    syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
}

// Runs while the process is frozen: raw syscalls and atomics only.
void *watchdog_main(void *) {
    __atomic_store_n(&g_watch_tid, static_cast<int>(current_tid()), __ATOMIC_RELEASE);
    futex_wake_all(&g_watch_tid);

    int state;
    while ((state = __atomic_load_n(&g_watch_state, __ATOMIC_ACQUIRE)) == 0) {
        syscall(SYS_futex, &g_watch_state, FUTEX_WAIT_PRIVATE, 0, nullptr, nullptr, 0);
    }
    if (state != 1) {
        return nullptr;
    }

    const long long deadline = monotonic_ms() + DUMPER_FREEZE_MAX_MS;
    while (__atomic_load_n(&g_watch_state, __ATOMIC_ACQUIRE) == 1) {
        const long long left = deadline - monotonic_ms();
        if (left <= 0) {
            __atomic_store_n(&g_watch_fired, 1, __ATOMIC_RELEASE);
            release_parked_threads();
            break;
        }
        struct timespec ts{};
        ts.tv_sec = static_cast<time_t>(left / 1000);
        ts.tv_nsec = static_cast<long>((left % 1000) * 1000000L);
        syscall(SYS_futex, &g_watch_state, FUTEX_WAIT_PRIVATE, 1, &ts, nullptr, 0);
    }
    return nullptr;
}

bool start_watchdog() {
    __atomic_store_n(&g_watch_state, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_watch_tid, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_watch_fired, 0, __ATOMIC_RELEASE);
    if (pthread_create(&g_watch_thread, nullptr, watchdog_main, nullptr) != 0) {
        return false;
    }
    g_watch_running = true;
    const long long deadline = monotonic_ms() + 1000;
    while (__atomic_load_n(&g_watch_tid, __ATOMIC_ACQUIRE) == 0 && monotonic_ms() < deadline) {
        struct timespec ts{0, 10 * 1000000L};
        syscall(SYS_futex, &g_watch_tid, FUTEX_WAIT_PRIVATE, 0, &ts, nullptr, 0);
    }
    return __atomic_load_n(&g_watch_tid, __ATOMIC_ACQUIRE) != 0;
}

void arm_watchdog() {
    __atomic_store_n(&g_watch_state, 1, __ATOMIC_RELEASE);
    futex_wake_all(&g_watch_state);
}

// Tells the watchdog the dump is over. Safe while frozen (no join, no log).
void finish_watchdog() {
    __atomic_store_n(&g_watch_state, 2, __ATOMIC_RELEASE);
    futex_wake_all(&g_watch_state);
}

void join_watchdog() {
    if (g_watch_running) {
        finish_watchdog();
        pthread_join(g_watch_thread, nullptr);
        g_watch_running = false;
    }
}

bool install_handler() {
    if (g_handler_installed) {
        return g_signal != 0;
    }

    const int first = SIGRTMIN;
    const int last = SIGRTMAX;
    const int count = last - first + 1;
    int start = first + DUMPER_FREEZE_SIGNAL_OFFSET;
    if (start > last) {
        start = first;
    }

    // This runs before the barrier is armed, so allocation is fine here.
    static pid_t tids[kMaxThreads];
    int tid_count = 0;
    int dirfd = open_task_dir();
    if (dirfd >= 0) {
        int n = read_thread_ids(dirfd, tids, kMaxThreads);
        if (n > 0) {
            tid_count = n;
        }
        syscall(SYS_close, dirfd);
    }

    const pid_t self = current_tid();
    int best_signal = 0;
    int best_score = INT_MAX;
    int best_blocked = INT_MAX;

    for (int i = 0; i < count; ++i) {
        int sig = first + (start - first + i) % count;
        int preference = signal_preference(sig);
        if (preference < 0) {
            continue;
        }

        int blocked_count = 0;
        for (int t = 0; t < tid_count; ++t) {
            if (tids[t] == self) {
                continue;
            }
            if (signal_is_blocked(tids[t], sig)) {
                ++blocked_count;
            }
        }

        // Prefer a signal that no thread blocks, then a SIG_DFL signal, then
        // the lowest signal number.
        int score = blocked_count * 10 + preference;
        if (score < best_score) {
            best_score = score;
            best_signal = sig;
            best_blocked = blocked_count;
        }

        if (blocked_count == 0 && preference == 0) {
            break;
        }
    }

    if (best_signal == 0) {
        LOGE("freezer: no usable real time signal found");
        return false;
    }

    struct sigaction action{};
    action.sa_handler = freezer_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (sigaction(best_signal, &action, nullptr) != 0) {
        LOGE("freezer: sigaction(%d) failed: %s", best_signal, strerror(errno));
        return false;
    }

    g_signal = best_signal;
    g_handler_installed = true;
    LOGI("freezer: using signal %d (SIGRTMIN+%d), %d thread(s) block it",
         g_signal, g_signal - SIGRTMIN, best_blocked);
    return true;
}

// ---------------------------------------------------------------------------
// Dumper side of the companion protocol. Runs while the process is frozen:
// atomics and raw nanosleep only.
// ---------------------------------------------------------------------------

int load32(const int32_t *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

void store32(int32_t *p, int v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

bool companion_ready() {
    return g_control != nullptr &&
           __atomic_load_n(&g_control->magic, __ATOMIC_ACQUIRE) == kControlMagic &&
           load32(&g_control->ready) == 1;
}

// Posts a request and waits for the companion to complete it.
bool companion_request(int req, int wait_ms) {
    Control *c = g_control;
    const int seq = load32(&c->seq) + 1;
    store32(&c->req, req);
    store32(&c->seq, seq);
    const long long deadline = monotonic_ms() + wait_ms;
    while (load32(&c->ack) != seq) {
        if (monotonic_ms() >= deadline) {
            return false;
        }
        sleep_ms(1);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Companion side: root process, ordinary code (it is not the frozen process).
// ---------------------------------------------------------------------------

#define CLOG(prio, ...) __android_log_print(prio, "Il2CppDumperV2-root", __VA_ARGS__)

struct Remote {
    pid_t pid;
    uintptr_t addr;

    bool read(Control *out) const {
        struct iovec local{out, sizeof(*out)};
        struct iovec remote{reinterpret_cast<void *>(addr), sizeof(*out)};
        return process_vm_readv(pid, &local, 1, &remote, 1, 0) == sizeof(*out);
    }

    bool write_field(const int32_t *field_in_copy, const Control *copy, int value) const {
        const size_t off = reinterpret_cast<const char *>(field_in_copy) -
                           reinterpret_cast<const char *>(copy);
        struct iovec local{&value, sizeof(value)};
        struct iovec remote{reinterpret_cast<void *>(addr + off), sizeof(value)};
        return process_vm_writev(pid, &local, 1, &remote, 1, 0) == sizeof(value);
    }
};

#define RSET(remote, copy, field, value) (remote).write_field(&(copy).field, &(copy), (value))

struct Seized {
    pid_t tid;
    int pending_signal;
    bool stopped;
};

bool list_tasks(pid_t pid, std::vector<pid_t> &out) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/task", pid);
    DIR *dir = opendir(path);
    if (dir == nullptr) {
        return false;
    }
    out.clear();
    while (auto *e = readdir(dir)) {
        pid_t tid = 0;
        if (parse_tid(e->d_name, &tid)) {
            out.push_back(tid);
        }
    }
    closedir(dir);
    return true;
}

bool seized_contains(const std::vector<Seized> &v, pid_t tid) {
    for (const auto &s : v) {
        if (s.tid == tid) {
            return true;
        }
    }
    return false;
}

// Stops every thread of `pid` except `exclude` with PTRACE_SEIZE +
// PTRACE_INTERRUPT. This is a kernel level, per-thread stop: it needs no
// cooperation from the threads, so it also works for native bridge (Houdini)
// threads and for threads that block every signal.
void companion_freeze(pid_t pid, pid_t exclude, int timeout_ms,
                      std::vector<Seized> &seized, int *failed) {
    const long long deadline = monotonic_ms() + timeout_ms;
    std::vector<pid_t> tasks;
    *failed = 0;
    for (;;) {
        if (!list_tasks(pid, tasks)) {
            break;
        }
        bool added = false;
        for (pid_t tid : tasks) {
            if (tid == exclude || seized_contains(seized, tid)) {
                continue;
            }
            if (ptrace(PTRACE_SEIZE, tid, nullptr, nullptr) != 0) {
                if (errno != ESRCH) {
                    ++*failed;
                    CLOG(ANDROID_LOG_WARN, "PTRACE_SEIZE %d failed: %s", tid, strerror(errno));
                }
                continue;
            }
            ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr);
            seized.push_back({tid, 0, false});
            added = true;
        }

        // Wait for every seized thread to report its stop.
        for (;;) {
            bool all = true;
            for (auto &s : seized) {
                if (s.stopped || s.tid == 0) {
                    continue;
                }
                int status = 0;
                pid_t r = waitpid(s.tid, &status, __WALL | WNOHANG);
                if (r == s.tid) {
                    if (WIFSTOPPED(status)) {
                        s.stopped = true;
                        // A real signal arrived first: keep it, re-inject it
                        // on detach so the game does not lose it.
                        if ((status >> 16) == 0 && WSTOPSIG(status) != SIGTRAP) {
                            s.pending_signal = WSTOPSIG(status);
                        }
                    } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
                        s.tid = 0;
                    }
                } else if (r < 0 && errno == ECHILD) {
                    s.tid = 0;
                } else {
                    all = false;
                }
            }
            if (all || monotonic_ms() >= deadline) {
                break;
            }
            sleep_ms(1);
        }

        if (!added || monotonic_ms() >= deadline) {
            break;
        }
    }
}

int companion_thaw(std::vector<Seized> &seized) {
    int resumed = 0;
    for (auto &s : seized) {
        if (s.tid == 0) {
            continue;
        }
        if (!s.stopped) {
            // Not stopped yet: interrupt-stop it so that PTRACE_DETACH works.
            int status = 0;
            ptrace(PTRACE_INTERRUPT, s.tid, nullptr, nullptr);
            for (int i = 0; i < 200; ++i) {
                if (waitpid(s.tid, &status, __WALL | WNOHANG) == s.tid) {
                    break;
                }
                sleep_ms(1);
            }
        }
        if (ptrace(PTRACE_DETACH, s.tid, nullptr,
                   reinterpret_cast<void *>(static_cast<intptr_t>(s.pending_signal))) == 0) {
            ++resumed;
        }
    }
    seized.clear();
    return resumed;
}

} // namespace

Control *local_control() {
    return &g_local_control;
}

void use_control(Control *control, bool native_bridge) {
    g_control = control;
    g_native_bridge = native_bridge;
}

void companion_quit() {
    if (companion_ready()) {
        companion_request(3, 1000);
    }
}

bool companion_available() {
    return companion_ready();
}

void companion_handler(int client) {
    CompanionHello hello{};
    if (read(client, &hello, sizeof(hello)) != sizeof(hello) || hello.magic != kControlMagic) {
        CLOG(ANDROID_LOG_ERROR, "bad hello from module");
        return;
    }
    struct ucred cred{};
    socklen_t len = sizeof(cred);
    pid_t pid = hello.pid;
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.pid > 0 &&
        cred.pid != pid) {
        CLOG(ANDROID_LOG_WARN, "hello pid %d, peer pid %d: using peer", pid, cred.pid);
        pid = cred.pid;
    }

    Remote remote{pid, static_cast<uintptr_t>(hello.control_addr)};
    Control c{};
    // The module process is still being specialized: wait for the block.
    long long deadline = monotonic_ms() + 10000;
    while (!(remote.read(&c) && c.magic == kControlMagic)) {
        if (monotonic_ms() >= deadline) {
            CLOG(ANDROID_LOG_ERROR, "cannot read control block of %d at 0x%llx: %s", pid,
                 (unsigned long long) hello.control_addr, strerror(errno));
            return;
        }
        sleep_ms(10);
    }
    RSET(remote, c, ready, 1);
    CLOG(ANDROID_LOG_INFO, "companion attached to pid %d", pid);

    std::vector<Seized> seized;
    int handled = 0;
    long long frozen_at = 0;
    int max_ms = 0;
    const long long give_up = monotonic_ms() + 10 * 60 * 1000;

    for (;;) {
        if (!remote.read(&c)) {
            // Process gone. Exiting this thread also detaches every tracee.
            CLOG(ANDROID_LOG_INFO, "pid %d gone", pid);
            companion_thaw(seized);
            return;
        }

        // Watchdog: never keep the game stopped longer than max_ms.
        if (!seized.empty() && monotonic_ms() - frozen_at > max_ms) {
            int resumed = companion_thaw(seized);
            RSET(remote, c, watchdog_fired, 1);
            CLOG(ANDROID_LOG_WARN, "watchdog: released %d thread(s) after %d ms", resumed,
                 max_ms);
        }

        if (c.seq != handled) {
            handled = c.seq;
            if (c.req == 1 && seized.empty()) {
                int failed = 0;
                RSET(remote, c, watchdog_fired, 0);
                companion_freeze(pid, c.exclude_tid, c.timeout_ms > 0 ? c.timeout_ms : 3000,
                                 seized, &failed);
                int parked = 0;
                for (const auto &s : seized) {
                    parked += s.tid != 0 && s.stopped;
                }
                frozen_at = monotonic_ms();
                max_ms = c.max_ms > 0 ? c.max_ms : 8000;
                RSET(remote, c, parked, parked);
                RSET(remote, c, total, static_cast<int>(seized.size()));
                RSET(remote, c, failed, failed);
                RSET(remote, c, ack, handled);
                CLOG(ANDROID_LOG_INFO, "froze %d/%zu thread(s) of %d, %d failed", parked,
                     seized.size(), pid, failed);
            } else if (c.req == 2) {
                int resumed = companion_thaw(seized);
                RSET(remote, c, ack, handled);
                CLOG(ANDROID_LOG_INFO, "resumed %d thread(s)", resumed);
            } else if (c.req == 3) {
                companion_thaw(seized);
                RSET(remote, c, ready, 0);
                RSET(remote, c, ack, handled);
                return;
            } else {
                RSET(remote, c, ack, handled);
            }
        }

        if (seized.empty() && monotonic_ms() > give_up) {
            RSET(remote, c, ready, 0);
            return;
        }
        sleep_ms(seized.empty() ? 5 : 1);
    }
}

int freeze_process(int timeout_ms) {
    if (__atomic_load_n(&g_armed, __ATOMIC_ACQUIRE)) {
        return __atomic_load_n(&g_parked, __ATOMIC_ACQUIRE);
    }
    g_last_attempted = 1;
    g_last_signal = 0;
    g_last_parked = 0;
    g_last_expected = 0;
    g_last_skipped = 0;
    g_last_failed = 0;
    if (!install_handler()) {
        return 0;
    }
    g_last_signal = g_signal;

    __atomic_store_n(&g_parked, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_resumed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_release, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_armed, 1, __ATOMIC_RELEASE);

    // From here until thaw_process(), no allocation and no logging: a parked
    // thread may hold the allocator lock.
    static pid_t signalled[kMaxThreads];
    static pid_t tids[kMaxThreads];
    int signalled_count = 0;

    const pid_t self = current_tid();
    const pid_t pid = getpid();
    const long long deadline = monotonic_ms() + (timeout_ms > 0 ? timeout_ms : 0);

    signalled[signalled_count++] = self;
    const pid_t watchdog = static_cast<pid_t>(__atomic_load_n(&g_watch_tid, __ATOMIC_ACQUIRE));
    if (watchdog > 0) {
        signalled[signalled_count++] = watchdog;
    }

    int expected = 0;
    int skipped = 0;
    int failed = 0;
    int parked = 0;

    int dirfd = open_task_dir();
    if (dirfd < 0) {
        __atomic_store_n(&g_armed, 0, __ATOMIC_RELEASE);
        LOGE("freezer: cannot open /proc/self/task");
        return 0;
    }

    for (;;) {
        // Signal every thread we have not seen yet. A thread can be created
        // while we are walking the list, so the outer loop repeats until no
        // new thread appears.
        int tid_count = read_thread_ids(dirfd, tids, kMaxThreads);
        for (int i = 0; i < tid_count; ++i) {
            pid_t tid = tids[i];
            if (contains(signalled, signalled_count, tid)) {
                continue;
            }
            if (signalled_count >= kMaxThreads) {
                ++failed;
                continue;
            }
            signalled[signalled_count++] = tid;

            if (signal_is_blocked(tid, g_signal)) {
                ++skipped;
                continue;
            }

            if (syscall(SYS_tgkill, pid, tid, g_signal) == 0) {
                ++expected;
            } else if (errno != ESRCH) {
                ++failed;
            }
        }

        while ((parked = __atomic_load_n(&g_parked, __ATOMIC_ACQUIRE)) < expected &&
               monotonic_ms() < deadline) {
            sleep_ms(1);
        }

        bool unseen_thread = false;
        tid_count = read_thread_ids(dirfd, tids, kMaxThreads);
        for (int i = 0; i < tid_count; ++i) {
            if (!contains(signalled, signalled_count, tids[i])) {
                unseen_thread = true;
                break;
            }
        }

        if (!unseen_thread) {
            break;
        }
        if (monotonic_ms() >= deadline) {
            break;
        }
    }

    syscall(SYS_close, dirfd);

    parked = __atomic_load_n(&g_parked, __ATOMIC_ACQUIRE);

    // Optional extra hold: keep the rest of the process parked for a while
    // before (and during) the metadata walk. This makes the freeze clearly
    // observable and gives interrupted threads time to settle. No allocation
    // or logging happens here, so it is safe with parked threads.
    if (parked > 0 && DUMPER_FREEZE_HOLD_MS > 0) {
        sleep_ms(DUMPER_FREEZE_HOLD_MS);
    }

    g_last_parked = parked;
    g_last_expected = expected;
    g_last_skipped = skipped;
    g_last_failed = failed;
    g_last_valid = 1;

    return parked;
}

void thaw_process() {
    if (!__atomic_load_n(&g_armed, __ATOMIC_ACQUIRE)) {
        return;
    }

    release_parked_threads();
    __atomic_store_n(&g_armed, 0, __ATOMIC_RELEASE);

    // Threads are being released: logging and allocation are safe again.
    const int parked = __atomic_load_n(&g_parked, __ATOMIC_ACQUIRE);
    const long long deadline = monotonic_ms() + 1000;
    int resumed = 0;
    while ((resumed = __atomic_load_n(&g_resumed, __ATOMIC_ACQUIRE)) < parked &&
           monotonic_ms() < deadline) {
        sleep_ms(1);
    }

    if (resumed < parked) {
        LOGW("freezer: only %d/%d thread(s) resumed", resumed, parked);
    } else if (parked > 0) {
        LOGI("freezer: %d thread(s) resumed", resumed);
    }

    if (g_last_valid) {
        LOGI("freezer: %d/%d thread(s) parked, %d skipped, %d failed",
             g_last_parked, g_last_expected, g_last_skipped, g_last_failed);
        g_last_valid = 0;
    }

    __atomic_store_n(&g_resumed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_parked, 0, __ATOMIC_RELEASE);
}

Report last_report() {
    Report report;
    report.attempted = g_last_attempted != 0;
    report.frozen = g_last_parked > 0;
    report.signal = g_last_signal;
    report.parked = g_last_parked;
    report.expected = g_last_expected;
    report.skipped = g_last_skipped;
    report.failed = g_last_failed;
    report.watchdog_fired = __atomic_load_n(&g_watch_fired, __ATOMIC_ACQUIRE) != 0;
    report.method = g_last_method;
    return report;
}

ScopedFreeze::ScopedFreeze(int timeout_ms, bool enabled) {
    attempted_ = enabled;
    if (!enabled) {
        return;
    }

    if (companion_ready()) {
        // Root companion: ptrace stop, works under the native bridge.
        Control *c = g_control;
        g_last_attempted = 1;
        g_last_signal = 0;
        store32(&c->exclude_tid, static_cast<int>(current_tid()));
        store32(&c->timeout_ms, timeout_ms);
        store32(&c->max_ms, DUMPER_FREEZE_MAX_MS);
        const bool ok = companion_request(1, timeout_ms + 2000);
        parked_ = ok ? load32(&c->parked) : 0;
        g_last_parked = parked_;
        g_last_expected = ok ? load32(&c->total) : 0;
        g_last_failed = ok ? load32(&c->failed) : 0;
        g_last_skipped = 0;
        g_last_method = "ptrace (root companion)";
        companion_ = true;
        if (parked_ == 0) {
            companion_request(2, 2000);
            companion_ = false;
            LOGW("freezer: companion did not stop any thread (%s), dumping anyway",
                 ok ? "0 parked" : "no answer");
        }
        return;
    }

    if (g_native_bridge) {
        // Signals are delivered through Houdini and do not reach host threads
        // reliably: never use the signal barrier here.
        attempted_ = false;
        g_last_attempted = 1;
        g_last_parked = 0;
        g_last_method = "none";
        LOGW("freezer: root companion unavailable under the native bridge, "
             "dumping without freeze");
        return;
    }
    g_last_method = "signal barrier";
    // Without the watchdog a frozen thread holding an il2cpp lock would hang
    // the game forever, so refuse to freeze rather than risk it.
    if (!start_watchdog()) {
        join_watchdog();
        attempted_ = false;
        LOGW("freezer: cannot start the watchdog, dumping without freeze");
        return;
    }
    watchdog_ = true;
    parked_ = freeze_process(timeout_ms);
    if (parked_ == 0) {
        // Disarm before logging: a late signal could still park a thread that
        // holds an internal lock, and logging may take that same lock.
        thaw_process();
        join_watchdog();
        watchdog_ = false;
        LOGW("freezer: process was not frozen, dumping anyway");
        return;
    }
    arm_watchdog();
}

ScopedFreeze::~ScopedFreeze() {
    if (companion_) {
        const bool ok = companion_request(2, 3000);
        __atomic_store_n(&g_watch_fired, load32(&g_control->watchdog_fired), __ATOMIC_RELEASE);
        LOGI("freezer: companion %s, %d/%d thread(s) were stopped, %d failed",
             ok ? "resumed the process" : "did not answer the thaw request",
             g_last_parked, g_last_expected, g_last_failed);
        if (g_watch_fired) {
            LOGW("freezer: companion watchdog released the process after %d ms; the dump "
                 "finished on the live process", DUMPER_FREEZE_MAX_MS);
        }
        return;
    }
    if (!attempted_) {
        return;
    }
    if (watchdog_) {
        finish_watchdog();
    }
    thaw_process();
    if (watchdog_) {
        join_watchdog();
        if (__atomic_load_n(&g_watch_fired, __ATOMIC_ACQUIRE)) {
            LOGW("freezer: watchdog released the process after %d ms. The dump was "
                 "blocked, most likely on an il2cpp lock held by a frozen thread; "
                 "it finished on the live process.", DUMPER_FREEZE_MAX_MS);
        }
    }
}

} // namespace freezer
