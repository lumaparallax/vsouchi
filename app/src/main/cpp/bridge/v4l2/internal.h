/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Internal shared state between the V4L2 state machine and IPC command router.
#ifndef BRIDGE_V4L2_INTERNAL_H
#define BRIDGE_V4L2_INTERNAL_H

#include <stdint.h>
#include <pthread.h>
#include <linux/videodev2.h>
#include "vsouchi_ipc.h"

#define MAX_BUFFERS 8

typedef struct {
    uint32_t pixelformat;
    char description[32];
    int android_format;
    uint32_t flags;
} FormatMap;

typedef struct {
    int android_format;
    uint32_t width;
    uint32_t height;
    uint32_t max_fps;
} ResolutionMap;

extern FormatMap g_supported_formats[32];
extern int g_supported_format_count;

extern ResolutionMap g_resolutions[256];
extern int g_res_count;

extern uint32_t g_current_width;
extern uint32_t g_current_height;
extern uint32_t g_current_fps;
extern uint32_t g_current_format;
extern int g_current_android_format;

extern int g_buffer_fds[MAX_BUFFERS];
extern uint32_t g_buffer_lengths[MAX_BUFFERS];
extern void* g_buffer_ptrs[MAX_BUFFERS];
extern uint32_t g_buffer_count;
extern uint32_t g_buffer_bytesused[MAX_BUFFERS];
extern uint32_t g_buffer_sequence[MAX_BUFFERS];
extern uint64_t g_buffer_timestamp[MAX_BUFFERS];
extern uint32_t g_frame_sequence;
extern int g_buffer_state[MAX_BUFFERS]; // 0 = Dequeued, 1 = Queued, 2 = Done, 3 = Leased
extern int g_event_fd;

extern volatile int g_is_streaming;
extern pthread_mutex_t g_v4l2_mutex;
extern pthread_cond_t g_v4l2_cond;

uint32_t get_correct_colorspace(uint32_t width, uint32_t height);
uint32_t v4l2_get_current_frame_size();
uint32_t get_max_fps_for_res(uint32_t w, uint32_t h);

void v4l2_send_reply(int socket_fd, uint32_t client_fd, int32_t status, void* payload, uint32_t sz);
void v4l2_send_error(int socket_fd, uint32_t client_fd, int32_t error_code);
void v4l2_free_buffers();
void v4l2_queue_dqbuf_request(int socket_fd, uint32_t client_fd, IpcV4l2Buffer* buf);

static inline int read_exact(int fd, void *buf, size_t count) {
    size_t total = 0;
    char *p = (char *)buf;
    while (total < count) {
        ssize_t n = read(fd, p + total, count - total);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

#endif // BRIDGE_V4L2_INTERNAL_H
