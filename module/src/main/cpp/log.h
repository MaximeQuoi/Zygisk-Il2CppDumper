//
// Created by Perfare on 2020/7/4.
//

#ifndef ZYGISK_IL2CPPDUMPER_LOG_H
#define ZYGISK_IL2CPPDUMPER_LOG_H

#include <android/log.h>

#define LOG_TAG "Il2CppDumperV2"

// Opens <data_dir>/files/dumper.log in append mode. Every log line is also
// written there, so the user can retrieve a report without adb.
// Safe to call more than once; the first successful call wins.
void dlog_init(const char *data_dir);

// Writes one line to logcat and, when dlog_init() succeeded, to the log file.
void dlog(int priority, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOGD(...) dlog(ANDROID_LOG_DEBUG, __VA_ARGS__)
#define LOGW(...) dlog(ANDROID_LOG_WARN, __VA_ARGS__)
#define LOGE(...) dlog(ANDROID_LOG_ERROR, __VA_ARGS__)
#define LOGI(...) dlog(ANDROID_LOG_INFO, __VA_ARGS__)

#endif //ZYGISK_IL2CPPDUMPER_LOG_H
