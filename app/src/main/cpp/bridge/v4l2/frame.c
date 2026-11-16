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
#include "bridge/v4l2/internal.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "vSouchi:BridgeFrame", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "vSouchi:BridgeFrame", __VA_ARGS__)

extern uint32_t g_frame_sequence;
extern int g_sensor_hflip;
extern int g_sensor_vflip;
static AImageReader* g_ndk_reader = NULL;
static uint8_t* g_temp_i420_buffer = NULL;
static uint32_t g_temp_i420_size = 0;

static inline void mirror_row_8(const uint8_t* src, uint8_t* dst, int width) {
    for (int i = 0; i < width; i++) {
        dst[i] = src[width - 1 - i];
    }
}

static inline void mirror_row_uv(const uint8_t* src, uint8_t* dst, int pair_count) {
    for (int i = 0; i < pair_count; i++) {
        int src_idx = (pair_count - 1 - i) * 2;
        int dst_idx = i * 2;
        dst[dst_idx]     = src[src_idx];
        dst[dst_idx + 1] = src[src_idx + 1];
    }
}

static void onImageAvailableCallback(void* context, AImageReader* reader) {
    AImage* image = NULL;
    media_status_t status = AImageReader_acquireLatestImage(reader, &image);
    if (status != AMEDIA_OK || image == NULL) return;

    /* Extract hardware start-of-exposure timestamp and increment monotonic sequence */
    int64_t raw_ts = 0;
    AImage_getTimestamp(image, &raw_ts);
    uint64_t ts_ns = (raw_ts > 0) ? (uint64_t)raw_ts : 0;
    uint32_t seq = __atomic_fetch_add(&g_frame_sequence, 1, __ATOMIC_RELAXED);

    int index = v4l2_get_queued_index();
    if (index == -1) {
        AImage_delete(image);
        return;
    }
    if (!v4l2_is_buffer_ready_for_write(index)) {
        v4l2_abort_write(index);
        AImage_delete(image);
        return;
    }

    uint8_t* payload = (uint8_t*)v4l2_get_buffer_ptr(index);
    if (!payload) {
        v4l2_abort_write(index);
        AImage_delete(image);
        return;
    }

    /* Snapshot stream parameters atomically so mid-frame reconfig cannot desync sizes */
    pthread_mutex_lock(&g_v4l2_mutex);
    int android_fmt = g_current_android_format;
    int target_w = (int)g_current_width;
    int target_h = (int)g_current_height;
    uint32_t fmt = g_current_format;
    int rotate = g_current_rotate;
    int hflip = g_sensor_hflip;
    int vflip = g_sensor_vflip;
    pthread_mutex_unlock(&g_v4l2_mutex);

    int is_rotated = (rotate == 90 || rotate == 270);
    uint32_t max_capacity = v4l2_get_buffer_size(index);

    if (android_fmt == 256 || android_fmt == 32 || android_fmt == 37 || android_fmt == 1212500294) {
        uint8_t* data = NULL;
        int data_length = 0;
        if (AImage_getPlaneData(image, 0, &data, &data_length) != AMEDIA_OK || !data || data_length <= 0) {
            v4l2_abort_write(index);
            AImage_delete(image);
            return;
        }

        uint32_t copy_sz = ((uint32_t)data_length < max_capacity) ? (uint32_t)data_length : max_capacity;
        memcpy(payload, data, copy_sz);
        v4l2_set_buffer_bytesused(index, copy_sz);

    } else {
        int32_t width = 0, height = 0;
        AImage_getWidth(image, &width);
        AImage_getHeight(image, &height);

        if (target_w <= 0 || target_h <= 0) {
            v4l2_abort_write(index);
            AImage_delete(image);
            return;
        }

        int expected_w = is_rotated ? target_h : target_w;
        int expected_h = is_rotated ? target_w : target_h;
        uint32_t req_out_sz = (fmt == v4l2_fourcc('Y', 'U', 'Y', 'V'))
                ? (uint32_t)(target_w * target_h * 2)
                : (uint32_t)((target_w * target_h * 3) / 2);

        if (width != expected_w || height != expected_h || max_capacity < req_out_sz) {
            v4l2_abort_write(index);
            AImage_delete(image);
            return;
        }

        uint8_t *y_data = NULL, *u_data = NULL, *v_data = NULL;
        int y_len = 0, u_len = 0, v_len = 0;
        if (AImage_getPlaneData(image, 0, &y_data, &y_len) != AMEDIA_OK ||
            AImage_getPlaneData(image, 1, &u_data, &u_len) != AMEDIA_OK ||
            AImage_getPlaneData(image, 2, &v_data, &v_len) != AMEDIA_OK ||
            !y_data || !u_data || !v_data) {
            v4l2_abort_write(index);
            AImage_delete(image);
            return;
        }

        int y_stride = 0, uv_stride = 0, uv_pixel_stride = 0;
        AImage_getPlaneRowStride(image, 0, &y_stride);
        AImage_getPlaneRowStride(image, 2, &uv_stride);
        AImage_getPlanePixelStride(image, 2, &uv_pixel_stride);

        int eff_hflip = hflip ^ (rotate == 180);
        int eff_vflip = vflip ^ (rotate == 180);

        int is_nv12 = (fmt == v4l2_fourcc('N', 'V', '1', '2'));
        int is_nv21 = (fmt == v4l2_fourcc('N', 'V', '2', '1'));

        /* Check if hardware plane layout natively matches requested semi-planar NV12/NV21 (not applicable if dimensions are transposed) */
        int match_nv12 = (!is_rotated && is_nv12 &&
                          uv_pixel_stride == 2 && u_data == v_data - 1);
        int match_nv21 = (!is_rotated && is_nv21 &&
                          uv_pixel_stride == 2 && v_data == u_data - 1);

        if (match_nv12 || match_nv21) {
            uint32_t total_sz = (uint32_t)((width * height * 3) / 2);

            if (!eff_hflip && !eff_vflip) {
                /* Copy Y and UV planes separately to handle Gralloc vertical slice-height / page gaps */
                const uint8_t* src_uv = match_nv12 ? u_data : v_data;
                CopyPlane(y_data, y_stride, payload, width, width, height);
                CopyPlane(src_uv, uv_stride, payload + (width * height), width, width, height / 2);
            } else {
                /* Single-pass row unpadding with combined HFLIP / VFLIP */
                uint8_t* dst_y = payload;
                for (int r = 0; r < height; r++) {
                    int dst_r = eff_vflip ? (height - 1 - r) : r;
                    uint8_t* dst_line = dst_y + (dst_r * width);
                    const uint8_t* src_line = y_data + (r * y_stride);

                    if (eff_hflip) {
                        mirror_row_8(src_line, dst_line, width);
                    } else {
                        memcpy(dst_line, src_line, width);
                    }
                }

                uint8_t* dst_uv = payload + (width * height);
                const uint8_t* src_uv = match_nv12 ? u_data : v_data;
                for (int r = 0; r < height / 2; r++) {
                    int dst_r = eff_vflip ? (height / 2 - 1 - r) : r;
                    uint8_t* dst_line = dst_uv + (dst_r * width);
                    const uint8_t* src_line = src_uv + (r * uv_stride);

                    if (eff_hflip) {
                        mirror_row_uv(src_line, dst_line, width / 2);
                    } else {
                        memcpy(dst_line, src_line, width);
                    }
                }
            }
            v4l2_set_buffer_bytesused(index, total_sz);
        } else {
            uint32_t base_size = (width * height * 3) / 2;
            uint32_t num_buffers = 1;
            if (is_rotated) {
                num_buffers = (hflip || vflip) ? 3 : 2;
            } else if (eff_hflip || eff_vflip) {
                num_buffers = 2;
            }
            uint32_t req_size = base_size * num_buffers;

            if (g_temp_i420_size < req_size) {
                uint8_t* new_buf = (uint8_t*)malloc(req_size);
                if (!new_buf) {
                    v4l2_abort_write(index);
                    AImage_delete(image);
                    return;
                }
                free(g_temp_i420_buffer);
                g_temp_i420_buffer = new_buf;
                g_temp_i420_size = req_size;
            }

            uint8_t* temp_y = g_temp_i420_buffer;
            uint8_t* temp_u = temp_y + (width * height);
            uint8_t* temp_v = temp_u + ((width * height) / 4);

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

            uint8_t *src_y = temp_y, *src_u = temp_u, *src_v = temp_v;

            if (is_rotated) {
                uint8_t* rot_y = g_temp_i420_buffer + base_size;
                uint8_t* rot_u = rot_y + (target_w * target_h);
                uint8_t* rot_v = rot_u + ((target_w * target_h) / 4);

                enum RotationMode rot_mode = (rotate == 90) ? kRotate90 : kRotate270;
                I420Rotate(temp_y, width,
                           temp_u, width / 2,
                           temp_v, width / 2,
                           rot_y, target_w,
                           rot_u, target_w / 2,
                           rot_v, target_w / 2,
                           width, height,
                           rot_mode);

                src_y = rot_y;
                src_u = rot_u;
                src_v = rot_v;

                if (hflip || vflip) {
                    uint8_t* flip_y = g_temp_i420_buffer + (base_size * 2);
                    uint8_t* flip_u = flip_y + (target_w * target_h);
                    uint8_t* flip_v = flip_u + ((target_w * target_h) / 4);

                    int copy_height = vflip ? -target_h : target_h;

                    if (hflip) {
                        I420Mirror(rot_y, target_w,
                                   rot_u, target_w / 2,
                                   rot_v, target_w / 2,
                                   flip_y, target_w,
                                   flip_u, target_w / 2,
                                   flip_v, target_w / 2,
                                   target_w, copy_height);
                    } else {
                        I420Copy(rot_y, target_w,
                                 rot_u, target_w / 2,
                                 rot_v, target_w / 2,
                                 flip_y, target_w,
                                 flip_u, target_w / 2,
                                 flip_v, target_w / 2,
                                 target_w, copy_height);
                    }

                    src_y = flip_y;
                    src_u = flip_u;
                    src_v = flip_v;
                }
            } else if (eff_hflip || eff_vflip) {
                uint8_t* flip_y = g_temp_i420_buffer + base_size;
                uint8_t* flip_u = flip_y + (target_w * target_h);
                uint8_t* flip_v = flip_u + ((target_w * target_h) / 4);

                int copy_height = eff_vflip ? -target_h : target_h;

                if (eff_hflip) {
                    /* I420Mirror accepts a negative height for simultaneous VFLIP */
                    I420Mirror(temp_y, target_w,
                               temp_u, target_w / 2,
                               temp_v, target_w / 2,
                               flip_y, target_w,
                               flip_u, target_w / 2,
                               flip_v, target_w / 2,
                               target_w, copy_height);
                } else {
                    /* VFLIP only: I420Copy with negative height inverts the image */
                    I420Copy(temp_y, target_w,
                             temp_u, target_w / 2,
                             temp_v, target_w / 2,
                             flip_y, target_w,
                             flip_u, target_w / 2,
                             flip_v, target_w / 2,
                             target_w, copy_height);
                }

                src_y = flip_y;
                src_u = flip_u;
                src_v = flip_v;
            }

            if (fmt == v4l2_fourcc('Y', 'U', 'Y', 'V')) {
                I420ToYUY2(src_y, target_w, src_u, target_w / 2, src_v, target_w / 2,
                           payload, target_w * 2, target_w, target_h);
                v4l2_set_buffer_bytesused(index, target_w * target_h * 2);
            }
            else if (fmt == v4l2_fourcc('N', 'V', '1', '2')) {
                I420ToNV12(src_y, target_w, src_u, target_w / 2, src_v, target_w / 2,
                           payload, target_w, payload + (target_w * target_h), target_w,
                           target_w, target_h);
                v4l2_set_buffer_bytesused(index, (target_w * target_h * 3) / 2);
            }
            else if (fmt == v4l2_fourcc('N', 'V', '2', '1')) {
                I420ToNV21(src_y, target_w, src_u, target_w / 2, src_v, target_w / 2,
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

                I420Copy(src_y, target_w, src_u, target_w / 2, src_v, target_w / 2,
                         out_y, target_w, out_u, target_w / 2, out_v, target_w / 2,
                         target_w, target_h);
                v4l2_set_buffer_bytesused(index, (target_w * target_h * 3) / 2);
            }
            else {
                v4l2_abort_write(index);
                AImage_delete(image);
                return;
            }
        }
    }

    AImage_delete(image);
    v4l2_set_buffer_meta(index, seq, ts_ns);
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
    if (g_temp_i420_buffer != NULL) {
        free(g_temp_i420_buffer);
        g_temp_i420_buffer = NULL;
        g_temp_i420_size = 0;
    }
}
