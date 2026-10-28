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
#include <sys/syscall.h>
#include <pthread.h>
#include <sys/uio.h>
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

int g_buffer_fds[MAX_BUFFERS] = {-1, -1, -1, -1, -1, -1, -1, -1};
uint32_t g_buffer_lengths[MAX_BUFFERS] = {0};
void* g_buffer_ptrs[MAX_BUFFERS] = {NULL};
uint32_t g_buffer_count = 0;
uint32_t g_buffer_bytesused[MAX_BUFFERS] = {0};
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
    if (width >= 1280 || height >= 720) return 8; // V4L2_COLORSPACE_REC709
    return 1; // V4L2_COLORSPACE_SMPTE170M
}

uint32_t v4l2_get_current_frame_size() {
    if (g_current_format == V4L2_PIX_FMT_MJPEG) {
        return g_current_width * g_current_height; 
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
    for (int i = 0; i < g_res_count; i++) {
        if (g_resolutions[i].android_format == g_current_android_format && 
            g_resolutions[i].width == w && 
            g_resolutions[i].height == h) {
            return g_resolutions[i].max_fps;
        }
    }
    return 30; 
}

void v4l2_set_cfa_pattern(int cfa) {
    uint32_t fourcc = V4L2_PIX_FMT_SRGGB16;
    const char *desc = "16-bit Bayer RGRG/GBGB";

    switch (cfa) {
        case 1:
            fourcc = v4l2_fourcc('G', 'R', '1', '6');
            desc = "16-bit Bayer GRGR/BGBG";
            break;
        case 2:
            fourcc = v4l2_fourcc('G', 'B', '1', '6');
            desc = "16-bit Bayer GBGB/RGRG";
            break;
        case 3:
            fourcc = v4l2_fourcc('B', 'G', '1', '6');
            desc = "16-bit Bayer BGBG/GRGR";
            break;
        case 0:
        default:
            fourcc = v4l2_fourcc('R', 'G', '1', '6');
            desc = "16-bit Bayer RGRG/GBGB";
            break;
    }

    for (size_t i = 0; i < MASTER_FORMAT_COUNT; ++i) {
        if (kMasterFormats[i].android_format == 32) {
            kMasterFormats[i].pixelformat = fourcc;
            snprintf(kMasterFormats[i].description, sizeof(kMasterFormats[i].description), "%s", desc);
            break;
        }
    }
}

void v4l2_init_formats(int *android_formats, int count) {
    g_supported_format_count = 0;

    for (size_t m = 0; m < MASTER_FORMAT_COUNT; ++m) {
        int req_android_fmt = kMasterFormats[m].android_format;
        int is_supported = 0;

        for (int i = 0; i < count; ++i) {
            if (android_formats[i] == req_android_fmt) {
                is_supported = 1;
                break;
            }
        }

        if (is_supported && g_supported_format_count < 32) {
            g_supported_formats[g_supported_format_count++] = kMasterFormats[m];
        }
    }
}

void v4l2_init_resolutions(int* formats, int* widths, int* heights, int* fps, int count) {
    if (count <= 0) return;

    g_res_count = (count > 256) ? 256 : count;
    for (int i = 0; i < g_res_count; i++) {
        g_resolutions[i].android_format = formats[i];
        g_resolutions[i].width = (uint32_t)widths[i];
        g_resolutions[i].height = (uint32_t)heights[i];
        g_resolutions[i].max_fps = (uint32_t)fps[i];
    }

    if (g_supported_format_count > 0) {
        g_current_format = g_supported_formats[0].pixelformat;
        g_current_android_format = g_supported_formats[0].android_format;
    } else {
        g_current_format = v4l2_fourcc('N', 'V', '1', '2');
        g_current_android_format = 35;
    }

    // Default boot to 1080p, then 720p, else first valid resolution
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

    g_current_width = g_resolutions[target_idx].width;
    g_current_height = g_resolutions[target_idx].height;
    g_current_fps = get_max_fps_for_res(g_current_width, g_current_height);
}

int v4l2_get_android_format() { return g_current_android_format; }
int v4l2_get_width() { return g_current_width; }
int v4l2_get_height() { return g_current_height; }
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

void v4l2_free_buffers() {
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
        g_buffer_state[i] = 0;
    }
    g_buffer_count = 0;
    s_next_write_idx = 0;
    s_next_read_idx = 0;
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
            req.buf.flags = 0x00000001;
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
    if (g_buffer_state[index] == 1) ready = 1;
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
    for (int i = 0; i < (int)g_buffer_count; i++) {
        int candidate = (s_next_write_idx + i) % (int)g_buffer_count;
        if (g_buffer_state[candidate] == 1) {
            s_next_write_idx = (candidate + 1) % (int)g_buffer_count;
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
        g_buffer_state[index] = 2; 
        if (g_event_fd >= 0) {
            uint64_t val = 1;
            write(g_event_fd, &val, sizeof(val));
        }
        pthread_cond_signal(&g_v4l2_cond);
    }
    pthread_mutex_unlock(&g_v4l2_mutex);
}

void v4l2_set_buffer_bytesused(int index, uint32_t size) {
    if (index >= 0 && index < (int)g_buffer_count) {
        g_buffer_bytesused[index] = size;
    }
}
