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
#include <dlfcn.h>
#include <android/log.h>

#include "bridge/v4l2/v4l2.h"
#include "bridge/v4l2/internal.h"

#define TAG "vSouchi:V4L2State"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

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

volatile int g_is_streaming = 0;
pthread_mutex_t g_v4l2_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  g_v4l2_cond  = PTHREAD_COND_INITIALIZER;

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
    if (width >= 1280 || height >= 720) return 3; 
    return 1; 
}

uint32_t v4l2_get_current_frame_size() {
    if (g_current_format == v4l2_fourcc('J', 'P', 'E', 'G') || g_current_format == v4l2_fourcc('H', 'E', 'V', 'C')) {
        return g_current_width * g_current_height; 
    } else if (g_current_format == v4l2_fourcc('R', 'G', 'G', 'B') || g_current_format == v4l2_fourcc('R', 'G', '1', '0') || g_current_format == v4l2_fourcc('P', '0', '1', '0') || g_current_format == v4l2_fourcc('Y', 'U', 'Y', 'V')) {
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

void v4l2_init_formats(int* android_formats, int count) {
    g_supported_format_count = 0;
    for (int i = 0; i < count; i++) {
        int fmt = android_formats[i];
        if (fmt == 35 && g_supported_format_count < 30) { 
            g_supported_formats[g_supported_format_count++] = (FormatMap){v4l2_fourcc('N', 'V', '1', '2'), "YUV 4:2:0 (NV12)", 35};
            g_supported_formats[g_supported_format_count++] = (FormatMap){v4l2_fourcc('N', 'V', '2', '1'), "YUV 4:2:0 (NV21)", 35};
            g_supported_formats[g_supported_format_count++] = (FormatMap){v4l2_fourcc('Y', 'U', 'Y', 'V'), "YUYV 4:2:2", 35};
            g_supported_formats[g_supported_format_count++] = (FormatMap){v4l2_fourcc('Y', 'U', '1', '2'), "Planar YU12 4:2:0", 35};
            g_supported_formats[g_supported_format_count++] = (FormatMap){v4l2_fourcc('Y', 'V', '1', '2'), "Planar YV12 4:2:0", 35};
        } else if (fmt == 256 && g_supported_format_count < 32) { 
            g_supported_formats[g_supported_format_count++] = (FormatMap){v4l2_fourcc('J', 'P', 'E', 'G'), "Motion-JPEG", 256};
        } else if (fmt == 32 && g_supported_format_count < 32) { 
            g_supported_formats[g_supported_format_count++] = (FormatMap){v4l2_fourcc('R', 'G', 'G', 'B'), "16-bit Bayer RGRG/GBGB", 32};
        } else if (fmt == 37 && g_supported_format_count < 32) { 
            g_supported_formats[g_supported_format_count++] = (FormatMap){v4l2_fourcc('R', 'G', '1', '0'), "10-bit Bayer RGRG/GBGB", 37};
        } else if (fmt == 1212500294 && g_supported_format_count < 32) { 
            g_supported_formats[g_supported_format_count++] = (FormatMap){v4l2_fourcc('H', 'E', 'V', 'C'), "HEVC/H.265", 1212500294};
        } else if (fmt == 54 && g_supported_format_count < 32) { 
            g_supported_formats[g_supported_format_count++] = (FormatMap){v4l2_fourcc('P', '0', '1', '0'), "10-bit YUV 4:2:0", 54};
        }
    }
    
    if (g_supported_format_count > 0) {
        g_current_format = g_supported_formats[0].v4l2_fourcc;
        g_current_android_format = g_supported_formats[0].android_format;
    }
}

void v4l2_init_resolutions(int* fmts, int* widths, int* heights, int* fps, int count) {
    g_res_count = (count > 256) ? 256 : count;
    for (int i = 0; i < g_res_count; i++) {
        g_resolutions[i].android_format = fmts[i]; 
        g_resolutions[i].width = widths[i];
        g_resolutions[i].height = heights[i];
        g_resolutions[i].max_fps = fps[i];
    }
    if (g_res_count > 0) {
        g_current_width = g_resolutions[0].width;
        g_current_height = g_resolutions[0].height;
        g_current_fps = g_resolutions[0].max_fps;
    }
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

void v4l2_free_buffers() {
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
}

void v4l2_cleanup() {
    g_is_streaming = 0;
    
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
            for (int i = 0; i < g_buffer_count; i++) { 
                if (g_buffer_state[i] == 2) { idx = i; break; } 
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
    }
    pthread_mutex_unlock(&g_dq_mutex);
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
    if (index >= 0 && index < g_buffer_count) return g_buffer_ptrs[index];
    return NULL;
}

uint32_t v4l2_get_buffer_size(int index) {
    return (index >= 0 && index < g_buffer_count) ? g_buffer_lengths[index] : 0;
}

int v4l2_get_queued_index() {
    pthread_mutex_lock(&g_v4l2_mutex);
    for (int i = 0; i < g_buffer_count; i++) {
        if (g_buffer_state[i] == 1) { pthread_mutex_unlock(&g_v4l2_mutex); return i; }
    }
    pthread_mutex_unlock(&g_v4l2_mutex);
    return -1;
}

void v4l2_push_filled_buffer(int index) {
    pthread_mutex_lock(&g_v4l2_mutex);
    if (index >= 0 && index < g_buffer_count) {
        
        // Zero-latency enforcement: Recycle unread frames so Android can immediately overwrite them
        for (int i = 0; i < g_buffer_count; i++) {
            if (g_buffer_state[i] == 2) {
                g_buffer_state[i] = 1; 
            }
        }

        g_buffer_state[index] = 2; 
        pthread_cond_signal(&g_v4l2_cond);
    }
    pthread_mutex_unlock(&g_v4l2_mutex);
}

void v4l2_set_buffer_bytesused(int index, uint32_t size) {
    if (index >= 0 && index < g_buffer_count) {
        g_buffer_bytesused[index] = size;
    }
}
