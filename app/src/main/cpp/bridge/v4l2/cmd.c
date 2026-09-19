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
#include <sys/mman.h>
#include <linux/memfd.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <dlfcn.h>
#include <android/log.h>
#include <linux/videodev2.h>

#include "bridge/v4l2/v4l2.h"
#include "bridge/v4l2/internal.h"

#define TAG "vSouchi:V4L2Cmd"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Externs mapped from jni.c
extern int g_has_focus, g_has_exposure;
extern float g_min_focus_dist;
extern long g_min_exposure, g_max_exposure;
extern int g_current_focus_auto, g_current_focus_abs;
extern int g_current_exposure_auto, g_current_exposure_abs;
extern int g_last_ctrl_id, g_last_ctrl_val;

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

int v4l2_handle_command(int socket_fd, uint32_t client_fd, uint32_t cmd, const char* device_name, uint32_t kernel_version) {
    if (cmd == CMD_GET_CAPS) {
        IpcV4l2Caps caps;
        memset(&caps, 0, sizeof(IpcV4l2Caps));
        snprintf(caps.driver, sizeof(caps.driver), "android-v4l2");
        snprintf(caps.card, sizeof(caps.card), "%s", device_name);
        snprintf(caps.bus_info, sizeof(caps.bus_info), "platform:android");
        caps.version = kernel_version;
        caps.capabilities = 0x04000001; 
        
        v4l2_send_reply(socket_fd, client_fd, 0, &caps, sizeof(IpcV4l2Caps));
        return 0;

    } else if (cmd == CMD_ENUM_FMT) {
        IpcV4l2FmtDesc req_fmt;
        read(socket_fd, &req_fmt, sizeof(IpcV4l2FmtDesc));
        
        if (req_fmt.index < (uint32_t)g_supported_format_count) {
            IpcV4l2FmtDesc reply = { .index = req_fmt.index, .type = 1, .pixelformat = g_supported_formats[req_fmt.index].v4l2_fourcc };
            snprintf(reply.description, sizeof(reply.description), "%s", g_supported_formats[req_fmt.index].description);
            v4l2_send_reply(socket_fd, client_fd, 0, &reply, sizeof(IpcV4l2FmtDesc));
        } else {
            req_fmt.pixelformat = 0xFFFFFFFF; // Signal cutoff limit reached to V4L2 tools
            v4l2_send_reply(socket_fd, client_fd, 0, &req_fmt, sizeof(IpcV4l2FmtDesc));
        }
        return 0;

    } else if (cmd == CMD_G_FMT) {
        uint32_t bpl = g_current_width;
        if (g_current_format == v4l2_fourcc('J', 'P', 'E', 'G') || g_current_format == v4l2_fourcc('H', 'E', 'V', 'C')) {
            bpl = 0; // V4L2 spec requires exactly 0 bytes per line for compressed formats
        } else if (g_current_format == v4l2_fourcc('R', 'G', 'G', 'B') || g_current_format == v4l2_fourcc('R', 'G', '1', '0') || g_current_format == v4l2_fourcc('P', '0', '1', '0') || g_current_format == v4l2_fourcc('Y', 'U', 'Y', 'V')) {
            bpl = g_current_width * 2;
        }

        IpcV4l2Format fmt = { 
            .width = g_current_width, .height = g_current_height, 
            .pixelformat = g_current_format, 
            .field = 1, 
            .colorspace = get_correct_colorspace(g_current_width, g_current_height),
            .bytesperline = bpl, 
            .sizeimage = v4l2_get_current_frame_size() 
        };
        v4l2_send_reply(socket_fd, client_fd, 0, &fmt, sizeof(IpcV4l2Format));
        return 0;

    } else if (cmd == CMD_TRY_FMT || cmd == CMD_S_FMT) {
        IpcV4l2Format req_fmt;
        read(socket_fd, &req_fmt, sizeof(IpcV4l2Format));

        uint32_t req_pix = req_fmt.pixelformat;
        int found = 0;
        int target_android_fmt = 35; 
        for (int i = 0; i < g_supported_format_count; i++) {
            if (g_supported_formats[i].v4l2_fourcc == req_pix) {
                found = 1;
                target_android_fmt = g_supported_formats[i].android_format;
                break;
            }
        }

        // V4L2 spec: Drivers must silently override unsupported formats with a default, never return an error
        if (!found && g_supported_format_count > 0) {
            int fallback_idx = 0;
            for (int i = 0; i < g_supported_format_count; i++) {
                if (g_supported_formats[i].v4l2_fourcc == v4l2_fourcc('N', 'V', '1', '2') ||
                    g_supported_formats[i].v4l2_fourcc == v4l2_fourcc('N', 'V', '2', '1')) {
                    fallback_idx = i;
                    break;
                }
            }
            req_pix = g_supported_formats[fallback_idx].v4l2_fourcc;
            target_android_fmt = g_supported_formats[fallback_idx].android_format;
        }

        uint32_t best_w = g_current_width;
        uint32_t best_h = g_current_height;

        if (g_res_count > 0) {
            uint32_t min_diff = 0xFFFFFFFF;
            int has_valid_format = 0;

            for (int i = 0; i < g_res_count; i++) {
                if (g_resolutions[i].android_format == target_android_fmt) {
                    if (!has_valid_format) {
                        best_w = g_resolutions[i].width;
                        best_h = g_resolutions[i].height;
                        has_valid_format = 1;
                    }
                    uint32_t diff = abs((int)g_resolutions[i].width - (int)req_fmt.width) +
                                    abs((int)g_resolutions[i].height - (int)req_fmt.height);
                    if (diff < min_diff) {
                        min_diff = diff;
                        best_w = g_resolutions[i].width;
                        best_h = g_resolutions[i].height;
                    }
                }
            }
        }

        if (cmd == CMD_S_FMT) {
            g_current_width = best_w;
            g_current_height = best_h;
            g_current_format = req_pix;
            g_current_android_format = target_android_fmt; 
            g_current_fps = get_max_fps_for_res(g_current_width, g_current_height);
        }

        req_fmt.width = best_w;   
        req_fmt.height = best_h;  
        req_fmt.pixelformat = req_pix;
        req_fmt.field = 1;
        req_fmt.colorspace = get_correct_colorspace(g_current_width, g_current_height);

        if (req_pix == v4l2_fourcc('J', 'P', 'E', 'G') || req_pix == v4l2_fourcc('H', 'E', 'V', 'C')) {
            req_fmt.bytesperline = 0; 
        } else if (req_pix == v4l2_fourcc('R', 'G', 'G', 'B') || req_pix == v4l2_fourcc('R', 'G', '1', '0') || req_pix == v4l2_fourcc('P', '0', '1', '0') || req_pix == v4l2_fourcc('Y', 'U', 'Y', 'V')) {
            req_fmt.bytesperline = g_current_width * 2;
        } else {
            req_fmt.bytesperline = g_current_width;
        }
        
        req_fmt.sizeimage = v4l2_get_current_frame_size();
        
        v4l2_send_reply(socket_fd, client_fd, 0, &req_fmt, sizeof(IpcV4l2Format));
        return 0;
    
    } else if (cmd == CMD_ENUM_FRAMESIZES) {
        IpcV4l2FrmSizeEnum req;
        read(socket_fd, &req, sizeof(IpcV4l2FrmSizeEnum));
        req.type = 1; 
        
        int target_android_fmt = -1;
        for (int i = 0; i < g_supported_format_count; i++) {
            if (g_supported_formats[i].v4l2_fourcc == req.pixel_format) {
                target_android_fmt = g_supported_formats[i].android_format;
                break;
            }
        }

        int match_count = 0;
        int found_idx = -1;
        
        for (int i = 0; i < g_res_count; i++) {
            if (g_resolutions[i].android_format == target_android_fmt) {
                if (match_count == (int)req.index) {
                    found_idx = i; break;
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
        read(socket_fd, &req, sizeof(IpcV4l2FrmIvalEnum));
        req.type = 1; 
        req.fps_numerator = 0; 
        
        int target_android_fmt = -1;
        for (int i = 0; i < g_supported_format_count; i++) {
            if (g_supported_formats[i].v4l2_fourcc == req.pixel_format) {
                target_android_fmt = g_supported_formats[i].android_format;
                break;
            }
        }

        if (req.index == 0) {
            for (int i = 0; i < g_res_count; i++) {
                if (g_resolutions[i].android_format == target_android_fmt && 
                    g_resolutions[i].width == req.width && 
                    g_resolutions[i].height == req.height) {
                    
                    req.fps_numerator = 1;
                    req.fps_denominator = g_resolutions[i].max_fps; 
                    break;
                }
            }
        }
        v4l2_send_reply(socket_fd, client_fd, 0, &req, sizeof(IpcV4l2FrmIvalEnum));
        return 0;

    } else if (cmd == CMD_CROPCAP) {
        IpcV4l2CropCap crop;
        read(socket_fd, &crop, sizeof(IpcV4l2CropCap));
        
        crop.bounds_left = 0; crop.bounds_top = 0;
        crop.bounds_width = g_current_width; crop.bounds_height = g_current_height;
        crop.defrect_left = 0; crop.defrect_top = 0;
        crop.defrect_width = g_current_width; crop.defrect_height = g_current_height;
        crop.pixelaspect_num = 1; crop.pixelaspect_denom = 1;
        
        v4l2_send_reply(socket_fd, client_fd, 0, &crop, sizeof(IpcV4l2CropCap));
        return 0;

    } else if (cmd == CMD_QUERYCTRL) {
        uint32_t req_id;
        read(socket_fd, &req_id, sizeof(uint32_t));
        
        struct v4l2_queryctrl qc;
        memset(&qc, 0, sizeof(qc));
        
        uint32_t ctrls[5];
        int num_ctrls = 0;
        
        // Expose Camera Class heading first (v4l2-ctl groups them by this)
        ctrls[num_ctrls++] = 0x009a0001; 

        if (g_has_exposure) {
            ctrls[num_ctrls++] = V4L2_CID_EXPOSURE_AUTO;     // 0x009a0901
            ctrls[num_ctrls++] = V4L2_CID_EXPOSURE_ABSOLUTE; // 0x009a0902
        }
        if (g_has_focus) {
            ctrls[num_ctrls++] = V4L2_CID_FOCUS_ABSOLUTE;    // 0x009a090a
            ctrls[num_ctrls++] = V4L2_CID_FOCUS_AUTO;        // 0x009a090c
        }

        uint32_t target_id = 0;
        
        if (req_id & 0x80000000) { 
            // Mask off ALL flags to correctly handle V4L2_CTRL_FLAG_NEXT_CTRL queries
            uint32_t start_id = req_id & 0x0fffffff; 
            
            for (int i = 0; i < num_ctrls; i++) {
                if (ctrls[i] > start_id) {
                    target_id = ctrls[i];
                    break;
                }
            }
        } else {
            for (int i = 0; i < num_ctrls; i++) {
                if (ctrls[i] == req_id) {
                    target_id = ctrls[i];
                    break;
                }
            }
        }

        if (target_id == 0x009a0001) {
            qc.id = target_id;
            qc.type = 6; 
            strcpy((char*)qc.name, "Camera Controls");
        }
        else if (target_id == V4L2_CID_EXPOSURE_AUTO && g_has_exposure) {
            qc.id = target_id;
            qc.type = 3; 
            strcpy((char*)qc.name, "Exposure, Auto");
            qc.minimum = 1; qc.maximum = 3; qc.step = 1; qc.default_value = 3;
        } 
        else if (target_id == V4L2_CID_EXPOSURE_ABSOLUTE && g_has_exposure) {
            qc.id = target_id;
            qc.type = 1; 
            strcpy((char*)qc.name, "Exposure, Absolute");
            qc.minimum = 1; qc.maximum = 5000; qc.step = 1; qc.default_value = 156;
        }
        else if (target_id == V4L2_CID_FOCUS_ABSOLUTE && g_has_focus) {
            qc.id = target_id;
            qc.type = 1; 
            strcpy((char*)qc.name, "Focus, Absolute");
            qc.minimum = 0; qc.maximum = 100; qc.step = 1; qc.default_value = 0;
        }
        else if (target_id == V4L2_CID_FOCUS_AUTO && g_has_focus) {
            qc.id = target_id;
            qc.type = 2; 
            strcpy((char*)qc.name, "Focus, Auto");
            qc.minimum = 0; qc.maximum = 1; qc.step = 1; qc.default_value = 1;
        }

        v4l2_send_reply(socket_fd, client_fd, 0, &qc, sizeof(qc));
        return 0;

    } else if (cmd == CMD_G_CTRL) {
        uint32_t ctrl_id;
        read(socket_fd, &ctrl_id, sizeof(uint32_t));
        
        IpcControlPayload resp = { .id = ctrl_id, .value = 0 };
        
        if (ctrl_id == V4L2_CID_FOCUS_AUTO) resp.value = g_current_focus_auto;
        else if (ctrl_id == V4L2_CID_FOCUS_ABSOLUTE) resp.value = g_current_focus_abs;
        else if (ctrl_id == V4L2_CID_EXPOSURE_AUTO) resp.value = g_current_exposure_auto;
        else if (ctrl_id == V4L2_CID_EXPOSURE_ABSOLUTE) resp.value = g_current_exposure_abs;
        else resp.id = 0; 

        v4l2_send_reply(socket_fd, client_fd, 0, &resp, sizeof(resp));
        return 0;

    } else if (cmd == CMD_S_CTRL) {
        IpcControlPayload payload;
        read(socket_fd, &payload, sizeof(payload));
        
        if (payload.id == V4L2_CID_FOCUS_AUTO) g_current_focus_auto = payload.value;
        else if (payload.id == V4L2_CID_FOCUS_ABSOLUTE) g_current_focus_abs = payload.value;
        else if (payload.id == V4L2_CID_EXPOSURE_AUTO) g_current_exposure_auto = payload.value;
        else if (payload.id == V4L2_CID_EXPOSURE_ABSOLUTE) g_current_exposure_abs = payload.value;

        g_last_ctrl_id = payload.id;
        g_last_ctrl_val = payload.value;

        int32_t ack = 0;
        v4l2_send_reply(socket_fd, client_fd, 0, &ack, sizeof(ack));
        
        return 4; 

    } else if (cmd == CMD_G_PARM || cmd == CMD_S_PARM) {
        IpcV4l2StreamParm req;
        read(socket_fd, &req, sizeof(IpcV4l2StreamParm));
        
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
        read(socket_fd, &req, sizeof(IpcV4l2ReqBufs));
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
        req.count = g_buffer_count;
        
        // Atomic FD send via SCM_RIGHTS over the control message header
        IpcInboundHeader hdr = { .client_fd = client_fd, .payload_size = sizeof(IpcV4l2ReqBufs), .ipc_status = 0 };
        struct msghdr msg = {0};
        struct iovec iov[2] = {
            { &hdr, sizeof(IpcInboundHeader) },
            { &req, sizeof(IpcV4l2ReqBufs) }
        };
        msg.msg_iov = iov; msg.msg_iovlen = 2;
        
        char cmsg_buf[CMSG_SPACE(MAX_BUFFERS * sizeof(int))] = {0};
        msg.msg_control = cmsg_buf; 
        msg.msg_controllen = CMSG_SPACE(g_buffer_count * sizeof(int));
        
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(g_buffer_count * sizeof(int));
        memcpy(CMSG_DATA(cmsg), g_buffer_fds, g_buffer_count * sizeof(int));
        
        sendmsg(socket_fd, &msg, 0);
        return 0;

    } else if (cmd == CMD_QUERYBUF) {
        IpcV4l2Buffer req;
        read(socket_fd, &req, sizeof(IpcV4l2Buffer));
        if (req.index < g_buffer_count) {
            req.length = g_buffer_lengths[req.index];
            req.offset = req.index * 0x1000000;
            req.flags = 0x00000001; 
        } else {
            req.length = 0;
        }
        v4l2_send_reply(socket_fd, client_fd, 0, &req, sizeof(IpcV4l2Buffer));
        return 0;

    } else if (cmd == CMD_QBUF) {
        IpcV4l2Buffer buf;
        read(socket_fd, &buf, sizeof(IpcV4l2Buffer));
        pthread_mutex_lock(&g_v4l2_mutex);
        if (buf.index < g_buffer_count) g_buffer_state[buf.index] = 1; 
        pthread_mutex_unlock(&g_v4l2_mutex);
        buf.flags |= 0x00000002;
        v4l2_send_reply(socket_fd, client_fd, 0, &buf, sizeof(IpcV4l2Buffer));
        return 0;

    } else if (cmd == CMD_DQBUF) {
        IpcV4l2Buffer buf;
        read(socket_fd, &buf, sizeof(IpcV4l2Buffer));
        
        v4l2_queue_dqbuf_request(socket_fd, client_fd, &buf);
        return 0; 

    } else if (cmd == CMD_STREAMON || cmd == CMD_STREAMOFF) {
        int type;
        read(socket_fd, &type, sizeof(int));
        g_is_streaming = (cmd == CMD_STREAMON) ? 1 : 0;
        
        if (cmd == CMD_STREAMOFF) {
            pthread_mutex_lock(&g_v4l2_mutex);
            pthread_cond_broadcast(&g_v4l2_cond);
            pthread_mutex_unlock(&g_v4l2_mutex);
        }
        
        char ack = 'A'; 
        v4l2_send_reply(socket_fd, client_fd, 0, &ack, 1);
        return (cmd == CMD_STREAMON) ? 1 : 2; 

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
        char ack[4] = {(char)0xDE, (char)0xAD, (char)0xBE, (char)0xEF};
        v4l2_send_reply(socket_fd, client_fd, 0, ack, 4);
        return 3;
    }

    return 0;
}
