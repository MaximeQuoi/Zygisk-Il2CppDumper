//
// Zygisk-Il2CppDumper v2 - raw memory dump.
//
// Used when the il2cpp API is not usable (protectors such as NetEase
// HTProtect strip every il2cpp_* export that libunity.so does not import, and
// ship global-metadata.dat encrypted). Once the runtime is up, the decrypted
// metadata lives somewhere in memory with its normal header, and libil2cpp.so
// is mapped decrypted. Both are copied to <data>/files/il2cpp-dump/ so that
// Il2CppDumper can be run offline (it asks for the dump address of the .so).
//
// Every read goes through process_vm_readv on our own pid: an unreadable page
// returns EFAULT instead of crashing the game. Locating is done before the
// freeze (it may allocate); the copy runs frozen with raw syscalls only.
//

#include "mem_dump.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <link.h>
#include <string>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

#include "config.h"
#include "freeze.h"
#include "log.h"

namespace {

constexpr uint32_t kMetadataMagic = 0xFAB11BAF;
constexpr size_t kChunk = 1 << 20;
constexpr size_t kPage = 4096;
constexpr uint64_t kMaxMetadataSize = 512ull << 20;

alignas(16) unsigned char g_buffer[kChunk];
alignas(16) unsigned char g_zero[kPage];

struct Region {
    uintptr_t start;
    uintptr_t end;
    std::string name;
};

struct Metadata {
    uintptr_t addr;
    uint64_t size;
    int version;
    uint32_t magic;
    bool by_layout;
};

constexpr uint32_t kHtpxMagic = 0x58505448; // "HTPX", HTProtect encrypted blob

ssize_t safe_read(uintptr_t addr, void *dst, size_t len) {
    struct iovec local{dst, len};
    struct iovec remote{reinterpret_cast<void *>(addr), len};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
}

bool skip_mapping(const std::string &name) {
    return name.rfind("/dev/", 0) == 0 || name == "[vvar]" || name == "[vsyscall]" ||
           name == "[vectors]" || name.rfind("/system/", 0) == 0 ||
           name.rfind("/apex/", 0) == 0 || name.rfind("/vendor/", 0) == 0 ||
           name.find(".art") != std::string::npos || name.find(".oat") != std::string::npos ||
           name.find(".vdex") != std::string::npos || name.find("dalvik-") != std::string::npos;
}

std::vector<Region> readable_regions() {
    std::vector<Region> regions;
    FILE *maps = fopen("/proc/self/maps", "re");
    if (maps == nullptr) {
        LOGE("memdump: cannot open /proc/self/maps: %s", strerror(errno));
        return regions;
    }
    char line[1024];
    const auto own_start = reinterpret_cast<uintptr_t>(g_buffer);
    while (fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long start = 0, end = 0;
        char perms[8] = {0};
        int name_pos = 0;
        if (sscanf(line, "%llx-%llx %7s %*s %*s %*s %n", &start, &end, perms, &name_pos) < 3) {
            continue;
        }
        if (perms[0] != 'r') {
            continue;
        }
        std::string name = name_pos > 0 ? std::string(line + name_pos) : std::string();
        while (!name.empty() && (name.back() == '\n' || name.back() == ' ')) {
            name.pop_back();
        }
        if (skip_mapping(name)) {
            continue;
        }
        if (own_start >= start && own_start < end) {
            continue; // our scan buffer would match itself
        }
        regions.push_back({static_cast<uintptr_t>(start), static_cast<uintptr_t>(end), name});
    }
    fclose(maps);
    return regions;
}

// Magic-free check. In a real global-metadata header the (offset, size)
// pairs describe sections laid out back to back: the first one starts right
// after the header and each next one starts where the previous ends (0 to 7
// bytes of alignment). Checked on real v31 files: exact match, file size
// included. Protectors often wipe the magic/version but must keep this.
bool layout_matches(const int32_t *h, uint64_t *end_out) {
    const int first = h[2];
    if (first < 0x80 || first > 0x200 || first % 8 != 0) {
        return false;
    }
    const int pairs = (first - 8) / 8;
    if (pairs < 10) {
        return false;
    }
    int64_t expected = first;
    for (int k = 0; k < pairs; ++k) {
        const int64_t off = h[2 + 2 * k];
        const int64_t size = h[3 + 2 * k];
        const int64_t gap = off - expected;
        if (size < 0 || gap < 0 || gap > 7) {
            return false;
        }
        expected = off + size;
    }
    if (expected < 64 * 1024 || static_cast<uint64_t>(expected) > kMaxMetadataSize) {
        return false;
    }
    *end_out = static_cast<uint64_t>(expected);
    return true;
}

// Checks a candidate header: magic + version + consistent pairs, or the
// back-to-back section layout alone (magic wiped by a protector).
bool validate_metadata(uintptr_t addr, Metadata *out) {
    int32_t h[128];
    if (safe_read(addr, h, sizeof(h)) != static_cast<ssize_t>(sizeof(h))) {
        return false;
    }
    uint64_t layout_end = 0;
    if (layout_matches(h, &layout_end)) {
        out->addr = addr;
        out->size = layout_end;
        out->version = h[1];
        out->magic = static_cast<uint32_t>(h[0]);
        out->by_layout = true;
        return true;
    }
    if (static_cast<uint32_t>(h[0]) != kMetadataMagic) {
        return false;
    }
    const int version = h[1];
    if (version < 16 || version > 40) {
        return false;
    }
    const int first = h[2];
    if (first < 0x40 || first > 0x200 || first % 8 != 0) {
        return false;
    }
    uint64_t end = static_cast<uint64_t>(first);
    for (int i = 2; i + 1 < first / 4; i += 2) {
        const int64_t off = h[i];
        const int64_t size = h[i + 1];
        if (off == 0 && size == 0) {
            continue;
        }
        if (off < first || size < 0 || static_cast<uint64_t>(off + size) > kMaxMetadataSize) {
            return false;
        }
        if (static_cast<uint64_t>(off + size) > end) {
            end = static_cast<uint64_t>(off + size);
        }
    }
    if (end < static_cast<uint64_t>(first) + 4096) {
        return false;
    }
    out->addr = addr;
    out->size = end;
    out->version = version;
    out->magic = kMetadataMagic;
    out->by_layout = false;
    return true;
}

std::vector<Metadata> find_metadata(const std::vector<Region> &regions) {
    std::vector<Metadata> found;
    uint64_t scanned = 0;
    int htpx = 0;
    for (const auto &r : regions) {
        uintptr_t pos = r.start;
        while (pos < r.end) {
            size_t want = r.end - pos < kChunk ? r.end - pos : kChunk;
            ssize_t n = safe_read(pos, g_buffer, want);
            if (n <= 0) {
                pos += kPage; // unreadable page, try the next one
                continue;
            }
            scanned += static_cast<uint64_t>(n);
            for (size_t off = 0; off + 4 <= static_cast<size_t>(n); off += 4) {
                uint32_t v;
                memcpy(&v, g_buffer + off, sizeof(v));
                if (v == kHtpxMagic) {
                    if (htpx < 8) {
                        LOGI("memdump: encrypted HTPX blob at 0x%" PRIxPTR " (%s)", pos + off,
                             r.name.empty() ? "anonymous" : r.name.c_str());
                    }
                    ++htpx;
                }
                bool candidate = v == kMetadataMagic;
                if (!candidate && off + 24 <= static_cast<size_t>(n)) {
                    // Cheap pre-check of the layout signature on the buffer.
                    int32_t first, size0, off1;
                    memcpy(&first, g_buffer + off + 8, 4);
                    memcpy(&size0, g_buffer + off + 12, 4);
                    memcpy(&off1, g_buffer + off + 16, 4);
                    const int64_t gap = static_cast<int64_t>(off1) - first - size0;
                    candidate = first >= 0x80 && first <= 0x200 && first % 8 == 0 &&
                                size0 >= 0 && gap >= 0 && gap <= 7;
                }
                if (!candidate) {
                    continue;
                }
                Metadata m{};
                if (found.size() < 16 && validate_metadata(pos + off, &m)) {
                    bool duplicate = false;
                    for (const auto &f : found) {
                        duplicate |= f.addr == m.addr;
                    }
                    if (!duplicate) {
                        LOGI("memdump: metadata candidate at 0x%" PRIxPTR ", %" PRIu64
                             " bytes, magic=0x%08x version=%d, found by %s (%s)",
                             m.addr, m.size, m.magic, m.version,
                             m.by_layout ? "section layout" : "magic",
                             r.name.empty() ? "anonymous" : r.name.c_str());
                        found.push_back(m);
                    }
                }
            }
            pos += static_cast<uintptr_t>(n);
        }
    }
    LOGI("memdump: scanned %" PRIu64 " MB in %zu region(s), %d HTPX blob(s)", scanned >> 20,
         regions.size(), htpx);
    return found;
}

struct Image {
    uintptr_t base = 0;
    uintptr_t size = 0;
};

struct PhdrSearch {
    uintptr_t base;
    uintptr_t size;
};

int phdr_callback(struct dl_phdr_info *info, size_t, void *data) {
    auto *search = static_cast<PhdrSearch *>(data);
    if (info->dlpi_addr != search->base &&
        (info->dlpi_name == nullptr || strstr(info->dlpi_name, "libil2cpp.so") == nullptr)) {
        return 0;
    }
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const auto &ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD) {
            continue;
        }
        lo = ph.p_vaddr < lo ? ph.p_vaddr : lo;
        hi = ph.p_vaddr + ph.p_memsz > hi ? ph.p_vaddr + ph.p_memsz : hi;
    }
    if (hi > lo) {
        search->base = info->dlpi_addr + lo;
        search->size = ((hi - lo) + kPage - 1) & ~(kPage - 1);
        return 1;
    }
    return 0;
}

// Extent of libil2cpp in memory: linker phdrs first (they survive a wiped ELF
// header), the in-memory ELF header as a fallback.
Image locate_il2cpp(uintptr_t base) {
    Image image;
    PhdrSearch search{base, 0};
    if (dl_iterate_phdr(phdr_callback, &search) == 1 && search.size > 0) {
        image.base = search.base;
        image.size = search.size;
        return image;
    }
    ElfW(Ehdr) eh{};
    if (safe_read(base, &eh, sizeof(eh)) == static_cast<ssize_t>(sizeof(eh)) &&
        memcmp(eh.e_ident, ELFMAG, SELFMAG) == 0 && eh.e_phnum > 0 && eh.e_phnum < 64) {
        ElfW(Phdr) ph[64];
        const size_t len = sizeof(ElfW(Phdr)) * eh.e_phnum;
        if (safe_read(base + eh.e_phoff, ph, len) == static_cast<ssize_t>(len)) {
            uintptr_t hi = 0;
            for (int i = 0; i < eh.e_phnum; ++i) {
                if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > hi) {
                    hi = ph[i].p_vaddr + ph[i].p_memsz;
                }
            }
            image.base = base;
            image.size = (hi + kPage - 1) & ~(kPage - 1);
        }
    }
    return image;
}

// Copies [addr, addr + size) to fd. Runs frozen: raw syscalls, static buffers.
// Unreadable pages are written as zeros so offsets stay correct.
bool copy_memory(int fd, uintptr_t addr, uint64_t size, uint64_t *holes) {
    uint64_t done = 0;
    while (done < size) {
        size_t want = size - done < kChunk ? static_cast<size_t>(size - done) : kChunk;
        ssize_t n = safe_read(addr + done, g_buffer, want);
        const void *src = g_buffer;
        if (n <= 0) {
            n = static_cast<ssize_t>(want < kPage ? want : kPage);
            src = g_zero;
            ++*holes;
        }
        size_t written = 0;
        while (written < static_cast<size_t>(n)) {
            ssize_t w = write(fd, static_cast<const unsigned char *>(src) + written,
                              static_cast<size_t>(n) - written);
            if (w < 0 && errno == EINTR) {
                continue;
            }
            if (w <= 0) {
                return false;
            }
            written += static_cast<size_t>(w);
        }
        done += static_cast<uint64_t>(n);
    }
    return true;
}

} // namespace

bool memory_dump(const char *data_dir, uintptr_t il2cpp_base) {
    std::string dir = std::string(data_dir) + "/files";
    mkdir(dir.c_str(), 0700);
    dir += "/il2cpp-dump";
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        LOGE("memdump: cannot create %s: %s", dir.c_str(), strerror(errno));
        return false;
    }

    // 1. Locate everything on the live process (allocation allowed).
    Image image = locate_il2cpp(il2cpp_base);
    if (image.size > 0) {
        LOGI("memdump: libil2cpp.so at 0x%" PRIxPTR ", %" PRIuPTR " bytes", image.base, image.size);
    } else {
        LOGW("memdump: cannot find the extent of libil2cpp.so (base 0x%" PRIxPTR ")", il2cpp_base);
    }

    LOGI("memdump: scanning memory for the decrypted global-metadata...");
    std::vector<Metadata> metadata = find_metadata(readable_regions());
    if (metadata.empty()) {
        LOGE("memdump: no decrypted metadata header found in memory. The protector may "
             "keep it encrypted or use a modified header.");
    }

    struct Output {
        int fd;
        uintptr_t addr;
        uint64_t size;
        std::string path;
        bool ok;
        uint64_t holes;
        int metadata_index;
    };
    std::vector<Output> outputs;
    char name[128];
    if (image.size > 0) {
        snprintf(name, sizeof(name), "/libil2cpp-0x%" PRIxPTR ".so", image.base);
        outputs.push_back({-1, image.base, image.size, dir + name, false, 0, -1});
    }
    for (size_t i = 0; i < metadata.size(); ++i) {
        snprintf(name, sizeof(name), "/global-metadata-%zu.dat", i);
        outputs.push_back({-1, metadata[i].addr, metadata[i].size, dir + name, false, 0,
                           static_cast<int>(i)});
    }
    if (outputs.empty()) {
        return false;
    }
    for (auto &o : outputs) {
        o.fd = open(o.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (o.fd < 0) {
            LOGE("memdump: cannot open %s: %s", o.path.c_str(), strerror(errno));
        }
    }

    // 2. Copy while the rest of the process is frozen.
    {
        freezer::ScopedFreeze freeze(DUMPER_FREEZE_TIMEOUT_MS, DUMPER_FREEZE_BEFORE_DUMP != 0);
        for (auto &o : outputs) {
            if (o.fd >= 0) {
                o.ok = copy_memory(o.fd, o.addr, o.size, &o.holes);
            }
        }
    }

    const freezer::Report report = freezer::last_report();
    LOGI("freeze report: %s, method=%s, parked=%d/%d, failed=%d",
         !report.frozen ? "NOT FROZEN" : (report.watchdog_fired ? "RELEASED BY WATCHDOG" : "OK"),
         report.method, report.parked, report.expected, report.failed);

    bool any = false;
    for (auto &o : outputs) {
        if (o.ok && o.metadata_index >= 0) {
            // Restore a standard header so Il2CppDumper accepts the file.
            const Metadata &m = metadata[o.metadata_index];
            int32_t fix[2] = {static_cast<int32_t>(kMetadataMagic), m.version};
            bool changed = false;
            if (m.magic != kMetadataMagic) {
                changed = true;
            }
            if (m.version < 16 || m.version > 40) {
                // 31 sections (header 0x100) is the Unity 2021.2+ layout.
                fix[1] = 31;
                changed = true;
                LOGW("memdump: version field is %d, writing 31 (guess, fix it by hand if "
                     "Il2CppDumper complains)", m.version);
            }
            if (changed && pwrite(o.fd, fix, sizeof(fix), 0) == static_cast<ssize_t>(sizeof(fix))) {
                LOGI("memdump: header of %s repaired (magic 0x%08x -> 0x%08x, version %d)",
                     o.path.c_str(), m.magic, kMetadataMagic, fix[1]);
            }
        }
        if (o.fd >= 0) {
            close(o.fd);
        }
        if (o.ok) {
            any = true;
            LOGI("memdump: wrote %s (%" PRIu64 " bytes, %" PRIu64 " unreadable page(s) zeroed)",
                 o.path.c_str(), o.size, o.holes);
        } else {
            LOGE("memdump: failed to write %s", o.path.c_str());
            unlink(o.path.c_str());
        }
    }
    if (any && image.size > 0) {
        LOGI("memdump: run Il2CppDumper on the .so and the metadata; when asked for the "
             "dump address, enter 0x%" PRIxPTR, image.base);
    }
    return any;
}
