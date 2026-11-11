//
// Created by Perfare on 2020/7/4.
//

#include "il2cpp_dump.h"

#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "config.h"
#include "freeze.h"
#include "il2cpp-class.h"
#include "il2cpp-tabledefs.h"
#include "log.h"
#include "sleep_util.h"
#include "xdl.h"

#define DO_API(r, n, p) r (*n) p

#include "il2cpp-api-functions.h"

#undef DO_API

static uint64_t il2cpp_base = 0;

static void init_il2cpp_api(void *handle) {
    int found = 0;
    int missing = 0;
    std::string missing_names;
#define DO_API(r, n, p) {                           \
    n = (r (*) p)xdl_sym(handle, #n, nullptr);      \
    if (!n) n = (r (*) p)xdl_dsym(handle, #n, nullptr); \
    if (n) { ++found; } else {                      \
        ++missing;                                  \
        if (missing <= 12) { missing_names += " "; missing_names += #n; } \
    }                                               \
}

#include "il2cpp-api-functions.h"

#undef DO_API
    LOGI("il2cpp API: %d found, %d missing%s%s", found, missing,
         missing ? ", e.g." : "", missing_names.c_str());
}

bool il2cpp_api_complete() {
    const bool modern = il2cpp_image_get_class && il2cpp_image_get_class_count;
    const bool legacy = il2cpp_get_corlib && il2cpp_class_from_name &&
                        il2cpp_class_get_method_from_name && il2cpp_string_new &&
                        il2cpp_class_from_system_type;
    return il2cpp_domain_get && il2cpp_domain_get_assemblies && il2cpp_assembly_get_image &&
           il2cpp_image_get_name && (modern || legacy) && il2cpp_class_get_type &&
           il2cpp_class_from_type && il2cpp_class_get_name && il2cpp_class_get_namespace &&
           il2cpp_class_get_flags && il2cpp_class_is_valuetype && il2cpp_class_is_enum &&
           il2cpp_class_get_parent && il2cpp_class_get_interfaces && il2cpp_class_get_fields &&
           il2cpp_class_get_properties && il2cpp_class_get_methods && il2cpp_field_get_flags &&
           il2cpp_field_get_type && il2cpp_field_get_name && il2cpp_field_get_offset &&
           il2cpp_field_static_get_value && il2cpp_property_get_get_method &&
           il2cpp_property_get_set_method && il2cpp_property_get_name &&
           il2cpp_method_get_flags && il2cpp_method_get_return_type &&
           il2cpp_method_get_name && il2cpp_method_get_param_count &&
           il2cpp_method_get_param && il2cpp_method_get_param_name;
}

unsigned long long il2cpp_get_base() {
    return il2cpp_base;
}

static void write_method_modifier(std::ostream &out, uint32_t flags) {
    auto access = flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK;
    switch (access) {
        case METHOD_ATTRIBUTE_PRIVATE:
            out << "private ";
            break;
        case METHOD_ATTRIBUTE_PUBLIC:
            out << "public ";
            break;
        case METHOD_ATTRIBUTE_FAMILY:
            out << "protected ";
            break;
        case METHOD_ATTRIBUTE_ASSEM:
        case METHOD_ATTRIBUTE_FAM_AND_ASSEM:
            out << "internal ";
            break;
        case METHOD_ATTRIBUTE_FAM_OR_ASSEM:
            out << "protected internal ";
            break;
    }
    if (flags & METHOD_ATTRIBUTE_STATIC) {
        out << "static ";
    }
    if (flags & METHOD_ATTRIBUTE_ABSTRACT) {
        out << "abstract ";
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_REUSE_SLOT) {
            out << "override ";
        }
    } else if (flags & METHOD_ATTRIBUTE_FINAL) {
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_REUSE_SLOT) {
            out << "sealed override ";
        }
    } else if (flags & METHOD_ATTRIBUTE_VIRTUAL) {
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_NEW_SLOT) {
            out << "virtual ";
        } else {
            out << "override ";
        }
    }
    if (flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) {
        out << "extern ";
    }
}

static bool il2cpp_type_is_byref_safe(const Il2CppType *type) {
    auto byref = type->byref;
    if (il2cpp_type_is_byref) {
        byref = il2cpp_type_is_byref(type);
    }
    return byref;
}

static void dump_method(std::ostream &out, Il2CppClass *klass) {
    out << "\n\t// Methods\n";
    void *iter = nullptr;
    while (auto method = il2cpp_class_get_methods(klass, &iter)) {
        //TODO attribute
        if (method->methodPointer) {
            out << "\t// RVA: 0x";
            out << std::hex << (uint64_t) method->methodPointer - il2cpp_base;
            out << " VA: 0x";
            out << std::hex << (uint64_t) method->methodPointer;
            out << std::dec;
        } else {
            out << "\t// RVA: 0x VA: 0x0";
        }
        /*if (method->slot != 65535) {
            out << " Slot: " << std::dec << method->slot;
        }*/
        out << "\n\t";
        uint32_t iflags = 0;
        auto flags = il2cpp_method_get_flags(method, &iflags);
        write_method_modifier(out, flags);
        //TODO genericContainerIndex
        auto return_type = il2cpp_method_get_return_type(method);
        if (il2cpp_type_is_byref_safe(return_type)) {
            out << "ref ";
        }
        auto return_class = il2cpp_class_from_type(return_type);
        out << il2cpp_class_get_name(return_class) << " " << il2cpp_method_get_name(method)
            << "(";
        auto param_count = il2cpp_method_get_param_count(method);
        for (uint32_t i = 0; i < param_count; ++i) {
            if (i > 0) {
                out << ", ";
            }
            auto param = il2cpp_method_get_param(method, i);
            auto attrs = param->attrs;
            if (il2cpp_type_is_byref_safe(param)) {
                if (attrs & PARAM_ATTRIBUTE_OUT && !(attrs & PARAM_ATTRIBUTE_IN)) {
                    out << "out ";
                } else if (attrs & PARAM_ATTRIBUTE_IN && !(attrs & PARAM_ATTRIBUTE_OUT)) {
                    out << "in ";
                } else {
                    out << "ref ";
                }
            } else {
                if (attrs & PARAM_ATTRIBUTE_IN) {
                    out << "[In] ";
                }
                if (attrs & PARAM_ATTRIBUTE_OUT) {
                    out << "[Out] ";
                }
            }
            auto parameter_class = il2cpp_class_from_type(param);
            out << il2cpp_class_get_name(parameter_class) << " "
                << il2cpp_method_get_param_name(method, i);
        }
        out << ") { }\n";
        //TODO GenericInstMethod
    }
}

static void dump_property(std::ostream &out, Il2CppClass *klass) {
    out << "\n\t// Properties\n";
    void *iter = nullptr;
    while (auto prop_const = il2cpp_class_get_properties(klass, &iter)) {
        //TODO attribute
        auto prop = const_cast<PropertyInfo *>(prop_const);
        auto get = il2cpp_property_get_get_method(prop);
        auto set = il2cpp_property_get_set_method(prop);
        auto prop_name = il2cpp_property_get_name(prop);
        out << "\t";
        Il2CppClass *prop_class = nullptr;
        uint32_t iflags = 0;
        if (get) {
            write_method_modifier(out, il2cpp_method_get_flags(get, &iflags));
            prop_class = il2cpp_class_from_type(il2cpp_method_get_return_type(get));
        } else if (set) {
            write_method_modifier(out, il2cpp_method_get_flags(set, &iflags));
            auto param = il2cpp_method_get_param(set, 0);
            prop_class = il2cpp_class_from_type(param);
        }
        if (prop_class) {
            out << il2cpp_class_get_name(prop_class) << " " << prop_name << " { ";
            if (get) {
                out << "get; ";
            }
            if (set) {
                out << "set; ";
            }
            out << "}\n";
        } else {
            if (prop_name) {
                out << " // unknown property " << prop_name;
            }
        }
    }
}

static void dump_field(std::ostream &out, Il2CppClass *klass) {
    out << "\n\t// Fields\n";
    auto is_enum = il2cpp_class_is_enum(klass);
    void *iter = nullptr;
    while (auto field = il2cpp_class_get_fields(klass, &iter)) {
        //TODO attribute
        out << "\t";
        auto attrs = il2cpp_field_get_flags(field);
        auto access = attrs & FIELD_ATTRIBUTE_FIELD_ACCESS_MASK;
        switch (access) {
            case FIELD_ATTRIBUTE_PRIVATE:
                out << "private ";
                break;
            case FIELD_ATTRIBUTE_PUBLIC:
                out << "public ";
                break;
            case FIELD_ATTRIBUTE_FAMILY:
                out << "protected ";
                break;
            case FIELD_ATTRIBUTE_ASSEMBLY:
            case FIELD_ATTRIBUTE_FAM_AND_ASSEM:
                out << "internal ";
                break;
            case FIELD_ATTRIBUTE_FAM_OR_ASSEM:
                out << "protected internal ";
                break;
        }
        if (attrs & FIELD_ATTRIBUTE_LITERAL) {
            out << "const ";
        } else {
            if (attrs & FIELD_ATTRIBUTE_STATIC) {
                out << "static ";
            }
            if (attrs & FIELD_ATTRIBUTE_INIT_ONLY) {
                out << "readonly ";
            }
        }
        auto field_type = il2cpp_field_get_type(field);
        auto field_class = il2cpp_class_from_type(field_type);
        out << il2cpp_class_get_name(field_class) << " " << il2cpp_field_get_name(field);
        //TODO 获取构造函数初始化后的字段值
        if (attrs & FIELD_ATTRIBUTE_LITERAL && is_enum) {
            uint64_t val = 0;
            il2cpp_field_static_get_value(field, &val);
            out << " = " << std::dec << val;
        }
        out << "; // 0x" << std::hex << il2cpp_field_get_offset(field) << std::dec << "\n";
    }
}

static void dump_type(std::ostream &out, const Il2CppType *type) {
    auto *klass = il2cpp_class_from_type(type);
    out << "\n// Namespace: " << il2cpp_class_get_namespace(klass) << "\n";
    auto flags = il2cpp_class_get_flags(klass);
    if (flags & TYPE_ATTRIBUTE_SERIALIZABLE) {
        out << "[Serializable]\n";
    }
    //TODO attribute
    auto is_valuetype = il2cpp_class_is_valuetype(klass);
    auto is_enum = il2cpp_class_is_enum(klass);
    auto visibility = flags & TYPE_ATTRIBUTE_VISIBILITY_MASK;
    switch (visibility) {
        case TYPE_ATTRIBUTE_PUBLIC:
        case TYPE_ATTRIBUTE_NESTED_PUBLIC:
            out << "public ";
            break;
        case TYPE_ATTRIBUTE_NOT_PUBLIC:
        case TYPE_ATTRIBUTE_NESTED_FAM_AND_ASSEM:
        case TYPE_ATTRIBUTE_NESTED_ASSEMBLY:
            out << "internal ";
            break;
        case TYPE_ATTRIBUTE_NESTED_PRIVATE:
            out << "private ";
            break;
        case TYPE_ATTRIBUTE_NESTED_FAMILY:
            out << "protected ";
            break;
        case TYPE_ATTRIBUTE_NESTED_FAM_OR_ASSEM:
            out << "protected internal ";
            break;
    }
    if (flags & TYPE_ATTRIBUTE_ABSTRACT && flags & TYPE_ATTRIBUTE_SEALED) {
        out << "static ";
    } else if (!(flags & TYPE_ATTRIBUTE_INTERFACE) && flags & TYPE_ATTRIBUTE_ABSTRACT) {
        out << "abstract ";
    } else if (!is_valuetype && !is_enum && flags & TYPE_ATTRIBUTE_SEALED) {
        out << "sealed ";
    }
    if (flags & TYPE_ATTRIBUTE_INTERFACE) {
        out << "interface ";
    } else if (is_enum) {
        out << "enum ";
    } else if (is_valuetype) {
        out << "struct ";
    } else {
        out << "class ";
    }
    out << il2cpp_class_get_name(klass); //TODO genericContainerIndex
    bool firstExtends = true;
    auto parent = il2cpp_class_get_parent(klass);
    if (!is_valuetype && !is_enum && parent) {
        auto parent_type = il2cpp_class_get_type(parent);
        if (parent_type->type != IL2CPP_TYPE_OBJECT) {
            out << " : " << il2cpp_class_get_name(parent);
            firstExtends = false;
        }
    }
    void *iter = nullptr;
    while (auto itf = il2cpp_class_get_interfaces(klass, &iter)) {
        out << (firstExtends ? " : " : ", ") << il2cpp_class_get_name(itf);
        firstExtends = false;
    }
    out << "\n{";
    dump_field(out, klass);
    dump_property(out, klass);
    dump_method(out, klass);
    //TODO EventInfo
    out << "}\n";
}

bool il2cpp_api_init(void *handle) {
    LOGI("il2cpp_handle: %p", handle);

    init_il2cpp_api(handle);

    xdl_info_t info{};
    if (xdl_info(handle, XDL_DI_DLINFO, &info) == 0 && info.dli_fbase) {
        il2cpp_base = reinterpret_cast<uint64_t>(info.dli_fbase);
    } else {
        Dl_info dlInfo{};
        void *any = il2cpp_domain_get ? (void *) il2cpp_domain_get
                                      : (void *) il2cpp_domain_get_assemblies;
        if (any && dladdr(any, &dlInfo) && dlInfo.dli_fbase) {
            il2cpp_base = reinterpret_cast<uint64_t>(dlInfo.dli_fbase);
        }
    }
    if (il2cpp_base) {
        LOGI("il2cpp_base: 0x%" PRIx64, il2cpp_base);
    } else {
        LOGW("unable to resolve il2cpp base address");
    }

    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies) {
        // Cannot ask il2cpp whether it is ready: give it time to load and
        // decrypt its metadata, then let the caller dump memory.
        LOGW("il2cpp_domain_get(_assemblies) missing: waiting %d ms instead",
             DUMPER_BLIND_WAIT_MS);
        sleep_full_ms(DUMPER_BLIND_WAIT_MS);
        return true;
    }

    LOGI("waiting for il2cpp runtime...");
    const int attempts = DUMPER_IL2CPP_WAIT_MS / 500;
    for (int i = 0; i < attempts; ++i) {
        auto domain = il2cpp_domain_get();
        if (domain) {
            size_t size = 0;
            auto assemblies = il2cpp_domain_get_assemblies(domain, &size);
            if (assemblies && size > 0) {
                LOGI("il2cpp ready: %zu assembly(ies)", size);
                if (il2cpp_thread_attach && !il2cpp_thread_attach(domain)) {
                    LOGW("il2cpp_thread_attach failed");
                }
                return true;
            }
        }
        sleep_full_ms(500);
    }

    LOGE("il2cpp runtime did not become ready in %d ms", DUMPER_IL2CPP_WAIT_MS);
    return false;
}

namespace {

// Discards everything written to it. Used for the pre-freeze warm-up pass.
class NullBuffer : public std::streambuf {
protected:
    int_type overflow(int_type ch) override {
        return traits_type::not_eof(ch);
    }

    std::streamsize xsputn(const char *, std::streamsize count) override {
        return count;
    }
};

// A single assembly prepared for the pre-2018.3 reflection path. The type
// pointers are resolved *before* the freeze so that no managed call and no
// allocation happens while the rest of the process is parked.
struct ReflectionImage {
    const char *name = nullptr;
    std::vector<Il2CppClass *> classes;
};

// Modern path (Unity > 2018.3): classes are read straight from the image.
void dump_image_classes(std::ostream &out, const Il2CppAssembly **assemblies, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        auto image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image) {
            continue;
        }
        out << "\n// Dll : " << il2cpp_image_get_name(image);
        auto classCount = il2cpp_image_get_class_count(image);
        for (size_t j = 0; j < classCount; ++j) {
            auto klass = il2cpp_image_get_class(image, j);
            if (!klass) {
                continue;
            }
            auto type = il2cpp_class_get_type(const_cast<Il2CppClass *>(klass));
            dump_type(out, type);
        }
    }
}

// Legacy path: resolve every System.Type through reflection. All of this runs
// before the freeze; the freeze only walks the prepared class pointers.
bool prepare_reflection_classes(const Il2CppAssembly **assemblies, size_t size,
                                std::vector<ReflectionImage> &images) {
    if (!il2cpp_get_corlib || !il2cpp_class_from_name ||
        !il2cpp_class_get_method_from_name || !il2cpp_string_new ||
        !il2cpp_class_from_system_type) {
        LOGE("dump: reflection API symbols are missing");
        return false;
    }

    auto corlib = il2cpp_get_corlib();
    auto assemblyClass = il2cpp_class_from_name(corlib, "System.Reflection", "Assembly");
    if (!assemblyClass) {
        LOGE("dump: System.Reflection.Assembly not found");
        return false;
    }
    auto assemblyLoad = il2cpp_class_get_method_from_name(assemblyClass, "Load", 1);
    auto assemblyGetTypes = il2cpp_class_get_method_from_name(assemblyClass, "GetTypes", 0);
    if (!assemblyLoad || !assemblyLoad->methodPointer) {
        LOGE("dump: Assembly::Load not found");
        return false;
    }
    if (!assemblyGetTypes || !assemblyGetTypes->methodPointer) {
        LOGE("dump: Assembly::GetTypes not found");
        return false;
    }

    LOGI("Assembly::Load: %p", assemblyLoad->methodPointer);
    LOGI("Assembly::GetTypes: %p", assemblyGetTypes->methodPointer);

    typedef void *(*Assembly_Load_ftn)(void *, Il2CppString *, void *);
    typedef Il2CppArray *(*Assembly_GetTypes_ftn)(void *, void *);

    images.clear();
    images.reserve(size);
    for (size_t i = 0; i < size; ++i) {
        auto image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image) {
            continue;
        }
        auto image_name = il2cpp_image_get_name(image);

        char base_name[256];
        const char *dot = strrchr(image_name, '.');
        size_t base_len = dot ? static_cast<size_t>(dot - image_name) : strlen(image_name);
        if (base_len >= sizeof(base_name)) {
            base_len = sizeof(base_name) - 1;
        }
        memcpy(base_name, image_name, base_len);
        base_name[base_len] = '\0';

        auto assemblyFileName = il2cpp_string_new(base_name);
        auto reflectionAssembly = ((Assembly_Load_ftn) assemblyLoad->methodPointer)(
                nullptr, assemblyFileName, nullptr);
        auto reflectionTypes = ((Assembly_GetTypes_ftn) assemblyGetTypes->methodPointer)(
                reflectionAssembly, nullptr);

        ReflectionImage entry;
        entry.name = image_name;
        if (reflectionTypes) {
            auto items = reflectionTypes->vector;
            entry.classes.reserve(reflectionTypes->max_length);
            for (il2cpp_array_size_t j = 0; j < reflectionTypes->max_length; ++j) {
                auto klass = il2cpp_class_from_system_type((Il2CppReflectionType *) items[j]);
                if (klass) {
                    entry.classes.push_back(klass);
                }
            }
        }
        images.push_back(std::move(entry));
    }
    return true;
}

void dump_reflection_classes(std::ostream &out, const std::vector<ReflectionImage> &images) {
    for (const auto &entry : images) {
        out << "\n// Dll : " << entry.name;
        for (auto *klass : entry.classes) {
            auto type = il2cpp_class_get_type(klass);
            dump_type(out, type);
        }
    }
}

} // namespace

bool il2cpp_dump(const char *outDir) {
    if (outDir == nullptr) {
        LOGE("dump: outDir is null");
        return false;
    }

    LOGI("dumping...");

    auto domain = il2cpp_domain_get();
    if (!domain) {
        LOGE("dump: il2cpp_domain_get() returned null");
        return false;
    }

    size_t size = 0;
    auto assemblies = il2cpp_domain_get_assemblies(domain, &size);
    if (!assemblies || size == 0) {
        LOGE("dump: no assembly found");
        return false;
    }

    // Prepare the output file *before* freezing, so that opening the file,
    // allocating the stream buffer and building the header never run while
    // the rest of the process is parked.
    std::string filesDir = std::string(outDir) + "/files";
    if (mkdir(filesDir.c_str(), 0700) != 0 && errno != EEXIST) {
        LOGE("dump: cannot create %s: %s", filesDir.c_str(), strerror(errno));
        return false;
    }

    const std::string outPath = filesDir + "/" + DUMPER_OUTPUT_FILE;
    const std::string tmpPath = outPath + ".tmp";

    std::ofstream out(tmpPath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        LOGE("dump: cannot open %s: %s", tmpPath.c_str(), strerror(errno));
        return false;
    }

    char timeBuffer[64] = {0};
    time_t now = time(nullptr);
    struct tm localTime{};
    localtime_r(&now, &localTime);
    strftime(timeBuffer, sizeof(timeBuffer), "%Y-%m-%d %H:%M:%S", &localTime);

    out << "// Zygisk-Il2CppDumper v" DUMPER_VERSION "\n";
    out << "// Generated: " << timeBuffer << "\n";
    out << "// Assemblies: " << std::dec << size << "\n\n";

    out << "// Images:\n";
    for (size_t i = 0; i < size; ++i) {
        auto image = il2cpp_assembly_get_image(assemblies[i]);
        if (image) {
            out << "// Image " << std::dec << i << ": " << il2cpp_image_get_name(image) << "\n";
        }
    }

    const bool modern = il2cpp_image_get_class && il2cpp_image_get_class_count;
    std::vector<ReflectionImage> reflectionImages;

    if (modern) {
        LOGI("Version greater than 2018.3");
        //使用il2cpp_image_get_class
    } else {
        LOGI("Version less than 2018.3");
        //使用反射
        if (!prepare_reflection_classes(assemblies, size, reflectionImages)) {
            out.close();
            unlink(tmpPath.c_str());
            return false;
        }
    }

    {
        // Force lazy il2cpp metadata initialization before parking the other
        // threads, so that the frozen walk below is read-only.
        if (DUMPER_FREEZE_BEFORE_DUMP && DUMPER_WARMUP_BEFORE_FREEZE) {
            NullBuffer nullBuffer;
            std::ostream nullStream(&nullBuffer);
            LOGI("warming up il2cpp metadata...");
            if (modern) {
                dump_image_classes(nullStream, assemblies, size);
            } else {
                dump_reflection_classes(nullStream, reflectionImages);
            }
            LOGI("warm-up done");
        }
    }

    const auto dumpStart = std::chrono::steady_clock::now();
    {
        // Freeze every other thread only for the metadata walk. The guard is
        // scoped so that the output is flushed and renamed on a live process.
        freezer::ScopedFreeze freeze(DUMPER_FREEZE_TIMEOUT_MS,
                                     DUMPER_FREEZE_BEFORE_DUMP != 0);
        if (modern) {
            dump_image_classes(out, assemblies, size);
        } else {
            dump_reflection_classes(out, reflectionImages);
        }
    }
    const auto dumpEnd = std::chrono::steady_clock::now();
    const auto dumpMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            dumpEnd - dumpStart).count();

    const freezer::Report freezeReport = freezer::last_report();
    if (freezeReport.attempted) {
        LOGI("freeze report: %s, signal=%d, parked=%d/%d, skipped=%d, failed=%d, dump=%lld ms",
             !freezeReport.frozen ? "NOT FROZEN"
                                  : (freezeReport.watchdog_fired ? "RELEASED BY WATCHDOG" : "OK"),
             freezeReport.signal,
             freezeReport.parked, freezeReport.expected, freezeReport.skipped,
             freezeReport.failed, static_cast<long long>(dumpMs));
    } else {
        LOGW("freeze report: not attempted");
    }

    out.flush();
    const bool writeOk = out.good();
    out.close();

    if (!writeOk) {
        LOGE("dump: write error on %s", tmpPath.c_str());
        unlink(tmpPath.c_str());
        return false;
    }

    if (rename(tmpPath.c_str(), outPath.c_str()) != 0) {
        LOGE("dump: cannot rename %s to %s: %s", tmpPath.c_str(), outPath.c_str(),
             strerror(errno));
        unlink(tmpPath.c_str());
        return false;
    }

    LOGI("dump done: %s", outPath.c_str());
    return true;
}
