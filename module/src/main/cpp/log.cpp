//
// Zygisk-Il2CppDumper v2 - logcat + on-device log file.
//
// The module runs inside the game process, so it cannot rely on the user
// having adb. Every log line is mirrored to
// /data/data/<package>/files/dumper.log, which can be copied with a root file
// manager or `adb pull`.
//
// Logging is deliberately never performed while the freeze barrier is armed:
// a parked thread could hold an internal lock used by printf/localtime.
//

#include "log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// -1 = not initialized, -2 = initialization in progress, >= 0 = append fd.
std::atomic<int> g_log_fd{-1};

char level_char(int priority) {
    switch (priority) {
        case ANDROID_LOG_ERROR:
            return 'E';
        case ANDROID_LOG_WARN:
            return 'W';
        case ANDROID_LOG_DEBUG:
            return 'D';
        case ANDROID_LOG_VERBOSE:
            return 'V';
        default:
            return 'I';
    }
}

} // namespace

void dlog_init(const char *data_dir) {
    if (data_dir == nullptr || data_dir[0] == '\0') {
        return;
    }

    int expected = -1;
    if (!g_log_fd.compare_exchange_strong(expected, -2)) {
        // Already initialized (or being initialized by another thread).
        return;
    }

    int fd = -1;
    char files_dir[512];
    int n = snprintf(files_dir, sizeof(files_dir), "%s/files", data_dir);
    if (n > 0 && n < static_cast<int>(sizeof(files_dir))) {
        mkdir(files_dir, 0700);

        char path[576];
        n = snprintf(path, sizeof(path), "%s/dumper.log", files_dir);
        if (n > 0 && n < static_cast<int>(sizeof(path))) {
            fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        }
    }

    g_log_fd.store(fd);
}

void dlog(int priority, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(priority, LOG_TAG, fmt, args);
    va_end(args);

    const int fd = g_log_fd.load(std::memory_order_relaxed);
    if (fd < 0) {
        return;
    }

    char message[1024];
    va_start(args, fmt);
    int message_len = vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    if (message_len < 0) {
        return;
    }
    if (message_len >= static_cast<int>(sizeof(message))) {
        message_len = static_cast<int>(sizeof(message)) - 1;
    }

    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm local{};
    localtime_r(&ts.tv_sec, &local);

    char line[1152];
    int head = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03ld %c ",
                        local.tm_hour, local.tm_min, local.tm_sec,
                        ts.tv_nsec / 1000000L, level_char(priority));
    if (head < 0 || head >= static_cast<int>(sizeof(line))) {
        return;
    }

    int copy = message_len;
    if (head + copy + 1 >= static_cast<int>(sizeof(line))) {
        copy = static_cast<int>(sizeof(line)) - head - 2;
    }
    if (copy > 0) {
        memcpy(line + head, message, static_cast<size_t>(copy));
    }
    line[head + copy] = '\n';

    ssize_t ignored = write(fd, line, static_cast<size_t>(head + copy + 1));
    (void) ignored;
}
