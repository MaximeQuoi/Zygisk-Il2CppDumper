//
// Created by Perfare on 2020/7/4.
//

#include "hack.h"

#include <android/api-level.h>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <jni.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <thread>
#include <unistd.h>

#include "config.h"
#include "freeze.h"
#include "il2cpp_dump.h"
#include "mem_dump.h"
#include "log.h"
#include "sleep_util.h"
#include "xdl.h"

#if defined(__aarch64__)
#define DUMPER_ABI "arm64-v8a"
#elif defined(__arm__)
#define DUMPER_ABI "armeabi-v7a"
#elif defined(__x86_64__)
#define DUMPER_ABI "x86_64"
#else
#define DUMPER_ABI "x86"
#endif

static bool wait_for_il2cpp_library(void **handle) {
    const int attempts = DUMPER_LIB_WAIT_MS / 500;
    for (int i = 0; i < attempts; ++i) {
        *handle = xdl_open("libil2cpp.so", 0);
        if (*handle != nullptr) {
            LOGI("libil2cpp.so found after %d ms", i * 500);
            return true;
        }
        sleep_full_ms(500);
    }
    LOGE("libil2cpp.so not found after %d ms", DUMPER_LIB_WAIT_MS);
    return false;
}

static void hack_start(const char *game_data_dir) {
    LOGI("dumper v" DUMPER_VERSION " (" DUMPER_ABI ") started in thread %d, freeze=%d",
         gettid(), DUMPER_FREEZE_BEFORE_DUMP);
    void *handle = nullptr;
    if (!wait_for_il2cpp_library(&handle)) {
        LOGE("libil2cpp.so not found in thread %d", gettid());
        return;
    }

    if (!il2cpp_api_init(handle)) {
        LOGE("unable to initialize the il2cpp API");
        return;
    }

    if (DUMPER_DELAY_BEFORE_DUMP_MS > 0) {
        LOGI("waiting %d ms before dumping", DUMPER_DELAY_BEFORE_DUMP_MS);
        sleep_full_ms(DUMPER_DELAY_BEFORE_DUMP_MS);
    }

    LOGI("freeze method: %s", freezer::companion_available()
                                    ? "ptrace via root companion"
                                    : "no companion (signal barrier or none)");

    const bool complete = il2cpp_api_complete();
    if (complete) {
        // il2cpp_dump() freezes the process right before walking the metadata.
        il2cpp_dump(game_data_dir);
    } else {
        LOGW("il2cpp API incomplete (stripped exports): dump.cs is not possible, "
             "switching to the memory dump");
    }
    if (DUMPER_MEMORY_DUMP == 2 || (DUMPER_MEMORY_DUMP == 1 && !complete)) {
        if (DUMPER_MEMORY_DUMP_DELAY_MS > 0) {
            LOGI("memdump: waiting %d ms for the game to finish loading",
                 DUMPER_MEMORY_DUMP_DELAY_MS);
            sleep_full_ms(DUMPER_MEMORY_DUMP_DELAY_MS);
        }
        memory_dump(game_data_dir, static_cast<uintptr_t>(il2cpp_get_base()));
    }
    freezer::companion_quit();
}

static std::string GetLibDir(JavaVM *vms) {
    JNIEnv *env = nullptr;
    bool attached = false;
    if (vms->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (vms->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("AttachCurrentThread failed");
            return {};
        }
        attached = true;
    }

    std::string lib_dir;
    jclass activity_thread_clz = env->FindClass("android/app/ActivityThread");
    if (activity_thread_clz != nullptr) {
        jmethodID currentApplicationId = env->GetStaticMethodID(activity_thread_clz,
                                                                "currentApplication",
                                                                "()Landroid/app/Application;");
        if (currentApplicationId) {
            jobject application = env->CallStaticObjectMethod(activity_thread_clz,
                                                              currentApplicationId);
            jclass application_clazz = env->GetObjectClass(application);
            if (application_clazz) {
                jmethodID get_application_info = env->GetMethodID(application_clazz,
                                                                  "getApplicationInfo",
                                                                  "()Landroid/content/pm/ApplicationInfo;");
                if (get_application_info) {
                    jobject application_info = env->CallObjectMethod(application,
                                                                     get_application_info);
                    jfieldID native_library_dir_id = env->GetFieldID(
                            env->GetObjectClass(application_info), "nativeLibraryDir",
                            "Ljava/lang/String;");
                    if (native_library_dir_id) {
                        auto native_library_dir_jstring = (jstring) env->GetObjectField(
                                application_info, native_library_dir_id);
                        auto path = env->GetStringUTFChars(native_library_dir_jstring, nullptr);
                        LOGI("lib dir %s", path);
                        lib_dir = path;
                        env->ReleaseStringUTFChars(native_library_dir_jstring, path);
                    } else {
                        LOGE("nativeLibraryDir not found");
                    }
                } else {
                    LOGE("getApplicationInfo not found");
                }
            } else {
                LOGE("application class not found");
            }
        } else {
            LOGE("currentApplication not found");
        }
    } else {
        LOGE("ActivityThread not found");
    }

    if (attached) {
        vms->DetachCurrentThread();
    }
    return lib_dir;
}

static std::string GetNativeBridgeLibrary() {
    auto value = std::array<char, PROP_VALUE_MAX>();
    __system_property_get("ro.dalvik.vm.native.bridge", value.data());
    return {value.data()};
}

struct NativeBridgeCallbacks {
    uint32_t version;
    void *initialize;

    void *(*loadLibrary)(const char *libpath, int flag);

    void *(*getTrampoline)(void *handle, const char *name, const char *shorty, uint32_t len);

    void *isSupported;
    void *getAppEnv;
    void *isCompatibleWith;
    void *getSignalHandler;
    void *unloadLibrary;
    void *getError;
    void *isPathSupported;
    void *initAnonymousNamespace;
    void *createNamespace;
    void *linkNamespaces;

    void *(*loadLibraryExt)(const char *libpath, int flag, void *ns);
};

static bool NativeBridgeLoad(const char *game_data_dir, int api_level, void *data,
                             size_t length) {
    //TODO 等待houdini初始化
    sleep_full_ms(5000);

    auto libart = dlopen("libart.so", RTLD_NOW);
    if (!libart) {
        LOGE("libart.so not found");
        munmap(data, length);
        return false;
    }
    auto JNI_GetCreatedJavaVMs = (jint (*)(JavaVM **, jsize, jsize *)) dlsym(
            libart, "JNI_GetCreatedJavaVMs");
    LOGI("JNI_GetCreatedJavaVMs %p", JNI_GetCreatedJavaVMs);
    if (!JNI_GetCreatedJavaVMs) {
        munmap(data, length);
        return false;
    }

    JavaVM *vms_buf[1];
    jsize num_vms = 0;
    jint status = JNI_GetCreatedJavaVMs(vms_buf, 1, &num_vms);
    if (status != JNI_OK || num_vms == 0) {
        LOGE("GetCreatedJavaVMs error");
        munmap(data, length);
        return false;
    }
    JavaVM *vms = vms_buf[0];

    auto lib_dir = GetLibDir(vms);
    if (lib_dir.empty()) {
        LOGE("GetLibDir error");
        munmap(data, length);
        return false;
    }
    if (lib_dir.find("/lib/x86") != std::string::npos) {
        LOGI("no need NativeBridge");
        munmap(data, length);
        return false;
    }

    auto nb = dlopen("libhoudini.so", RTLD_NOW);
    if (!nb) {
        auto native_bridge = GetNativeBridgeLibrary();
        LOGI("native bridge: %s", native_bridge.data());
        nb = dlopen(native_bridge.data(), RTLD_NOW);
    }
    if (!nb) {
        LOGE("native bridge library not found");
        munmap(data, length);
        return false;
    }

    LOGI("nb %p", nb);
    auto callbacks = (NativeBridgeCallbacks *) dlsym(nb, "NativeBridgeItf");
    if (!callbacks) {
        LOGE("NativeBridgeItf not found");
        munmap(data, length);
        return false;
    }

    LOGI("NativeBridgeLoadLibrary %p", callbacks->loadLibrary);
    LOGI("NativeBridgeLoadLibraryExt %p", callbacks->loadLibraryExt);
    LOGI("NativeBridgeGetTrampoline %p", callbacks->getTrampoline);

    int fd = syscall(__NR_memfd_create, "anon", MFD_CLOEXEC);
    if (fd == -1) {
        LOGE("memfd_create failed: %s", strerror(errno));
        munmap(data, length);
        return false;
    }
    if (ftruncate(fd, (off_t) length) != 0) {
        LOGE("ftruncate failed: %s", strerror(errno));
        close(fd);
        munmap(data, length);
        return false;
    }
    void *mem = mmap(nullptr, length, PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        LOGE("mmap failed: %s", strerror(errno));
        close(fd);
        munmap(data, length);
        return false;
    }
    memcpy(mem, data, length);
    munmap(mem, length);
    munmap(data, length);

    char path[PATH_MAX];
    snprintf(path, PATH_MAX, "/proc/self/fd/%d", fd);
    LOGI("arm path %s", path);

    void *arm_handle;
    if (api_level >= 26 && callbacks->loadLibraryExt) {
        arm_handle = callbacks->loadLibraryExt(path, RTLD_NOW, (void *) 3);
    } else {
        arm_handle = callbacks->loadLibrary(path, RTLD_NOW);
    }
    if (arm_handle) {
        LOGI("arm handle %p", arm_handle);
        auto init = (void (*)(JavaVM *, void *)) callbacks->getTrampoline(arm_handle,
                                                                          "JNI_OnLoad",
                                                                          nullptr, 0);
        LOGI("JNI_OnLoad %p", init);
        if (init) {
            // The ARM copy has its own globals. Hand it the data dir followed
            // by the address of our control block, the one the root companion
            // polls (guest and host share the address space).
            static char handoff[PATH_MAX + 32];
            size_t dir_len = strlen(game_data_dir);
            if (dir_len > PATH_MAX - 1) {
                dir_len = PATH_MAX - 1;
            }
            memcpy(handoff, game_data_dir, dir_len);
            handoff[dir_len] = '\0';
            const uint32_t magic = freezer::kControlMagic;
            const uint64_t addr = reinterpret_cast<uintptr_t>(freezer::local_control());
            memcpy(handoff + dir_len + 1, &magic, sizeof(magic));
            memcpy(handoff + dir_len + 1 + sizeof(magic), &addr, sizeof(addr));
            init(vms, (void *) handoff);
            return true;
        }
    }

    close(fd);
    return false;
}

void hack_prepare(const char *game_data_dir, void *data, size_t length) {
    dlog_init(game_data_dir);
    LOGI("hack thread: %d (v" DUMPER_VERSION ", " DUMPER_ABI ")", gettid());
    int api_level = android_get_device_api_level();
    LOGI("api level: %d", api_level);

#if defined(__i386__) || defined(__x86_64__)
    if (!NativeBridgeLoad(game_data_dir, api_level, data, length)) {
#else
    (void) data;
    (void) length;
    (void) api_level;
#endif
        hack_start(game_data_dir);
#if defined(__i386__) || defined(__x86_64__)
    }
#endif
}

#if defined(__arm__) || defined(__aarch64__)

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    auto game_data_dir = (const char *) reserved;
    // This copy of the module runs through the native bridge and has its own
    // globals: open the log file here too, or nothing after JNI_OnLoad is kept.
    dlog_init(game_data_dir);
    LOGI("arm side loaded through the native bridge");
    if (game_data_dir != nullptr) {
        const char *tail = game_data_dir + strlen(game_data_dir) + 1;
        uint32_t magic = 0;
        memcpy(&magic, tail, sizeof(magic));
        if (magic == freezer::kControlMagic) {
            uint64_t addr = 0;
            memcpy(&addr, tail + sizeof(magic), sizeof(addr));
            freezer::use_control(reinterpret_cast<freezer::Control *>(
                                         static_cast<uintptr_t>(addr)), true);
            LOGI("using the x86 control block at 0x%llx", (unsigned long long) addr);
        } else {
            freezer::use_control(nullptr, true);
            LOGW("no control block from the x86 side (old x86 build?)");
        }
    }
    std::thread hack_thread(hack_start, game_data_dir);
    hack_thread.detach();
    return JNI_VERSION_1_6;
}

#endif
