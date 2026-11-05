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
static uint32_t g_kernel_version = 0;
char g_device_name[32] = "vSouchi Virtual Camera";
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
int g_current_exposure_auto = 0;
int g_current_exposure_abs = 156;

int g_last_ctrl_id = 0;
int g_last_ctrl_val = 0;

static jmethodID method_onStreamOn = NULL;
static jmethodID method_onStreamOff = NULL;
static jmethodID method_onDisconnect = NULL;
static jmethodID method_onSetControl = NULL;
static jmethodID method_onSelectInput = NULL;

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

JNIEXPORT void JNICALL Java_io_github_lumaparallax_vsouchi_v4l2_NativeBridge_nativeClearCameras(JNIEnv *env, jobject thiz) {
    v4l2_clear_cameras();
}

JNIEXPORT void JNICALL Java_io_github_lumaparallax_vsouchi_v4l2_NativeBridge_nativeAddCamera(
        JNIEnv *env, jobject thiz, jint index, jstring deviceName, jstring cameraId,
        jintArray formats, jintArray resFormats, jintArray resWs, jintArray resHs, jintArray resFps,
        jintArray aeFps,
        jint cfa, jboolean hasFocus, jfloat minFocus, jboolean hasExposure,
        jlong minExposure, jlong maxExposure, jint orientation, jint hflip) {

    const char *nameStr = (*env)->GetStringUTFChars(env, deviceName, 0);
    const char *idStr = (*env)->GetStringUTFChars(env, cameraId, 0);

    jsize fmtLen = (*env)->GetArrayLength(env, formats);
    jint *fmtBody = (*env)->GetIntArrayElements(env, formats, 0);

    jsize resLen = (*env)->GetArrayLength(env, resWs);
    jint *rf = (*env)->GetIntArrayElements(env, resFormats, 0);
    jint *rw = (*env)->GetIntArrayElements(env, resWs, 0);
    jint *rh = (*env)->GetIntArrayElements(env, resHs, 0);
    jint *rr = (*env)->GetIntArrayElements(env, resFps, 0);

    jsize aeFpsLen = (*env)->GetArrayLength(env, aeFps);
    jint *aeBody = (*env)->GetIntArrayElements(env, aeFps, 0);

    v4l2_add_camera((int)index, nameStr, idStr,
                    (int*)fmtBody, (int)fmtLen,
                    (int*)rf, (int*)rw, (int*)rh, (int*)rr, (int)resLen,
                    (int*)aeBody, (int)aeFpsLen,
                    (int)cfa, (int)hasFocus, (float)minFocus, (int)hasExposure,
                    (long)minExposure, (long)maxExposure, (int)orientation, (int)hflip);

    (*env)->ReleaseIntArrayElements(env, formats, fmtBody, JNI_ABORT);
    (*env)->ReleaseIntArrayElements(env, resFormats, rf, JNI_ABORT);
    (*env)->ReleaseIntArrayElements(env, resWs, rw, JNI_ABORT);
    (*env)->ReleaseIntArrayElements(env, resHs, rh, JNI_ABORT);
    (*env)->ReleaseIntArrayElements(env, resFps, rr, JNI_ABORT);
    (*env)->ReleaseIntArrayElements(env, aeFps, aeBody, JNI_ABORT);
    (*env)->ReleaseStringUTFChars(env, deviceName, nameStr);
    (*env)->ReleaseStringUTFChars(env, cameraId, idStr);
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
    method_onSelectInput = (*env)->GetMethodID(env, clazz, "onSelectInput", "(I)V");

    if (g_kernel_version == 0) {
        g_kernel_version = get_real_kernel_version();
    }

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
                                   v4l2_get_sensor_width(), v4l2_get_sensor_height(),
                                   v4l2_get_fps(), v4l2_get_android_format());
        } else if (signal == 2) {
            (*env)->CallVoidMethod(env, thiz, method_onStreamOff);
        } else if (signal == 3) {
            (*env)->CallVoidMethod(env, thiz, method_onDisconnect);
        } else if (signal == 4) {
            (*env)->CallVoidMethod(env, thiz, method_onSetControl, g_last_ctrl_id, g_last_ctrl_val);
        } else if (signal == 5) {
            (*env)->CallVoidMethod(env, thiz, method_onSelectInput, g_active_camera_idx);
        }

    }
}
