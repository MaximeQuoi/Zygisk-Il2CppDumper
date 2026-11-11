#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

#include "config.h"
#include "freeze.h"
#include "hack.h"
#include "log.h"
#include "zygisk.hpp"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

class MyModule : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        if (args->nice_name == nullptr) {
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        auto package_name = env->GetStringUTFChars(args->nice_name, nullptr);
        if (package_name == nullptr) {
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        preSpecialize(package_name, args->app_data_dir);
        env->ReleaseStringUTFChars(args->nice_name, package_name);
    }

    void postAppSpecialize(const AppSpecializeArgs *) override {
        if (!enable_hack || game_data_dir == nullptr) {
            return;
        }
        std::thread hack_thread(hack_prepare, game_data_dir, data, length);
        hack_thread.detach();
    }

private:
    Api *api = nullptr;
    JNIEnv *env = nullptr;
    bool enable_hack = false;
    char *game_data_dir = nullptr;
    void *data = nullptr;
    size_t length = 0;

    void preSpecialize(const char *package_name, jstring app_data_dir) {
        if (strcmp(package_name, GamePackageName) != 0) {
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        if (app_data_dir == nullptr) {
            LOGE("app data dir is null");
            return;
        }

        auto data_dir = env->GetStringUTFChars(app_data_dir, nullptr);
        if (data_dir == nullptr) {
            LOGE("unable to read app data dir");
            return;
        }

        LOGI("detected target: %s", package_name);
        enable_hack = true;

        // Root companion: it stops the game threads with ptrace when the dump
        // asks for it, through a control block in our memory. The socket is
        // closed right away (API v2 closes module fds after preAppSpecialize).
        auto *control = freezer::local_control();
        control->magic = freezer::kControlMagic;
        freezer::use_control(control, false);
        int companion = api->connectCompanion();
        if (companion >= 0) {
            freezer::CompanionHello hello{};
            hello.magic = freezer::kControlMagic;
            hello.pid = getpid();
            hello.control_addr = reinterpret_cast<uintptr_t>(control);
            if (write(companion, &hello, sizeof(hello)) != sizeof(hello)) {
                LOGW("companion: write failed: %s", strerror(errno));
            }
            close(companion);
        } else {
            LOGW("companion: connectCompanion failed");
        }
        game_data_dir = new char[strlen(data_dir) + 1];
        strcpy(game_data_dir, data_dir);
        env->ReleaseStringUTFChars(app_data_dir, data_dir);

#if defined(__i386__) || defined(__x86_64__)
        // On x86 devices the game may be an ARM app running through a native
        // bridge (Houdini). Load the matching ARM build of this module.
#if defined(__i386__)
        const char *path = "zygisk/armeabi-v7a.so";
#else
        const char *path = "zygisk/arm64-v8a.so";
#endif
        int dirfd = api->getModuleDir();
        int fd = openat(dirfd, path, O_RDONLY);
        if (fd != -1) {
            struct stat sb{};
            if (fstat(fd, &sb) == 0 && sb.st_size > 0) {
                length = (size_t) sb.st_size;
                data = mmap(nullptr, length, PROT_READ, MAP_PRIVATE, fd, 0);
                if (data == MAP_FAILED) {
                    data = nullptr;
                    length = 0;
                    LOGE("unable to map %s", path);
                }
            } else {
                LOGE("unable to stat %s: %s", path, strerror(errno));
            }
            close(fd);
        } else {
            LOGE("unable to open %s: %s", path, strerror(errno));
        }

        if (data == nullptr) {
            enable_hack = false;
        }
#endif
    }
};

REGISTER_ZYGISK_MODULE(MyModule)
REGISTER_ZYGISK_COMPANION(freezer::companion_handler)
