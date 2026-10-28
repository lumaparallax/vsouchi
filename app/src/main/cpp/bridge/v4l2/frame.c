/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Isolates NDK Hardware Buffer extraction and libyuv pixel translation.
#define _GNU_SOURCE
#include <jni.h>
#include <string.h>
#include <stdlib.h>
#include <android/log.h>
#include <linux/videodev2.h>
#include "libyuv.h"
#include <media/NdkImageReader.h>
#include <media/NdkImage.h>
#include <android/hardware_buffer.h>
#include <android/native_window_jni.h>
#include "bridge/v4l2/v4l2.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "vSouchi:BridgeFrame", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "vSouchi:BridgeFrame", __VA_ARGS__)

static AImageReader* g_ndk_reader = NULL;
static uint8_t* g_temp_i420_buffer = NULL;
static uint32_t g_temp_i420_size = 0;

static void onImageAvailableCallback(void* context, AImageReader* reader) {
    AImage* image = NULL;
    media_status_t status = AImageReader_acquireLatestImage(reader, &image);
    if (status != AMEDIA_OK || image == NULL) return;

    int index = v4l2_get_queued_index();
    if (index == -1 || !v4l2_is_buffer_ready_for_write(index)) { 
        AImage_delete(image); 
        return; 
    }

    uint8_t* payload = (uint8_t*)v4l2_get_buffer_ptr(index);
    if (!payload) { 
        AImage_delete(image); 
        return; 
    }

    int android_fmt = v4l2_get_android_format();

    if (android_fmt == 256 || android_fmt == 32 || android_fmt == 37 || android_fmt == 1212500294) {
        uint8_t* data = NULL;
        int data_length = 0;
        AImage_getPlaneData(image, 0, &data, &data_length);
        
        uint32_t max_capacity = v4l2_get_buffer_size(index);
        uint32_t copy_sz = (data_length < max_capacity) ? (uint32_t)data_length : max_capacity;
        
        memcpy(payload, data, copy_sz);
        v4l2_set_buffer_bytesused(index, copy_sz);

    } else {
        int32_t width, height;
        AImage_getWidth(image, &width);
        AImage_getHeight(image, &height);

        uint8_t *y_data, *u_data, *v_data;
        int y_len, u_len, v_len;
        AImage_getPlaneData(image, 0, &y_data, &y_len);
        AImage_getPlaneData(image, 1, &u_data, &u_len);
        AImage_getPlaneData(image, 2, &v_data, &v_len);

        int y_stride, uv_stride, uv_pixel_stride;
        AImage_getPlaneRowStride(image, 0, &y_stride);
        AImage_getPlaneRowStride(image, 2, &uv_stride);
        AImage_getPlanePixelStride(image, 2, &uv_pixel_stride);

        int target_w = v4l2_get_width();
        int target_h = v4l2_get_height();
        uint32_t fmt = v4l2_get_current_format();

        int is_nv12 = (fmt == v4l2_fourcc('N', 'V', '1', '2'));
        int is_nv21 = (fmt == v4l2_fourcc('N', 'V', '2', '1'));

        // Zero-Padding Contiguous NV12 / NV21 direct from GPU optimization
        int direct_nv12 = (is_nv12 && uv_pixel_stride == 2 && uv_stride == width && y_stride == width && u_data == v_data - 1);
        int direct_nv21 = (is_nv21 && uv_pixel_stride == 2 && uv_stride == width && y_stride == width && v_data == u_data - 1);

        if (direct_nv12 || direct_nv21) {
            uint32_t total_sz = (width * height * 3) / 2;
            memcpy(payload, y_data, total_sz);
            v4l2_set_buffer_bytesused(index, total_sz);
        } else {
            uint32_t req_size = (width * height * 3) / 2;
            if (g_temp_i420_size < req_size) {
                if (g_temp_i420_buffer) free(g_temp_i420_buffer);
                g_temp_i420_buffer = (uint8_t*)malloc(req_size);
                g_temp_i420_size = req_size;
            }

            uint8_t* temp_y = g_temp_i420_buffer;
            uint8_t* temp_u = temp_y + (width * height);
            uint8_t* temp_v = temp_u + ((width * height) / 4);

            // Let libyuv untangle the Android multi-plane buffer into I420
            Android420ToI420(
                y_data, y_stride,
                u_data, uv_stride,
                v_data, uv_stride,
                uv_pixel_stride,
                temp_y, width,
                temp_u, width / 2,
                temp_v, width / 2,
                width, height
            );

            // Pack the I420 intermediate buffer into the specific V4L2 requested format
            if (fmt == v4l2_fourcc('Y', 'U', 'Y', 'V')) {
                I420ToYUY2(temp_y, width, temp_u, width / 2, temp_v, width / 2,
                           payload, target_w * 2, target_w, target_h);
                v4l2_set_buffer_bytesused(index, target_w * target_h * 2);
            } 
            else if (fmt == v4l2_fourcc('N', 'V', '1', '2')) {
                I420ToNV12(temp_y, width, temp_u, width / 2, temp_v, width / 2,
                           payload, target_w, payload + (target_w * target_h), target_w,
                           target_w, target_h);
                v4l2_set_buffer_bytesused(index, (target_w * target_h * 3) / 2);
            }
            else if (fmt == v4l2_fourcc('N', 'V', '2', '1')) {
                I420ToNV21(temp_y, width, temp_u, width / 2, temp_v, width / 2,
                           payload, target_w, payload + (target_w * target_h), target_w,
                           target_w, target_h);
                v4l2_set_buffer_bytesused(index, (target_w * target_h * 3) / 2);
            }
            else if (fmt == v4l2_fourcc('Y', 'U', '1', '2') || fmt == v4l2_fourcc('Y', 'V', '1', '2')) {
                uint8_t* out_y = payload;
                uint8_t* out_u = payload + (target_w * target_h);
                uint8_t* out_v = out_u + ((target_w * target_h) / 4);
                
                if (fmt == v4l2_fourcc('Y', 'V', '1', '2')) {
                    uint8_t* swap = out_u; out_u = out_v; out_v = swap;
                }

                I420Copy(temp_y, width, temp_u, width / 2, temp_v, width / 2,
                         out_y, target_w, out_u, target_w / 2, out_v, target_w / 2,
                         target_w, target_h);
                v4l2_set_buffer_bytesused(index, (target_w * target_h * 3) / 2);
            }
        }
    }

    AImage_delete(image);
    v4l2_push_filled_buffer(index);
}

JNIEXPORT jobject JNICALL Java_io_github_lumaparallax_vsouchi_v4l2_NativeBridge_nativeCreateImageReader(JNIEnv *env, jobject thiz, jint width, jint height, jint format, jint max_images) {
    if (g_ndk_reader != NULL) {
        AImageReader_delete(g_ndk_reader);
        g_ndk_reader = NULL;
    }

    // AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN ensures the GPU allocator avoids tiled formats
    // so we can read the memory linearly without costly format conversions on the CPU.
    uint64_t usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
    
    media_status_t status = AImageReader_newWithUsage(width, height, format, usage, max_images, &g_ndk_reader);
    if (status != AMEDIA_OK || g_ndk_reader == NULL) {
        LOGE("AImageReader allocation failed! Status: %d", status);
        return NULL;
    }

    AImageReader_ImageListener listener;
    listener.context = NULL;
    listener.onImageAvailable = onImageAvailableCallback;
    AImageReader_setImageListener(g_ndk_reader, &listener);

    ANativeWindow* native_window = NULL;
    AImageReader_getWindow(g_ndk_reader, &native_window);
    
    if (native_window == NULL) {
        LOGE("Failed to get ANativeWindow from AImageReader!");
        return NULL;
    }

    return ANativeWindow_toSurface(env, native_window);
}

JNIEXPORT void JNICALL Java_io_github_lumaparallax_vsouchi_v4l2_NativeBridge_nativeDestroyImageReader(JNIEnv *env, jobject thiz) {
    if (g_ndk_reader != NULL) {
        AImageReader_delete(g_ndk_reader);
        g_ndk_reader = NULL;
    }
}
