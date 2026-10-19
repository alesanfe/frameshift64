#pragma once

/* Android logcat wrappers shared by the platform/android TUs. Each
 * translation unit defines its own LOG_TAG before including so logcat
 * stays attributable per component (e.g. "ssb64.torch", "ssb64.touch"). */

#include <android/log.h>

#ifndef LOG_TAG
#define LOG_TAG "ssb64"
#endif

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
