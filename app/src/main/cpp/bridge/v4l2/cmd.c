/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Translates incoming V4L2 IPC commands, manages format negotiation, and provisions shared memory.
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/mman.h>
#include <linux/memfd.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <dlfcn.h>
#include <errno.h>
#include <android/log.h>
#include <linux/videodev2.h>

#include "bridge/v4l2/v4l2.h"
#include "bridge/v4l2/internal.h"

#define TAG "vSouchi:V4L2Cmd"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#ifndef V4L2_PIX_FMT_MJPEG
#define V4L2_PIX_FMT_MJPEG v4l2_fourcc('M', 'J', 'P', 'G')
#endif
#ifndef V4L2_PIX_FMT_SRGGB16
#define V4L2_PIX_FMT_SRGGB16 v4l2_fourcc('R', 'G', '1', '6')
#endif

// Externs mapped from jni.c
extern char g_bus_info[32];
extern int g_has_focus, g_has_exposure;
extern float g_min_focus_dist;
extern long g_min_exposure, g_max_exposure;
extern int g_current_focus_auto, g_current_focus_abs;
extern int g_current_exposure_auto, g_current_exposure_abs;
extern int g_last_ctrl_id, g_last_ctrl_val;
extern int g_sensor_orientation, g_sensor_hflip, g_sensor_vflip;

// Bypasses standard malloc. Tries Android's ASharedMemory first for zero-copy GPU compatibility, 
// falling back to Linux memfd_create on older devices.
static int create_shared_memory(const char *name, size_t size) {
    void *libandroid = dlopen("libandroid.so", RTLD_NOW);
    if (libandroid) {
        int (*ASharedMemory_create)(const char *, size_t) = dlsym(libandroid, "ASharedMemory_create");
        void (*ASharedMemory_setProt)(int, int) = dlsym(libandroid, "ASharedMemory_setProt");
        if (ASharedMemory_create && ASharedMemory_setProt) {
            int fd = ASharedMemory_create(name, size);
            if (fd >= 0) {
                ASharedMemory_setProt(fd, PROT_READ | PROT_WRITE);
                dlclose(libandroid);
                return fd;
            }
        }
        dlclose(libandroid);
    }
    int fd = syscall(SYS_memfd_create, name, MFD_CLOEXEC);
    if (fd >= 0) {
        ftruncate(fd, size);
        return fd;
    }
    return -1;
}

static void v4l2_fill_ipc_format(IpcV4l2Format *fmt, uint32_t width, uint32_t height, uint32_t pixelformat) {
    fmt->width = width;
    fmt->height = height;
    fmt->pixelformat = pixelformat;
    fmt->field = 1; // V4L2_FIELD_NONE (Progressive)

    switch (pixelformat) {
        case v4l2_fourcc('N', 'V', '1', '2'):
        case v4l2_fourcc('N', 'V', '2', '1'):
        case v4l2_fourcc('Y', 'U', '1', '2'):
        case v4l2_fourcc('Y', 'V', '1', '2'):
            fmt->bytesperline = width;
            fmt->sizeimage = (width * height * 3) / 2;
            break;

        case v4l2_fourcc('Y', 'U', 'Y', 'V'):
        case v4l2_fourcc('R', 'G', '1', '6'):
        case v4l2_fourcc('G', 'R', '1', '6'):
        case v4l2_fourcc('G', 'B', '1', '6'):
        case v4l2_fourcc('B', 'G', '1', '6'):
            fmt->bytesperline = width * 2;
            fmt->sizeimage = width * height * 2;
            break;

        case V4L2_PIX_FMT_MJPEG:
        default:
            fmt->bytesperline = 0; // Compressed bitstreams have no line stride
            fmt->sizeimage = width * height * 2; // Conservative buffer boundary
            break;
    }

    fmt->colorspace = get_correct_colorspace(width, height);
}

static void v4l2_negotiate_format(uint32_t req_w, uint32_t req_h, uint32_t req_fmt,
                                  uint32_t *out_w, uint32_t *out_h, uint32_t *out_fmt, int *out_android_fmt) {
    uint32_t chosen_fmt = 0;
    int chosen_android_fmt = 35;

    // 1. FourCC Validation: Verify requested format is supported
    for (int i = 0; i < g_supported_format_count; ++i) {
        if (g_supported_formats[i].pixelformat == req_fmt) {
            chosen_fmt = req_fmt;
            chosen_android_fmt = g_supported_formats[i].android_format;
            break;
        }
    }

    // V4L2 spec: Drivers must silently override unsupported formats with a default, never return an error
    if (chosen_fmt == 0) {
        chosen_fmt = (g_supported_format_count > 0) ? g_supported_formats[0].pixelformat : v4l2_fourcc('N', 'V', '1', '2');
        chosen_android_fmt = (g_supported_format_count > 0) ? g_supported_formats[0].android_format : 35;
    }

    // 2. Resolution Clamping: Exact match or nearest Euclidean distance
    if (g_res_count == 0) {
        *out_w = req_w;
        *out_h = req_h;
        *out_fmt = chosen_fmt;
        *out_android_fmt = chosen_android_fmt;
        return;
    }

    int chosen_idx = -1;
    uint32_t min_diff = UINT32_MAX;

    for (int i = 0; i < g_res_count; ++i) {
        if (g_resolutions[i].android_format == chosen_android_fmt) {
            if (g_resolutions[i].width == req_w && g_resolutions[i].height == req_h) {
                chosen_idx = i;
                break;
            }

            int dw = (int)g_resolutions[i].width - (int)req_w;
            int dh = (int)g_resolutions[i].height - (int)req_h;
            uint32_t diff = (uint32_t)(abs(dw) + abs(dh));

            if (diff < min_diff) {
                min_diff = diff;
                chosen_idx = i;
            }
        }
    }

    if (chosen_idx == -1) {
        chosen_idx = 0;
    }

    *out_w = g_resolutions[chosen_idx].width;
    *out_h = g_resolutions[chosen_idx].height;
    *out_fmt = chosen_fmt;
    *out_android_fmt = chosen_android_fmt;
}

int v4l2_handle_command(int socket_fd, uint32_t client_fd, uint32_t cmd, const char* device_name, uint32_t kernel_version) {

    if (cmd == CMD_GET_CAPS) {
        IpcV4l2Caps caps;
        memset(&caps, 0, sizeof(IpcV4l2Caps));
        snprintf(caps.driver, sizeof(caps.driver), "vsouchi");
        snprintf(caps.card, sizeof(caps.card), "%s", device_name);
        snprintf(caps.bus_info, sizeof(caps.bus_info), "%s", g_bus_info);

        if ((kernel_version >> 16) >= 3) {
            caps.version = kernel_version;
        } else {
            caps.version = (((5) << 16) + ((10) << 8) + (0));
        }

        // V4L2_CAP_VIDEO_CAPTURE (0x01) | V4L2_CAP_EXT_PIX_FORMAT (0x00200000) | V4L2_CAP_STREAMING (0x04000000)
        uint32_t dev_caps = 0x04200001;
        caps.device_caps = dev_caps;
        // V4L2_CAP_DEVICE_CAPS (0x80000000) signals to userspace that device_caps is valid
        caps.capabilities = dev_caps | 0x80000000;
        
        v4l2_send_reply(socket_fd, client_fd, 0, &caps, sizeof(IpcV4l2Caps));
        return 0;

    } else if (cmd == CMD_ENUM_FMT) {
        IpcV4l2FmtDesc req;
        if (read_exact(socket_fd, &req, sizeof(IpcV4l2FmtDesc)) != 0) return -1;

        if (req.index >= (uint32_t)g_supported_format_count || req.type != 1 /* V4L2_BUF_TYPE_VIDEO_CAPTURE */) {
            v4l2_send_error(socket_fd, client_fd, EINVAL);
            return 0;
        }

        IpcV4l2FmtDesc reply;
        memset(&reply, 0, sizeof(IpcV4l2FmtDesc));
        reply.index = req.index;
        reply.type = req.type;
        reply.flags = g_supported_formats[req.index].flags;
        reply.pixelformat = g_supported_formats[req.index].pixelformat;
        snprintf(reply.description, sizeof(reply.description), "%s", g_supported_formats[req.index].description);

        v4l2_send_reply(socket_fd, client_fd, 0, &reply, sizeof(IpcV4l2FmtDesc));
        return 0;

    } else if (cmd == CMD_G_FMT) {
        IpcV4l2Format reply;
        memset(&reply, 0, sizeof(IpcV4l2Format));
        v4l2_fill_ipc_format(&reply, g_current_width, g_current_height, g_current_format);
        v4l2_send_reply(socket_fd, client_fd, 0, &reply, sizeof(IpcV4l2Format));
        return 0;

    } else if (cmd == CMD_TRY_FMT || cmd == CMD_S_FMT) {
        IpcV4l2Format req_fmt;
        if (read_exact(socket_fd, &req_fmt, sizeof(IpcV4l2Format)) != 0) return -1;

        uint32_t best_w, best_h, best_fmt;
        int best_android_fmt;
        v4l2_negotiate_format(req_fmt.width, req_fmt.height, req_fmt.pixelformat,
                              &best_w, &best_h, &best_fmt, &best_android_fmt);

        if (cmd == CMD_S_FMT) {
            if (g_is_streaming) {
                v4l2_send_error(socket_fd, client_fd, EBUSY);
                return 0;
            }
            g_current_width = best_w;
            g_current_height = best_h;
            g_current_format = best_fmt;
            g_current_android_format = best_android_fmt;
            g_current_fps = get_max_fps_for_res(g_current_width, g_current_height);
        }

        IpcV4l2Format reply;
        memset(&reply, 0, sizeof(IpcV4l2Format));
        v4l2_fill_ipc_format(&reply, best_w, best_h, best_fmt);
        v4l2_send_reply(socket_fd, client_fd, 0, &reply, sizeof(IpcV4l2Format));
        return 0;

    } else if (cmd == CMD_ENUM_FRAMESIZES) {
        IpcV4l2FrmSizeEnum req;
        if (read_exact(socket_fd, &req, sizeof(IpcV4l2FrmSizeEnum)) != 0) return -1;
        req.type = 1;
        
        int target_android_fmt = -1;
        for (int i = 0; i < g_supported_format_count; i++) {
            if (g_supported_formats[i].pixelformat == req.pixel_format) {
                target_android_fmt = g_supported_formats[i].android_format;
                break;
            }
        }

        int match_count = 0;
        int found_idx = -1;
        
        for (int i = 0; i < g_res_count; i++) {
            if (g_resolutions[i].android_format == target_android_fmt) {
                if (match_count == (int)req.index) {
                    found_idx = i;
                    break;
                }
                match_count++;
            }
        }

        if (found_idx != -1) {
            req.width = g_resolutions[found_idx].width;
            req.height = g_resolutions[found_idx].height;
        } else {
            req.pixel_format = 0xFFFFFFFF; 
            req.width = 0;
            req.height = 0;
        }
        v4l2_send_reply(socket_fd, client_fd, 0, &req, sizeof(IpcV4l2FrmSizeEnum));
        return 0;

    } else if (cmd == CMD_ENUM_FRAMEINTERVALS) {
        IpcV4l2FrmIvalEnum req;
        if (read_exact(socket_fd, &req, sizeof(IpcV4l2FrmIvalEnum)) != 0) return -1;
        req.type = 1;
        req.fps_numerator = 0; 
        
        int target_android_fmt = -1;
        for (int i = 0; i < g_supported_format_count; i++) {
            if (g_supported_formats[i].pixelformat == req.pixel_format) {
                target_android_fmt = g_supported_formats[i].android_format;
                break;
            }
        }

        for (int i = 0; i < g_res_count; i++) {
            if (g_resolutions[i].android_format == target_android_fmt && 
                g_resolutions[i].width == req.width && 
                g_resolutions[i].height == req.height) {
                
                if (req.index < g_resolutions[i].fps_count) {
                    req.fps_numerator = 1;
                    req.fps_denominator = g_resolutions[i].fps_list[req.index];
                }
                break;
            }
        }
        v4l2_send_reply(socket_fd, client_fd, 0, &req, sizeof(IpcV4l2FrmIvalEnum));
        return 0;

    } else if (cmd == CMD_CROPCAP) {
        IpcV4l2CropCap crop;
        if (read_exact(socket_fd, &crop, sizeof(IpcV4l2CropCap)) != 0) return -1;
        
        crop.bounds_left = 0; crop.bounds_top = 0;
        crop.bounds_width = g_current_width; crop.bounds_height = g_current_height;
        crop.defrect_left = 0; crop.defrect_top = 0;
        crop.defrect_width = g_current_width; crop.defrect_height = g_current_height;
        crop.pixelaspect_num = 1; crop.pixelaspect_denom = 1;
        
        v4l2_send_reply(socket_fd, client_fd, 0, &crop, sizeof(IpcV4l2CropCap));
        return 0;

    } else if (cmd == CMD_QUERYCTRL) {
        uint32_t req_id;
        if (read_exact(socket_fd, &req_id, sizeof(uint32_t)) != 0) return -1;

        struct v4l2_queryctrl qc;
        memset(&qc, 0, sizeof(qc));

        uint32_t ctrls[16];
        int num_ctrls = 0;
        
        ctrls[num_ctrls++] = 0x00980001; // V4L2_CID_USER_CLASS
        ctrls[num_ctrls++] = V4L2_CID_HFLIP;  
        ctrls[num_ctrls++] = V4L2_CID_VFLIP;  
        ctrls[num_ctrls++] = V4L2_CID_ROTATE; 
        ctrls[num_ctrls++] = 0x009a0001; // V4L2_CID_CAMERA_CLASS

        if (g_has_exposure) {
            ctrls[num_ctrls++] = V4L2_CID_EXPOSURE_AUTO;     
            ctrls[num_ctrls++] = V4L2_CID_EXPOSURE_ABSOLUTE; 
        }
        if (g_has_focus) {
            ctrls[num_ctrls++] = V4L2_CID_FOCUS_ABSOLUTE;    
            ctrls[num_ctrls++] = V4L2_CID_FOCUS_AUTO;        
        }

        uint32_t target_id = 0;

        // Fail only if compound controls are requested ALONE without NEXT_CTRL
        if ((req_id & 0x40000000 /* NEXT_COMPOUND */) && !(req_id & 0x80000000 /* NEXT_CTRL */)) {
            v4l2_send_error(socket_fd, client_fd, EINVAL);
            return 0;
        }

        // Standard next control enumeration
        if (req_id & 0x80000000 /* NEXT_CTRL */) {
            uint32_t start_id = req_id & 0x0fffffff; 
            for (int i = 0; i < num_ctrls; i++) {
                if (ctrls[i] > start_id) {
                    target_id = ctrls[i];
                    break;
                }
            }
        } else {
            // Direct control query by exact ID
            for (int i = 0; i < num_ctrls; i++) {
                if (ctrls[i] == req_id) {
                    target_id = ctrls[i];
                    break;
                }
            }
        }

        if (target_id == 0) {
            v4l2_send_error(socket_fd, client_fd, EINVAL);
            return 0;
        }

        if (target_id == 0x00980001) {
            qc.id = target_id;
            qc.type = 6; // V4L2_CTRL_TYPE_CTRL_CLASS
            strcpy((char*)qc.name, "User Controls");
            qc.flags = 0x0044; // V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_WRITE_ONLY per checkQCtrl
        }
        else if (target_id == V4L2_CID_HFLIP) {
            qc.id = target_id;
            qc.type = 2;
            strcpy((char*)qc.name, "Horizontal Flip");
            qc.minimum = 0; qc.maximum = 1; qc.step = 1; qc.default_value = g_sensor_hflip;
            qc.flags = 0;
        }
        else if (target_id == V4L2_CID_VFLIP) {
            qc.id = target_id;
            qc.type = 2;
            strcpy((char*)qc.name, "Vertical Flip");
            qc.minimum = 0; qc.maximum = 1; qc.step = 1; qc.default_value = 0;
            qc.flags = 0;
        }
        else if (target_id == V4L2_CID_ROTATE) {
            qc.id = target_id;
            qc.type = 1; 
            strcpy((char*)qc.name, "Rotate");
            qc.minimum = 0; qc.maximum = 270; qc.step = 90; qc.default_value = g_sensor_orientation;
            qc.flags = 0x0004;
        }
        else if (target_id == 0x009a0001) {
            qc.id = target_id;
            qc.type = 6; // V4L2_CTRL_TYPE_CTRL_CLASS
            strcpy((char*)qc.name, "Camera Controls");
            qc.flags = 0x0044; // V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_WRITE_ONLY per checkQCtrl
        }
        else if (target_id == V4L2_CID_EXPOSURE_AUTO && g_has_exposure) {
            qc.id = target_id;
            qc.type = 3; // Menu
            strcpy((char*)qc.name, "Exposure, Auto");
            qc.minimum = 0; qc.maximum = 1; qc.step = 1; qc.default_value = 0;
        } 
        else if (target_id == V4L2_CID_EXPOSURE_ABSOLUTE && g_has_exposure) {
            qc.id = target_id;
            qc.type = 1; 
            strcpy((char*)qc.name, "Exposure (Absolute)");
            qc.minimum = 1; qc.maximum = 5000; qc.step = 1; qc.default_value = 156;
        }
        else if (target_id == V4L2_CID_FOCUS_ABSOLUTE && g_has_focus) {
            qc.id = target_id;
            qc.type = 1; 
            strcpy((char*)qc.name, "Focus (Absolute)");
            qc.minimum = 0; qc.maximum = 100; qc.step = 1; qc.default_value = 0;
        }
        else if (target_id == V4L2_CID_FOCUS_AUTO && g_has_focus) {
            qc.id = target_id;
            qc.type = 2; 
            strcpy((char*)qc.name, "Focus, Automatic");
            qc.minimum = 0; qc.maximum = 1; qc.step = 1; qc.default_value = 1;
        }

        v4l2_send_reply(socket_fd, client_fd, 0, &qc, sizeof(struct v4l2_queryctrl));
        return 0;

    } else if (cmd == CMD_G_CTRL) {
        uint32_t ctrl_id;
        if (read_exact(socket_fd, &ctrl_id, sizeof(uint32_t)) != 0) return -1;
        
        IpcControlPayload resp = { .id = ctrl_id, .value = 0 };
        int valid = 1;

        if (ctrl_id == V4L2_CID_FOCUS_AUTO) resp.value = g_current_focus_auto;
        else if (ctrl_id == V4L2_CID_FOCUS_ABSOLUTE) resp.value = g_current_focus_abs;
        else if (ctrl_id == V4L2_CID_EXPOSURE_AUTO) resp.value = g_current_exposure_auto;
        else if (ctrl_id == V4L2_CID_EXPOSURE_ABSOLUTE) resp.value = g_current_exposure_abs ? g_current_exposure_abs : 156;
        else if (ctrl_id == V4L2_CID_ROTATE) resp.value = g_sensor_orientation;
        else if (ctrl_id == V4L2_CID_HFLIP) resp.value = g_sensor_hflip;
        else if (ctrl_id == V4L2_CID_VFLIP) resp.value = g_sensor_vflip;
        else valid = 0;

        if (!valid) {
            v4l2_send_error(socket_fd, client_fd, EINVAL);
            return 0;
        }

        v4l2_send_reply(socket_fd, client_fd, 0, &resp, sizeof(resp));
        return 0;

    } else if (cmd == CMD_S_CTRL) {
        IpcControlPayload payload;
        if (read_exact(socket_fd, &payload, sizeof(payload)) != 0) return -1;

        if (payload.id == V4L2_CID_ROTATE) {
            v4l2_send_error(socket_fd, client_fd, EACCES);
            return 0;
        }

        int valid_id = 0;
        int in_range = 0;

        if (payload.id == V4L2_CID_HFLIP) {
            valid_id = 1;
            if (payload.value >= 0 && payload.value <= 1) {
                g_sensor_hflip = payload.value;
                in_range = 1;
            }
        }
        else if (payload.id == V4L2_CID_VFLIP) {
            valid_id = 1;
            if (payload.value >= 0 && payload.value <= 1) {
                g_sensor_vflip = payload.value;
                in_range = 1;
            }
        }
        else if (payload.id == V4L2_CID_FOCUS_AUTO) { 
            valid_id = 1;
            if (payload.value >= 0 && payload.value <= 1) {
                g_current_focus_auto = payload.value; 
                in_range = 1;
            }
        }
        else if (payload.id == V4L2_CID_FOCUS_ABSOLUTE) { 
            valid_id = 1;
            if (payload.value >= 0 && payload.value <= 100) {
                g_current_focus_abs = payload.value; 
                in_range = 1;
            }
        }
        else if (payload.id == V4L2_CID_EXPOSURE_AUTO) { 
            valid_id = 1;
            if (payload.value >= 0 && payload.value <= 1) {
                g_current_exposure_auto = payload.value; 
                in_range = 1;
            }
        }
        else if (payload.id == V4L2_CID_EXPOSURE_ABSOLUTE) { 
            valid_id = 1;
            if (payload.value >= 1 && payload.value <= 5000) {
                g_current_exposure_abs = payload.value; 
                in_range = 1;
            }
        }

        if (!valid_id) {
            v4l2_send_error(socket_fd, client_fd, EINVAL);
            return 0;
        }

        if (!in_range) {
            v4l2_send_error(socket_fd, client_fd, ERANGE);
            return 0;
        }

        g_last_ctrl_id = payload.id;
        g_last_ctrl_val = payload.value;

        int32_t ack = 0;
        v4l2_send_reply(socket_fd, client_fd, 0, &ack, sizeof(ack));
        return 4;

    } else if (cmd == CMD_G_PARM || cmd == CMD_S_PARM) {
        IpcV4l2StreamParm req;
        if (read_exact(socket_fd, &req, sizeof(IpcV4l2StreamParm)) != 0) return -1;
        
        if (cmd == CMD_S_PARM && req.fps_denominator > 0) {
            uint32_t requested_fps = req.fps_denominator;
            if (req.fps_numerator > 1) requested_fps = req.fps_denominator / req.fps_numerator;

            uint32_t max_fps = get_max_fps_for_res(g_current_width, g_current_height);
            if (requested_fps > max_fps) requested_fps = max_fps;
            // Prevent Android Image Signal Processor crashes caused by excessively slow frame requests
            if (requested_fps < 7) requested_fps = 7; 
            
            g_current_fps = requested_fps;
        }
        
        req.fps_numerator = 1;
        req.fps_denominator = g_current_fps;
        v4l2_send_reply(socket_fd, client_fd, 0, &req, sizeof(IpcV4l2StreamParm));
        return 0;

    } else if (cmd == CMD_REQBUFS) {
        IpcV4l2ReqBufs req;
        if (read_exact(socket_fd, &req, sizeof(IpcV4l2ReqBufs)) != 0) return -1;
        v4l2_free_buffers();
        
        g_buffer_count = (req.count > MAX_BUFFERS) ? MAX_BUFFERS : req.count;
        uint32_t sz = v4l2_get_current_frame_size();
 
        for (uint32_t i = 0; i < g_buffer_count; i++) {
            g_buffer_fds[i] = create_shared_memory("vsouchi_frame", sz);
            g_buffer_lengths[i] = sz;
            g_buffer_bytesused[i] = 0;
            g_buffer_ptrs[i] = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, g_buffer_fds[i], 0);
            g_buffer_state[i] = 0;
        }

        if (g_buffer_count > 0) {
            g_event_fd = eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE);
        }
        req.count = g_buffer_count;

        int total_fds = g_buffer_count + (g_event_fd >= 0 ? 1 : 0);
        int send_fds[MAX_BUFFERS + 1];
        for (uint32_t i = 0; i < g_buffer_count; i++) {
            send_fds[i] = g_buffer_fds[i];
        }
        if (g_event_fd >= 0) {
            send_fds[g_buffer_count] = g_event_fd;
        }
        
        // Atomic FD send via SCM_RIGHTS over the control message header
        IpcInboundHeader hdr = { .client_fd = client_fd, .payload_size = sizeof(IpcV4l2ReqBufs), .ipc_status = 0 };
        struct msghdr msg = {0};
        struct iovec iov[2] = {
            { &hdr, sizeof(IpcInboundHeader) },
            { &req, sizeof(IpcV4l2ReqBufs) }
        };
        msg.msg_iov = iov; msg.msg_iovlen = 2;
        
        char cmsg_buf[CMSG_SPACE((MAX_BUFFERS + 1) * sizeof(int))] = {0};
        msg.msg_control = cmsg_buf; 
        msg.msg_controllen = CMSG_SPACE(total_fds * sizeof(int));
        
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(total_fds * sizeof(int));
        memcpy(CMSG_DATA(cmsg), send_fds, total_fds * sizeof(int));
        
        sendmsg(socket_fd, &msg, 0);
        return 0;

    } else if (cmd == CMD_QUERYBUF) {
        IpcV4l2Buffer req;
        if (read_exact(socket_fd, &req, sizeof(IpcV4l2Buffer)) != 0) return -1;

        if (req.type != 1 || req.memory != 1 || req.index >= g_buffer_count) {
            v4l2_send_error(socket_fd, client_fd, EINVAL);
            return 0;
        }

        pthread_mutex_lock(&g_v4l2_mutex);
        req.length = g_buffer_lengths[req.index];
        req.offset = req.index * 0x1000000;

        // V4L2_BUF_FLAG_MAPPED (0x01) | V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC (0x2000)
        uint32_t flags = 0x00002001;
        if (g_buffer_state[req.index] == 1) flags |= 0x00000002; // QUEUED
        else if (g_buffer_state[req.index] == 2) flags |= 0x00000004; // DONE
        req.flags = flags;
        pthread_mutex_unlock(&g_v4l2_mutex);

        v4l2_send_reply(socket_fd, client_fd, 0, &req, sizeof(IpcV4l2Buffer));
        return 0;

    } else if (cmd == CMD_QBUF) {
        IpcV4l2Buffer buf;
        if (read_exact(socket_fd, &buf, sizeof(IpcV4l2Buffer)) != 0) return -1;

        if (buf.type != 1 || buf.memory != 1 || buf.index >= g_buffer_count) {
            v4l2_send_error(socket_fd, client_fd, EINVAL);
            return 0;
        }

        pthread_mutex_lock(&g_v4l2_mutex);
        /* Reject queuing a buffer that is not currently in the dequeued state */
        if (g_buffer_state[buf.index] != 0) {
            pthread_mutex_unlock(&g_v4l2_mutex);
            v4l2_send_error(socket_fd, client_fd, EINVAL);
            return 0;
        }
        g_buffer_state[buf.index] = 1;
        pthread_mutex_unlock(&g_v4l2_mutex);

        buf.flags = 0x00002003; // MAPPED | QUEUED | TIMESTAMP_MONOTONIC
        v4l2_send_reply(socket_fd, client_fd, 0, &buf, sizeof(IpcV4l2Buffer));
        return 0;

        pthread_mutex_lock(&g_v4l2_mutex);
        g_buffer_state[buf.index] = 1;
        pthread_mutex_unlock(&g_v4l2_mutex);

        buf.flags = 0x00002003; // MAPPED | QUEUED | TIMESTAMP_MONOTONIC
        v4l2_send_reply(socket_fd, client_fd, 0, &buf, sizeof(IpcV4l2Buffer));
        return 0;

    } else if (cmd == CMD_DQBUF) {
        IpcV4l2Buffer buf;
        if (read_exact(socket_fd, &buf, sizeof(IpcV4l2Buffer)) != 0) return -1;
        v4l2_queue_dqbuf_request(socket_fd, client_fd, &buf);
        return 0; 

    } else if (cmd == CMD_STREAMON || cmd == CMD_STREAMOFF) {
        int type;
        if (read_exact(socket_fd, &type, sizeof(int)) != 0) return -1;

        if (type != 1 /* V4L2_BUF_TYPE_VIDEO_CAPTURE */) {
            v4l2_send_error(socket_fd, client_fd, EINVAL);
            return 0;
        }

        int signal = 0;
        pthread_mutex_lock(&g_v4l2_mutex);
        if (cmd == CMD_STREAMON) {
            /* V4L2 spec: reject streaming without allocated buffers */
            if (g_buffer_count == 0) {
                pthread_mutex_unlock(&g_v4l2_mutex);
                v4l2_send_error(socket_fd, client_fd, EINVAL);
                return 0;
            }

            /* Repeated STREAMON calls while already streaming must succeed as a no-op */
            if (g_is_streaming) {
                pthread_mutex_unlock(&g_v4l2_mutex);
                char ack = 'A';
                v4l2_send_reply(socket_fd, client_fd, 0, &ack, 1);
                return 0;
            }

            g_is_streaming = 1;
            g_frame_sequence = 0;
            signal = 1;

            // Safely drain any leftover wake tokens without blocking if the descriptor is empty
            if (g_event_fd >= 0) {
                struct pollfd pfd = { .fd = g_event_fd, .events = POLLIN };
                while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                    uint64_t drain_val;
                    read(g_event_fd, &drain_val, sizeof(drain_val));
                }
            }
        } else {
            g_is_streaming = 0;
            signal = 2;
            for (uint32_t i = 0; i < g_buffer_count; i++) {
                g_buffer_state[i] = 0;
            }
            if (g_event_fd >= 0) {
                uint64_t wake_val = 1;
                write(g_event_fd, &wake_val, sizeof(wake_val));
            }
            pthread_cond_broadcast(&g_v4l2_cond);
        }
        pthread_mutex_unlock(&g_v4l2_mutex);
        
        char ack = 'A'; 
        v4l2_send_reply(socket_fd, client_fd, 0, &ack, 1);
        return signal;

    } else if (cmd == CMD_POLL) {
        int ready = 0;
        pthread_mutex_lock(&g_v4l2_mutex);
        for (uint32_t i = 0; i < g_buffer_count; i++) {
            if (g_buffer_state[i] == 2) { ready = 1; break; }
        }
        pthread_mutex_unlock(&g_v4l2_mutex);
        v4l2_send_reply(socket_fd, client_fd, 0, &ready, sizeof(int));
        return 0;

    } else if (cmd == CMD_DISCONNECT) {
        g_is_streaming = 0;
        v4l2_free_buffers();
        pthread_mutex_lock(&g_v4l2_mutex);
        pthread_cond_broadcast(&g_v4l2_cond);
        pthread_mutex_unlock(&g_v4l2_mutex);

        char ack[4] = {(char)0xDE, (char)0xAD, (char)0xBE, (char)0xEF};
        v4l2_send_reply(socket_fd, client_fd, 0, ack, 4);
        return 3;

    } else if (cmd == CMD_ENUM_INPUT) {
        IpcV4l2Input req;
        if (read_exact(socket_fd, &req, sizeof(IpcV4l2Input)) != 0) return -1;

        pthread_mutex_lock(&g_v4l2_mutex);
        if (req.index >= (uint32_t)g_camera_count) {
            pthread_mutex_unlock(&g_v4l2_mutex);
            v4l2_send_error(socket_fd, client_fd, EINVAL);
            return 0;
        }

        IpcV4l2Input reply;
        memset(&reply, 0, sizeof(IpcV4l2Input));
        reply.index = req.index;
        reply.type = 2; /* V4L2_INPUT_TYPE_CAMERA */
        snprintf(reply.name, sizeof(reply.name), "%s", g_cameras[req.index].name);
        pthread_mutex_unlock(&g_v4l2_mutex);

        v4l2_send_reply(socket_fd, client_fd, 0, &reply, sizeof(IpcV4l2Input));
        return 0;

    } else if (cmd == CMD_G_INPUT) {
        pthread_mutex_lock(&g_v4l2_mutex);
        uint32_t active = (uint32_t)g_active_camera_idx;
        pthread_mutex_unlock(&g_v4l2_mutex);

        v4l2_send_reply(socket_fd, client_fd, 0, &active, sizeof(uint32_t));
        return 0;

    } else if (cmd == CMD_S_INPUT) {
        uint32_t target_idx;
        if (read_exact(socket_fd, &target_idx, sizeof(uint32_t)) != 0) return -1;

        pthread_mutex_lock(&g_v4l2_mutex);
        /* V4L2 spec: switching video inputs during active streaming must fail with EBUSY */
        if (g_is_streaming) {
            pthread_mutex_unlock(&g_v4l2_mutex);
            v4l2_send_error(socket_fd, client_fd, EBUSY);
            return 0;
        }

        if (target_idx >= (uint32_t)g_camera_count) {
            pthread_mutex_unlock(&g_v4l2_mutex);
            v4l2_send_error(socket_fd, client_fd, EINVAL);
            return 0;
        }

        int already_active = (target_idx == (uint32_t)g_active_camera_idx);
        pthread_mutex_unlock(&g_v4l2_mutex);

        if (!already_active) {
            if (v4l2_switch_camera((int)target_idx) != 0) {
                v4l2_send_error(socket_fd, client_fd, EINVAL);
                return 0;
            }
        }

        int32_t ack = 0;
        v4l2_send_reply(socket_fd, client_fd, 0, &ack, sizeof(ack));
        return already_active ? 0 : 5;
    }

    return 0;
}
