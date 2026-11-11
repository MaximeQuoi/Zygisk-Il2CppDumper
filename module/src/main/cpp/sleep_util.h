//
// Zygisk-Il2CppDumper v2 - sleep that survives signals.
//
// usleep()/sleep() return early with EINTR when a signal arrives, which
// happens constantly under the native bridge (Houdini) and in games with
// profiling or anti-cheat timers. This one always sleeps the full duration.
//

#ifndef ZYGISK_IL2CPPDUMPER_SLEEP_UTIL_H
#define ZYGISK_IL2CPPDUMPER_SLEEP_UTIL_H

#include <cerrno>
#include <time.h>

inline void sleep_full_ms(long ms) {
    if (ms <= 0) {
        return;
    }
    struct timespec ts{};
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

#endif // ZYGISK_IL2CPPDUMPER_SLEEP_UTIL_H
