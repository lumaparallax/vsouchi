/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Core V4L2 state machine, memory provisioning, and lock-free DQBUF threading.
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/memfd.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <sys/uio.h>
#include <time.h>
#include <dlfcn.h>
#include <errno.h>
#include <android/log.h>

#include "bridge/v4l2/v4l2.h"
#include "bridge/v4l2/internal.h"

#define TAG "vSouchi:V4L2State"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#ifndef V4L2_PIX_FMT_MJPEG
#define V4L2_PIX_FMT_MJPEG v4l2_fourcc('M', 'J', 'P', 'G')
#endif
#ifndef V4L2_PIX_FMT_SRGGB16
#define V4L2_PIX_FMT_SRGGB16 v4l2_fourcc('R', 'G', '1', '6')
#endif
#ifndef V4L2_FMT_FLAG_COMPRESSED
#define V4L2_FMT_FLAG_COMPRESSED 0x0001
#endif

static FormatMap kMasterFormats[] = {
    { v4l2_fourcc('N', 'V', '1', '2'), "Y/UV 4:2:0",                 35,  0 },
    { v4l2_fourcc('Y', 'U', 'Y', 'V'), "YUYV 4:2:2",                 35,  0 },
    { v4l2_fourcc('N', 'V', '2', '1'), "Y/VU 4:2:0",                 35,  0 },
    { v4l2_fourcc('Y', 'U', '1', '2'), "Planar YUV 4:2:0",          35,  0 },
    { v4l2_fourcc('Y', 'V', '1', '2'), "Planar YVU 4:2:0",          35,  0 },
    { V4L2_PIX_FMT_MJPEG,              "Motion-JPEG",               256,  V4L2_FMT_FLAG_COMPRESSED },
    { V4L2_PIX_FMT_SRGGB16,            "16-bit Bayer RGRG/GBGB",     32,  0 }
};

CameraProfile g_cameras[MAX_CAMERAS];
int g_camera_count = 0;
int g_active_camera_idx = 0;

extern int g_has_focus, g_has_exposure;
extern float g_min_focus_dist;
extern long g_min_exposure, g_max_exposure;
extern int g_sensor_orientation, g_sensor_hflip, g_sensor_vflip;

#define MASTER_FORMAT_COUNT (sizeof(kMasterFormats) / sizeof(kMasterFormats[0]))

FormatMap g_supported_formats[32];
int g_supported_format_count = 0;

ResolutionMap g_resolutions[256];
int g_res_count = 0;

uint32_t g_current_width = 1920;
uint32_t g_current_height = 1080;
uint32_t g_current_fps = 30;
uint32_t g_current_format = v4l2_fourcc('N', 'V', '1', '2');
int g_current_android_format = 35;
int g_current_rotate = 0;

int g_buffer_fds[MAX_BUFFERS] = {-1, -1, -1, -1, -1, -1, -1, -1};
uint32_t g_buffer_lengths[MAX_BUFFERS] = {0};
void* g_buffer_ptrs[MAX_BUFFERS] = {NULL};
uint32_t g_buffer_count = 0;
uint32_t g_buffer_bytesused[MAX_BUFFERS] = {0};
uint32_t g_buffer_sequence[MAX_BUFFERS] = {0};
uint64_t g_buffer_timestamp[MAX_BUFFERS] = {0};
uint32_t g_frame_sequence = 0;
int g_buffer_state[MAX_BUFFERS] = {0};
int g_event_fd = -1;

volatile int g_is_streaming = 0;
pthread_mutex_t g_v4l2_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  g_v4l2_cond  = PTHREAD_COND_INITIALIZER;

static int s_next_write_idx = 0;
static int s_next_read_idx = 0;

static pthread_mutex_t g_bridge_tx_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int socket_fd;
    uint32_t client_fd;
    IpcV4l2Buffer buf;
} DqbufArgs;

#define DQ_QUEUE_SIZE 16
static DqbufArgs g_dq_queue[DQ_QUEUE_SIZE];
static int g_dq_head = 0;
static int g_dq_tail = 0;
static int g_dq_count = 0;

static pthread_mutex_t g_dq_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_dq_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_worker_tid;
static volatile int g_worker_running = 0;

uint32_t get_correct_colorspace(uint32_t width, uint32_t height) {
    if (width >= 1280 || height >= 720) return V4L2_COLORSPACE_REC709;
    return V4L2_COLORSPACE_SMPTE170M;
}

uint32_t v4l2_get_current_frame_size() {
    if (g_current_format == V4L2_PIX_FMT_MJPEG) {
        return g_current_width * g_current_height * 2;
    } else if (g_current_format == v4l2_fourcc('R', 'G', '1', '6') ||
               g_current_format == v4l2_fourcc('G', 'R', '1', '6') ||
               g_current_format == v4l2_fourcc('G', 'B', '1', '6') ||
               g_current_format == v4l2_fourcc('B', 'G', '1', '6') ||
               g_current_format == v4l2_fourcc('Y', 'U', 'Y', 'V')) {
        return g_current_width * g_current_height * 2;
    }
    return (g_current_width * g_current_height * 3) / 2;
}

uint32_t get_max_fps_for_res(uint32_t w, uint32_t h) {
    uint32_t sw = is_rotated_90_270() ? h : w;
    uint32_t sh = is_rotated_90_270() ? w : h;
    for (int i = 0; i < g_res_count; i++) {
        if (g_resolutions[i].android_format == g_current_android_format &&
            g_resolutions[i].width == sw &&
            g_resolutions[i].height == sh) {
            return g_resolutions[i].max_fps;
        }
    }
    return 30;
}

void v4l2_clear_cameras(void) {
    pthread_mutex_lock(&g_v4l2_mutex);
    g_camera_count = 0;
    g_active_camera_idx = 0;
    pthread_mutex_unlock(&g_v4l2_mutex);
}

int v4l2_switch_camera(int index) {
    pthread_mutex_lock(&g_v4l2_mutex);
    if (index < 0 || index >= g_camera_count) {
        pthread_mutex_unlock(&g_v4l2_mutex);
        return -1;
    }

    g_active_camera_idx = index;
    CameraProfile *cam = &g_cameras[index];

    snprintf(g_device_name, sizeof(g_device_name), "%s", cam->name);
    snprintf(g_bus_info, sizeof(g_bus_info), "%s", cam->bus_info);

    g_supported_format_count = cam->supported_format_count;
    memcpy(g_supported_formats, cam->supported_formats, sizeof(FormatMap) * cam->supported_format_count);

    g_res_count = cam->res_count;
    memcpy(g_resolutions, cam->resolutions, sizeof(ResolutionMap) * cam->res_count);

    g_has_focus = cam->has_focus;
    g_min_focus_dist = cam->min_focus_dist;
    g_has_exposure = cam->has_exposure;
    g_min_exposure = cam->min_exposure;
    g_max_exposure = cam->max_exposure;
    g_sensor_orientation = cam->orientation;
    g_sensor_hflip = cam->hflip;
    g_sensor_vflip = 0;
    g_current_rotate = 0;

    /* Re-negotiate default resolution for the selected sensor */
    if (g_supported_format_count > 0) {
        g_current_format = g_supported_formats[0].pixelformat;
        g_current_android_format = g_supported_formats[0].android_format;
    } else {
        g_current_format = v4l2_fourcc('N', 'V', '1', '2');
        g_current_android_format = 35;
    }

    int target_idx = -1;
    for (int i = 0; i < g_res_count; ++i) {
        if (g_resolutions[i].android_format == g_current_android_format &&
            g_resolutions[i].width == 1920 && g_resolutions[i].height == 1080) {
            target_idx = i;
            break;
        }
    }
    if (target_idx == -1) {
        for (int i = 0; i < g_res_count; ++i) {
            if (g_resolutions[i].android_format == g_current_android_format &&
                g_resolutions[i].width == 1280 && g_resolutions[i].height == 720) {
                target_idx = i;
                break;
            }
        }
    }
    if (target_idx == -1) {
        for (int i = 0; i < g_res_count; ++i) {
            if (g_resolutions[i].android_format == g_current_android_format) {
                target_idx = i;
                break;
            }
        }
    }
    if (target_idx == -1) target_idx = 0;

    if (g_res_count > 0) {
        g_current_width = g_resolutions[target_idx].width;
        g_current_height = g_resolutions[target_idx].height;
        g_current_fps = get_max_fps_for_res(g_current_width, g_current_height);
    }

    pthread_mutex_unlock(&g_v4l2_mutex);
    return 0;
}

void v4l2_add_camera(int index, const char* name, const char* id,
                     int* formats, int fmt_count,
                     int* res_fmts, int* res_ws, int* res_hs, int* res_fps, int res_count,
                     int* ae_fps, int ae_fps_count,
                     int cfa, int has_focus, float min_focus, int has_exp,
                     long min_exp, long max_exp, int orientation, int hflip) {
    if (index < 0 || index >= MAX_CAMERAS) return;

    pthread_mutex_lock(&g_v4l2_mutex);
    CameraProfile *cam = &g_cameras[index];
    memset(cam, 0, sizeof(CameraProfile));

    snprintf(cam->name, sizeof(cam->name), "%s", name);
    snprintf(cam->camera_id, sizeof(cam->camera_id), "%s", id);
    snprintf(cam->bus_info, sizeof(cam->bus_info), "platform:vsouchi-cam%s", id);

    uint32_t bayer_fourcc = V4L2_PIX_FMT_SRGGB16;
    const char *bayer_desc = "16-bit Bayer RGRG/GBGB";
    if (cfa == 1) { bayer_fourcc = v4l2_fourcc('G', 'R', '1', '6'); bayer_desc = "16-bit Bayer GRGR/BGBG"; }
    else if (cfa == 2) { bayer_fourcc = v4l2_fourcc('G', 'B', '1', '6'); bayer_desc = "16-bit Bayer GBGB/RGRG"; }
    else if (cfa == 3) { bayer_fourcc = v4l2_fourcc('B', 'G', '1', '6'); bayer_desc = "16-bit Bayer BGBG/GRGR"; }

    cam->supported_format_count = 0;
    for (size_t m = 0; m < MASTER_FORMAT_COUNT; ++m) {
        int req_android_fmt = kMasterFormats[m].android_format;
        int is_supported = 0;

        for (int i = 0; i < fmt_count; ++i) {
            if (formats[i] == req_android_fmt) {
                is_supported = 1;
                break;
            }
        }

        if (is_supported && cam->supported_format_count < 32) {
            FormatMap fmt = kMasterFormats[m];
            if (req_android_fmt == 32) {
                fmt.pixelformat = bayer_fourcc;
                snprintf(fmt.description, sizeof(fmt.description), "%s", bayer_desc);
            }
            cam->supported_formats[cam->supported_format_count++] = fmt;
        }
    }

    cam->res_count = (res_count > 256) ? 256 : res_count;
    for (int i = 0; i < cam->res_count; i++) {
        cam->resolutions[i].android_format = res_fmts[i];
        cam->resolutions[i].width = (uint32_t)res_ws[i];
        cam->resolutions[i].height = (uint32_t)res_hs[i];
        uint32_t max_f = (uint32_t)res_fps[i];
        cam->resolutions[i].max_fps = max_f;

        uint32_t count = 0;

        /* If max_fps is not in ae_fps, insert it first */
        int has_exact_max = 0;
        for (int f = 0; f < ae_fps_count; f++) {
            if ((uint32_t)ae_fps[f] == max_f) {
                has_exact_max = 1;
                break;
            }
        }
        if (!has_exact_max && max_f > 0 && count < MAX_FPS_PER_RES) {
            cam->resolutions[i].fps_list[count++] = max_f;
        }

        for (int f = 0; f < ae_fps_count && count < MAX_FPS_PER_RES; f++) {
            uint32_t rate = (uint32_t)ae_fps[f];
            if (rate <= max_f && rate > 0) {
                if (rate != max_f) {
                    cam->resolutions[i].fps_list[count++] = rate;
                } else if (has_exact_max && count == 0) {
                    cam->resolutions[i].fps_list[count++] = rate;
                }
            }
        }

        if (count == 0) {
            cam->resolutions[i].fps_list[count++] = (max_f > 0) ? max_f : 30;
        }
        cam->resolutions[i].fps_count = count;
    }

    cam->cfa_pattern = cfa;
    cam->has_focus = has_focus;
    cam->min_focus_dist = min_focus;
    cam->has_exposure = has_exp;
    cam->min_exposure = min_exp;
    cam->max_exposure = max_exp;
    cam->orientation = orientation;
    cam->hflip = hflip;

    if (index >= g_camera_count) {
        g_camera_count = index + 1;
    }
    pthread_mutex_unlock(&g_v4l2_mutex);

    /* Initialize camera 0 as default active sensor */
    if (index == 0) {
        v4l2_switch_camera(0);
    }
}

int v4l2_get_android_format() { return g_current_android_format; }
int v4l2_get_width() { return g_current_width; }
int v4l2_get_height() { return g_current_height; }
int v4l2_get_sensor_width() { return is_rotated_90_270() ? g_current_height : g_current_width; }
int v4l2_get_sensor_height() { return is_rotated_90_270() ? g_current_width : g_current_height; }
uint32_t v4l2_get_current_format() { return g_current_format; }
int v4l2_get_fps() { return g_current_fps; }

void v4l2_send_reply(int socket_fd, uint32_t client_fd, int32_t status, void* payload, uint32_t sz) {
    IpcInboundHeader hdr = {
        .client_fd = client_fd,
        .payload_size = sz,
        .ipc_status = status
    };

    struct iovec iov[2];
    iov[0].iov_base = &hdr; iov[0].iov_len = sizeof(IpcInboundHeader);
    int count = 1;

    if (payload && sz > 0) {
        iov[1].iov_base = payload; iov[1].iov_len = sz;
        count = 2;
    }

    pthread_mutex_lock(&g_bridge_tx_mutex);
    writev(socket_fd, iov, count);
    pthread_mutex_unlock(&g_bridge_tx_mutex);
}

void v4l2_send_error(int socket_fd, uint32_t client_fd, int32_t error_code) {
    v4l2_send_reply(socket_fd, client_fd, -error_code, NULL, 0);
}

static int g_in_flight_writes = 0;
static pthread_cond_t g_write_done_cond = PTHREAD_COND_INITIALIZER;

void v4l2_free_buffers() {
    pthread_mutex_lock(&g_v4l2_mutex);
    /* Drain in-flight frame conversions before unmapping memory */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 50000000; /* 50ms cap */
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000;
    }
    while (g_in_flight_writes > 0) {
        if (pthread_cond_timedwait(&g_write_done_cond, &g_v4l2_mutex, &ts) != 0) {
            break;
        }
    }

    if (g_event_fd >= 0) {
        close(g_event_fd);
        g_event_fd = -1;
    }
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (g_buffer_ptrs[i] != NULL && g_buffer_ptrs[i] != MAP_FAILED) {
            munmap(g_buffer_ptrs[i], g_buffer_lengths[i]);
            g_buffer_ptrs[i] = NULL;
        }

        if (g_buffer_fds[i] >= 0) {
            close(g_buffer_fds[i]);
            g_buffer_fds[i] = -1;
        }

        g_buffer_lengths[i] = 0;
        g_buffer_bytesused[i] = 0;
        g_buffer_sequence[i] = 0;
        g_buffer_timestamp[i] = 0;
        g_buffer_state[i] = 0;
    }
    g_buffer_count = 0;
    s_next_write_idx = 0;
    s_next_read_idx = 0;
    pthread_mutex_unlock(&g_v4l2_mutex);
}

void v4l2_cleanup() {
    g_is_streaming = 0;
    if (g_event_fd >= 0) {
        close(g_event_fd);
        g_event_fd = -1;
    }

    pthread_mutex_lock(&g_v4l2_mutex);
    pthread_cond_broadcast(&g_v4l2_cond);
    pthread_mutex_unlock(&g_v4l2_mutex);
}

static void* persistent_dqbuf_worker(void* arg) {
    while (1) {
        pthread_mutex_lock(&g_dq_mutex);
        while (g_dq_count == 0 && g_worker_running) {
            pthread_cond_wait(&g_dq_cond, &g_dq_mutex);
        }

        if (!g_worker_running && g_dq_count == 0) {
            pthread_mutex_unlock(&g_dq_mutex);
            break;
        }

        DqbufArgs req = g_dq_queue[g_dq_head];
        g_dq_head = (g_dq_head + 1) % DQ_QUEUE_SIZE;
        g_dq_count--;
        pthread_mutex_unlock(&g_dq_mutex);

        pthread_mutex_lock(&g_v4l2_mutex);
        int idx = -1;
        while (g_is_streaming) {
            for (int i = 0; i < (int)g_buffer_count; i++) {
                int candidate = (s_next_read_idx + i) % (int)g_buffer_count;
                if (g_buffer_state[candidate] == 2) {
                    idx = candidate;
                    s_next_read_idx = (candidate + 1) % (int)g_buffer_count;
                    break;
                }
            }
            if (idx != -1) break;
            pthread_cond_wait(&g_v4l2_cond, &g_v4l2_mutex);
        }

        if (idx != -1) {
            g_buffer_state[idx] = 0;
            req.buf.index = idx;
            req.buf.bytesused = g_buffer_bytesused[idx];
            req.buf.length = g_buffer_lengths[idx];
            req.buf.flags = V4L2_BUF_FLAG_MAPPED | V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
            req.buf.sequence = g_buffer_sequence[idx];
            req.buf.timestamp_ns = g_buffer_timestamp[idx];
        } else {
            req.buf.length = 0xFFFFFFFF;
        }
        pthread_mutex_unlock(&g_v4l2_mutex);

        v4l2_send_reply(req.socket_fd, req.client_fd, 0, &req.buf, sizeof(IpcV4l2Buffer));
    }
    return NULL;
}

void v4l2_queue_dqbuf_request(int socket_fd, uint32_t client_fd, IpcV4l2Buffer* buf) {
    int overflow = 0;
    pthread_mutex_lock(&g_dq_mutex);
    if (g_dq_count < DQ_QUEUE_SIZE) {
        g_dq_queue[g_dq_tail].socket_fd = socket_fd;
        g_dq_queue[g_dq_tail].client_fd = client_fd;
        g_dq_queue[g_dq_tail].buf = *buf;

        g_dq_tail = (g_dq_tail + 1) % DQ_QUEUE_SIZE;
        g_dq_count++;
        pthread_cond_signal(&g_dq_cond);
    } else {
        LOGE("CRITICAL: DQBUF Queue Overflow! App is requesting frames too fast.");
        overflow = 1;
    }
    pthread_mutex_unlock(&g_dq_mutex);

    if (overflow) {
        v4l2_send_error(socket_fd, client_fd, EBUSY);
    }
}

void v4l2_start_worker() {
    if (!g_worker_running) {
        g_worker_running = 1;
        g_dq_head = 0; g_dq_tail = 0; g_dq_count = 0;
        pthread_create(&g_worker_tid, NULL, persistent_dqbuf_worker, NULL);
    }
}

void v4l2_stop_worker() {
    if (g_worker_running) {
        pthread_mutex_lock(&g_dq_mutex);
        g_worker_running = 0;
        pthread_cond_broadcast(&g_dq_cond);
        pthread_mutex_unlock(&g_dq_mutex);
        pthread_join(g_worker_tid, NULL);
    }
}

int v4l2_is_buffer_ready_for_write(int index) {
    int ready = 0;
    if (index < 0 || index >= (int)g_buffer_count) return 0;

    pthread_mutex_lock(&g_v4l2_mutex);
    /* State 3 represents an active write lease */
    if (g_buffer_state[index] == 3 && g_is_streaming) ready = 1;
    pthread_mutex_unlock(&g_v4l2_mutex);

    return ready;
}

void* v4l2_get_buffer_ptr(int index) {
    if (index >= 0 && index < (int)g_buffer_count) return g_buffer_ptrs[index];
    return NULL;
}

uint32_t v4l2_get_buffer_size(int index) {
    return (index >= 0 && index < (int)g_buffer_count) ? g_buffer_lengths[index] : 0;
}

int v4l2_get_queued_index() {
    pthread_mutex_lock(&g_v4l2_mutex);
    if (!g_is_streaming) {
        pthread_mutex_unlock(&g_v4l2_mutex);
        return -1;
    }
    for (int i = 0; i < (int)g_buffer_count; i++) {
        int candidate = (s_next_write_idx + i) % (int)g_buffer_count;
        if (g_buffer_state[candidate] == 1) {
            s_next_write_idx = (candidate + 1) % (int)g_buffer_count;
            g_buffer_state[candidate] = 3;
            g_in_flight_writes++;
            pthread_mutex_unlock(&g_v4l2_mutex);
            return candidate;
        }
    }
    pthread_mutex_unlock(&g_v4l2_mutex);
    return -1;
}

void v4l2_push_filled_buffer(int index) {
    pthread_mutex_lock(&g_v4l2_mutex);
    if (index >= 0 && index < (int)g_buffer_count) {
        if (g_is_streaming) {
            g_buffer_state[index] = 2;
            if (g_event_fd >= 0) {
                uint64_t val = 1;
                write(g_event_fd, &val, sizeof(val));
            }
            pthread_cond_signal(&g_v4l2_cond);
        } else {
            g_buffer_state[index] = 0;
        }
    }
    if (g_in_flight_writes > 0) {
        g_in_flight_writes--;
        if (g_in_flight_writes == 0) {
            pthread_cond_broadcast(&g_write_done_cond);
        }
    }
    pthread_mutex_unlock(&g_v4l2_mutex);
}

void v4l2_abort_write(int index) {
    pthread_mutex_lock(&g_v4l2_mutex);
    if (index >= 0 && index < (int)g_buffer_count) {
        g_buffer_state[index] = g_is_streaming ? 1 : 0;
    }
    if (g_in_flight_writes > 0) {
        g_in_flight_writes--;
        if (g_in_flight_writes == 0) {
            pthread_cond_broadcast(&g_write_done_cond);
        }
    }
    pthread_mutex_unlock(&g_v4l2_mutex);
}

void v4l2_set_buffer_bytesused(int index, uint32_t size) {
    if (index >= 0 && index < (int)g_buffer_count) {
        g_buffer_bytesused[index] = size;
    }
}

void v4l2_set_buffer_meta(int index, uint32_t sequence, uint64_t timestamp_ns) {
    if (index >= 0 && index < (int)g_buffer_count) {
        g_buffer_sequence[index] = sequence;
        g_buffer_timestamp[index] = timestamp_ns;
    }
}
