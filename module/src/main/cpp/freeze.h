//
// Zygisk-Il2CppDumper v2 - process freeze barrier.
//
// The freezer parks every thread of the current process except the calling
// one. It does not use ptrace, so it works with the permissions of the app
// itself and does not need a second process.
//

#ifndef ZYGISK_IL2CPPDUMPER_FREEZE_H
#define ZYGISK_IL2CPPDUMPER_FREEZE_H

#include <stdint.h>

namespace freezer {

// Shared between the module (in the game process) and its root Zygisk
// companion. The companion reads and writes it with process_vm_readv/writev,
// so it must have the same layout on every ABI: 32-bit fields only.
struct Control {
    uint32_t magic;          // kControlMagic, set by the module
    int32_t ready;           // companion: 1 while it is polling
    int32_t req;             // dumper: 0 none, 1 freeze, 2 thaw, 3 quit
    int32_t seq;             // dumper: bumped on every request
    int32_t exclude_tid;     // dumper thread, never stopped
    int32_t timeout_ms;      // how long the companion may take to stop threads
    int32_t max_ms;          // companion watchdog: thaw after this long
    int32_t ack;             // companion: seq of the last completed request
    int32_t parked;
    int32_t total;
    int32_t failed;
    int32_t watchdog_fired;
};

constexpr uint32_t kControlMagic = 0x5a44465a; // "ZFDZ"

// Message sent to the companion from preAppSpecialize.
struct CompanionHello {
    uint32_t magic;
    int32_t pid;
    uint64_t control_addr;
};

// The control block of this copy of the module.
Control *local_control();

// Under the native bridge, the ARM copy uses the control block of the x86
// copy (same address space), the one the companion knows about.
void use_control(Control *control, bool native_bridge);

// True once the root companion is attached and polling the control block.
bool companion_available();

// Tells the companion the dump is over so it stops polling.
void companion_quit();

// Root companion entry point (runs in the Zygisk daemon, not in the game).
void companion_handler(int client);

// Result of the last freeze attempt. Valid after freeze_process() returns and
// kept until the next attempt, so callers can read it after the freeze scope
// has been released.
struct Report {
    bool attempted = false;
    bool frozen = false;
    int signal = 0;
    int parked = 0;
    int expected = 0;
    int skipped = 0;
    int failed = 0;
    bool watchdog_fired = false;
    const char *method = "none";
};

// Parks every thread of the current process except the caller.
// Returns the number of threads that actually parked. 0 means that the freeze
// could not be installed (or no other thread was alive).
int freeze_process(int timeout_ms);

// Statistics for the most recent freeze attempt.
Report last_report();

// Wakes every thread parked by freeze_process(). Safe to call when no freeze
// is active.
void thaw_process();

// RAII helper: freezes on construction, thaws on destruction.
class ScopedFreeze {
public:
    explicit ScopedFreeze(int timeout_ms, bool enabled = true);

    ~ScopedFreeze();

    ScopedFreeze(const ScopedFreeze &) = delete;

    ScopedFreeze &operator=(const ScopedFreeze &) = delete;

    bool frozen() const { return parked_ > 0; }

    int parked() const { return parked_; }

private:
    bool attempted_ = false;
    bool watchdog_ = false;
    bool companion_ = false;
    int parked_ = 0;
};

} // namespace freezer

#endif // ZYGISK_IL2CPPDUMPER_FREEZE_H
