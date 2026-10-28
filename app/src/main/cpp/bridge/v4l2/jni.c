/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Handles JNI boundary data caching and the blocking V4L2 IPC socket loop.
#define _GNU_SOURCE
#include <jni.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <android/log.h>
#include "vsouchi_ipc.h"
#include "bridge/v4l2/internal.h"
#include "bridge/v4l2/v4l2.h"

#ifndef KERNEL_VERSION
#define KERNEL_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + (c))
#endif

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "vSouchi:BridgeJNI", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "vSouchi:BridgeJNI", __VA_ARGS__)

static int g_socket_fd = -1;
static char g_device_name[32] = "vSouchi Virtual Camera";
static uint32_t g_kernel_version = 0;
char g_bus_info[32] = "platform:vsouchi-cam0";

int g_has_focus = 0;
float g_min_focus_dist = 0.0f;
int g_has_exposure = 0;
long g_min_exposure = 0;
long g_max_exposure = 0;

int g_sensor_orientation = 0;
int g_sensor_hflip = 0;
int g_sensor_vflip = 0;

int g_current_focus_auto = 1;
int g_current_focus_abs = 0;
int g_current_exposure_auto = 3;
int g_current_exposure_abs = 156;

int g_last_ctrl_id = 0;
int g_last_ctrl_val = 0;

static jmethodID method_onStreamOn = NULL;
static jmethodID method_onStreamOff = NULL;
static jmethodID method_onDisconnect = NULL;
static jmethodID method_onSetControl = NULL;

static uint32_t get_real_kernel_version() {
    struct utsname un;
    if (uname(&un) == 0) {
        int major = 0, minor = 0, rev = 0;
        sscanf(un.release, "%d.%d.%d", &major, &minor, &rev);
        return KERNEL_VERSION(major, minor, rev);
    }
    return KERNEL_VERSION(5, 10, 0); 
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    JNIEnv* env;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return -1;
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL Java_io_github_lumaparallax_vsouchi_v4l2_NativeBridge_nativeSetDeviceInfo(JNIEnv *env, jobject thiz, jstring deviceName, jstring cameraId) {
    const char *nativeString = (*env)->GetStringUTFChars(env, deviceName, 0);
    snprintf(g_device_name, sizeof(g_device_name), "%s", nativeString);
    (*env)->ReleaseStringUTFChars(env, deviceName, nativeString);

    const char *cidStr = (*env)->GetStringUTFChars(env, cameraId, 0);
    snprintf(g_bus_info, sizeof(g_bus_info), "platform:vsouchi-cam%s", cidStr);
    (*env)->ReleaseStringUTFChars(env, cameraId, cidStr);
}

JNIEXPORT void JNICALL Java_io_github_lumaparallax_vsouchi_v4l2_NativeBridge_nativeSetResolutions(JNIEnv *env, jobject thiz, jintArray formats, jintArray widths, jintArray heights, jintArray fps) {
    jsize len = (*env)->GetArrayLength(env, widths);
    jint *f = (*env)->GetIntArrayElements(env, formats, 0);
    jint *w = (*env)->GetIntArrayElements(env, widths, 0);
    jint *h = (*env)->GetIntArrayElements(env, heights, 0);
    jint *r = (*env)->GetIntArrayElements(env, fps, 0);

    v4l2_init_resolutions((int*)f, (int*)w, (int*)h, (int*)r, (int)len);

    (*env)->ReleaseIntArrayElements(env, formats, f, JNI_ABORT);
    (*env)->ReleaseIntArrayElements(env, widths, w, JNI_ABORT);
    (*env)->ReleaseIntArrayElements(env, heights, h, JNI_ABORT);
    (*env)->ReleaseIntArrayElements(env, fps, r, JNI_ABORT);
}

JNIEXPORT void JNICALL Java_io_github_lumaparallax_vsouchi_v4l2_NativeBridge_nativeSetCfaPattern(JNIEnv *env, jobject thiz, jint cfa) {
    v4l2_set_cfa_pattern((int)cfa);
}

JNIEXPORT void JNICALL Java_io_github_lumaparallax_vsouchi_v4l2_NativeBridge_nativeSetFormats(JNIEnv *env, jobject thiz, jintArray formats) {
    jsize len = (*env)->GetArrayLength(env, formats);
    jint *body = (*env)->GetIntArrayElements(env, formats, 0);
    v4l2_init_formats((int*)body, (int)len);
    (*env)->ReleaseIntArrayElements(env, formats, body, JNI_ABORT);
}

JNIEXPORT void JNICALL Java_io_github_lumaparallax_vsouchi_v4l2_NativeBridge_nativeSetCameraCaps(JNIEnv *env, jobject thiz, jboolean has_focus, jfloat min_focus, jboolean has_exp, jlong min_exp, jlong max_exp, jint orientation, jint hflip) {
    g_has_focus = has_focus;
    g_min_focus_dist = min_focus;
    g_has_exposure = has_exp;
    g_min_exposure = min_exp;
    g_max_exposure = max_exp;
    g_sensor_orientation = orientation;
    g_sensor_hflip = hflip;
    g_sensor_vflip = 0;
    LOGI("Hardware Caps Cached -> Focus: %d (Max: %f), Exposure: %d, Orientation: %d, HFlip: %d", 
         g_has_focus, g_min_focus_dist, g_has_exposure, g_sensor_orientation, g_sensor_hflip);
}

JNIEXPORT jboolean JNICALL Java_io_github_lumaparallax_vsouchi_v4l2_NativeBridge_nativeConnect(JNIEnv *env, jobject thiz, jint uds_fd) {
    if (uds_fd < 0) return JNI_FALSE;
    g_socket_fd = uds_fd;
    return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_io_github_lumaparallax_vsouchi_v4l2_NativeBridge_nativeStartLoop(JNIEnv *env, jobject thiz, jint socket_fd) {
    jclass clazz = (*env)->GetObjectClass(env, thiz);
    method_onStreamOn = (*env)->GetMethodID(env, clazz, "onStreamOn", "(IIII)V");
    method_onStreamOff = (*env)->GetMethodID(env, clazz, "onStreamOff", "()V");
    method_onDisconnect = (*env)->GetMethodID(env, clazz, "onDisconnect", "()V");
    method_onSetControl = (*env)->GetMethodID(env, clazz, "onSetControl", "(II)V");

    v4l2_start_worker();

    while (1) {
        IpcOutboundHeader req;
        if (read_exact(socket_fd, &req, sizeof(IpcOutboundHeader)) != 0) {
            (*env)->CallVoidMethod(env, thiz, method_onStreamOff);
            v4l2_stop_worker();
            v4l2_cleanup();
            break;
        }
        
        int signal = v4l2_handle_command(socket_fd, req.client_fd, req.cmd, g_device_name, g_kernel_version);
        if (signal < 0) {
            (*env)->CallVoidMethod(env, thiz, method_onStreamOff);
            v4l2_stop_worker();
            v4l2_cleanup();
            break;
        }

        if (signal == 1) { 
            (*env)->CallVoidMethod(env, thiz, method_onStreamOn, 
                                   v4l2_get_width(), v4l2_get_height(), 
                                   v4l2_get_fps(), v4l2_get_android_format());
        } else if (signal == 2) { 
            (*env)->CallVoidMethod(env, thiz, method_onStreamOff);
        } else if (signal == 3) { 
            (*env)->CallVoidMethod(env, thiz, method_onDisconnect);
        } else if (signal == 4) { 
            (*env)->CallVoidMethod(env, thiz, method_onSetControl, g_last_ctrl_id, g_last_ctrl_val);
        }
    }
}
