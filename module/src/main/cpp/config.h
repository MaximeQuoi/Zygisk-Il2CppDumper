//
// Zygisk-Il2CppDumper v2 - build time configuration.
//

#ifndef ZYGISK_IL2CPPDUMPER_CONFIG_H
#define ZYGISK_IL2CPPDUMPER_CONFIG_H

#include "game.h"

#define DUMPER_VERSION "2.5.0"

// Freeze every thread of the process (except the dumping thread) before
// walking the il2cpp metadata. This gives a coherent snapshot and prevents
// the game from mutating data while it is being read.
//
// 1 = enabled (default), 0 = disabled (old v1 behaviour).
#ifndef DUMPER_FREEZE_BEFORE_DUMP
#define DUMPER_FREEZE_BEFORE_DUMP 1
#endif

// How long (milliseconds) we wait for the other threads to park before
// starting the dump anyway. Some threads may be in an uninterruptible
// syscall, or may block the signal we use.
#ifndef DUMPER_FREEZE_TIMEOUT_MS
#define DUMPER_FREEZE_TIMEOUT_MS 3000
#endif

// Real time signal used for the freeze barrier, relative to SIGRTMIN.
// The dumper picks a signal that is not already used and not blocked by the
// process threads. This is only the first candidate.
#ifndef DUMPER_FREEZE_SIGNAL_OFFSET
#define DUMPER_FREEZE_SIGNAL_OFFSET 6
#endif

// Keep the rest of the process parked for this long after every thread has
// been frozen, before the metadata walk. Debug only: a long freeze makes ART
// and app watchdogs (FinalizerWatchdog, anti-cheat timers) kill the process
// once it resumes. Keep 0.
#ifndef DUMPER_FREEZE_HOLD_MS
#define DUMPER_FREEZE_HOLD_MS 0
#endif

// Safety net. The il2cpp API takes il2cpp's global metadata lock even for
// classes that are already initialized. If a frozen thread held that lock,
// the dump would block forever and the game would stay frozen. A watchdog
// thread releases every frozen thread after this delay; the dump then
// finishes on a live process instead of hanging.
#ifndef DUMPER_FREEZE_MAX_MS
#define DUMPER_FREEZE_MAX_MS 8000
#endif

// Extra delay between "il2cpp is ready" and the freeze + dump. Increase it if
// the game loads assemblies late and your dump is missing classes.
#ifndef DUMPER_DELAY_BEFORE_DUMP_MS
#define DUMPER_DELAY_BEFORE_DUMP_MS 0
#endif

// Walk the metadata once *without* freezing before the real dump. This forces
// il2cpp to lazily build its method tables, generic classes and static fields,
// so the frozen walk below only reads memory and never allocates. Without it,
// a frozen thread could hold the allocator lock while il2cpp needs to allocate,
// which would deadlock the whole process.
//
// 1 = enabled (default), 0 = dump immediately (faster, less safe).
#ifndef DUMPER_WARMUP_BEFORE_FREEZE
#define DUMPER_WARMUP_BEFORE_FREEZE 1
#endif

// File written in /data/data/<package>/files/.
#ifndef DUMPER_OUTPUT_FILE
#define DUMPER_OUTPUT_FILE "dump.cs"
#endif

// How long we wait for libil2cpp.so to appear (milliseconds).
#ifndef DUMPER_LIB_WAIT_MS
#define DUMPER_LIB_WAIT_MS 60000
#endif

// How long we wait for the il2cpp runtime to be ready (milliseconds).
#ifndef DUMPER_IL2CPP_WAIT_MS
#define DUMPER_IL2CPP_WAIT_MS 60000
#endif

// Memory dump (decrypted global-metadata + libil2cpp image) for games whose
// il2cpp API is stripped. 0 = never, 1 = when the API is incomplete (default),
// 2 = always, in addition to dump.cs.
#ifndef DUMPER_MEMORY_DUMP
#define DUMPER_MEMORY_DUMP 1
#endif

// Extra wait before the memory dump, so the game has loaded its assemblies.
#ifndef DUMPER_MEMORY_DUMP_DELAY_MS
#define DUMPER_MEMORY_DUMP_DELAY_MS 20000
#endif

// Used when il2cpp_domain_get is stripped too: fixed wait after libil2cpp.so
// appears.
#ifndef DUMPER_BLIND_WAIT_MS
#define DUMPER_BLIND_WAIT_MS 30000
#endif

#endif // ZYGISK_IL2CPPDUMPER_CONFIG_H
