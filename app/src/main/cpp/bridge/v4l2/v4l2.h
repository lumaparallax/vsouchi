/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Internal declarations for the Android NDK Bridge's V4L2 state machine and buffer caching.
#ifndef BRIDGE_V4L2_H
#define BRIDGE_V4L2_H

#include <stdint.h>

void v4l2_clear_cameras(void);
void v4l2_add_camera(int index, const char* name, const char* id,
                     int* formats, int fmt_count,
                     int* res_fmts, int* res_ws, int* res_hs, int* res_fps, int res_count,
                     int* ae_fps, int ae_fps_count,
                     int cfa, int has_focus, float min_focus, int has_exp,
                     long min_exp, long max_exp, int orientation, int hflip);
int v4l2_switch_camera(int index);
void v4l2_init_resolutions(int* formats, int* widths, int* heights, int* fps, int count);
int v4l2_handle_command(int socket_fd, uint32_t client_fd, uint32_t cmd, const char* device_name, uint32_t kernel_version);

void v4l2_start_worker();
void v4l2_stop_worker();
void v4l2_cleanup();

int v4l2_get_width();
int v4l2_get_height();
int v4l2_get_sensor_width();
int v4l2_get_sensor_height();
int v4l2_get_fps();

void v4l2_init_formats(int* android_formats, int count);
int v4l2_get_android_format();
void v4l2_set_cfa_pattern(int cfa);

void* v4l2_get_buffer_ptr(int index);
void v4l2_set_buffer_bytesused(int index, uint32_t size);
void v4l2_set_buffer_meta(int index, uint32_t sequence, uint64_t timestamp_ns);
uint32_t v4l2_get_buffer_size(int index);
int v4l2_get_queued_index();
void v4l2_push_filled_buffer(int index);
void v4l2_abort_write(int index);
uint32_t v4l2_get_current_format();
int v4l2_is_buffer_ready_for_write(int index);

#endif // BRIDGE_V4L2_H
