/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef SHIM_LOG_H
#define SHIM_LOG_H

#include <stdio.h>
#include <stdlib.h>

#ifdef __ANDROID__
#include <android/log.h>
#define SHIM_LOGI(tag, ...) __android_log_print(ANDROID_LOG_INFO, tag, __VA_ARGS__)
#define SHIM_LOGE(tag, ...) __android_log_print(ANDROID_LOG_ERROR, tag, __VA_ARGS__)
#else
// Route to stderr only if VSOUCHI_DEBUG is set, keeping stdout clean for piped streaming
#define SHIM_LOGI(tag, ...) do { \
    if (getenv("VSOUCHI_DEBUG")) { \
        fprintf(stderr, "[" tag "] " __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } \
} while (0)

#define SHIM_LOGE(tag, ...) do { \
    fprintf(stderr, "[" tag " ERROR] " __VA_ARGS__); \
    fprintf(stderr, "\n"); \
} while (0)
#endif

#endif // SHIM_LOG_H
