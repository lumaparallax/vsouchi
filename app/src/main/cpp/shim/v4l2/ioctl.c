/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Handles the massive VIDIOC_* translation layer for the virtual camera.
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <sys/time.h>
#include "shim/internal.h"
#include "vsouchi_ipc.h"

extern int g_shim_memfds[16];

// Helper exposed from ipc.c to safely write directly during complex transactions
extern int safe_write(int fd, const void *buf, size_t count);
extern int safe_read(int fd, void *buf, size_t count);

int shim_v4l2_handle_ioctl(int fd, int request, void *arg) {
    uint32_t req_clean = (uint32_t)request;

    if (req_clean == (uint32_t)VIDIOC_QUERYCAP) {
        IpcV4l2Caps ipc_caps;
        if (shim_ipc_transaction(CMD_GET_CAPS, NULL, 0, &ipc_caps, sizeof(IpcV4l2Caps)) != 0) return -1;

        struct v4l2_capability *cap = (struct v4l2_capability *)arg;
        memset(cap, 0, sizeof(*cap));
        strncpy((char *)cap->driver, ipc_caps.driver, sizeof(cap->driver));
        strncpy((char *)cap->card, ipc_caps.card, sizeof(cap->card));
        strncpy((char *)cap->bus_info, ipc_caps.bus_info, sizeof(cap->bus_info));
        cap->version = ipc_caps.version;
        cap->capabilities = ipc_caps.capabilities;
        return 0; 
    }

    if (req_clean == (uint32_t)VIDIOC_ENUMINPUT) {
        struct v4l2_input *inp = (struct v4l2_input *)arg;
        if (inp->index > 0) { errno = EINVAL; return -1; }

        IpcV4l2Caps ipc_caps;
        if (shim_ipc_transaction(CMD_GET_CAPS, NULL, 0, &ipc_caps, sizeof(IpcV4l2Caps)) != 0) return -1;

        memset(inp, 0, sizeof(*inp));
        inp->index = 0;
        strncpy((char *)inp->name, ipc_caps.card, sizeof(inp->name) - 1);
        inp->type = V4L2_INPUT_TYPE_CAMERA;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_G_INPUT || req_clean == (uint32_t)VIDIOC_S_INPUT) {
        int *index = (int *)arg;
        if (req_clean == (uint32_t)VIDIOC_S_INPUT && *index != 0) { errno = EINVAL; return -1; }
        *index = 0;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_CROPCAP) {
        struct v4l2_cropcap *crop = (struct v4l2_cropcap *)arg;
        IpcV4l2CropCap ipc_req = { .type = crop->type };
        
        if (shim_ipc_transaction(CMD_CROPCAP, &ipc_req, sizeof(IpcV4l2CropCap), &ipc_req, sizeof(IpcV4l2CropCap)) != 0) return -1;
        
        crop->bounds.left = ipc_req.bounds_left;
        crop->bounds.top = ipc_req.bounds_top;
        crop->bounds.width = ipc_req.bounds_width;
        crop->bounds.height = ipc_req.bounds_height;
        crop->defrect.left = ipc_req.defrect_left;
        crop->defrect.top = ipc_req.defrect_top;
        crop->defrect.width = ipc_req.defrect_width;
        crop->defrect.height = ipc_req.defrect_height;
        crop->pixelaspect.numerator = ipc_req.pixelaspect_num;
        crop->pixelaspect.denominator = ipc_req.pixelaspect_denom;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_G_PARM || req_clean == (uint32_t)VIDIOC_S_PARM) {
        struct v4l2_streamparm *parm = (struct v4l2_streamparm *)arg;
        uint32_t cmd = (req_clean == (uint32_t)VIDIOC_G_PARM) ? CMD_G_PARM : CMD_S_PARM;
        
        IpcV4l2StreamParm ipc_req;
        memset(&ipc_req, 0, sizeof(IpcV4l2StreamParm));
        
        if (cmd == CMD_S_PARM && parm->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
            ipc_req.fps_numerator = parm->parm.capture.timeperframe.numerator;
            ipc_req.fps_denominator = parm->parm.capture.timeperframe.denominator;
        }

        if (shim_ipc_transaction(cmd, &ipc_req, sizeof(IpcV4l2StreamParm), &ipc_req, sizeof(IpcV4l2StreamParm)) != 0) return -1;

        memset(parm, 0, sizeof(*parm));
        parm->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm->parm.capture.timeperframe.numerator = ipc_req.fps_numerator;
        parm->parm.capture.timeperframe.denominator = ipc_req.fps_denominator;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_ENUM_FMT) {
        struct v4l2_fmtdesc *fmt = (struct v4l2_fmtdesc *)arg;
        IpcV4l2FmtDesc ipc_fmt;
        memset(&ipc_fmt, 0, sizeof(IpcV4l2FmtDesc));
        ipc_fmt.index = fmt->index;

        if (shim_ipc_transaction(CMD_ENUM_FMT, &ipc_fmt, sizeof(IpcV4l2FmtDesc), &ipc_fmt, sizeof(IpcV4l2FmtDesc)) != 0) return -1;
        if (ipc_fmt.pixelformat == 0xFFFFFFFF) { errno = EINVAL; return -1; }

        fmt->flags = ipc_fmt.flags;
        strncpy((char *)fmt->description, ipc_fmt.description, sizeof(fmt->description));
        fmt->pixelformat = ipc_fmt.pixelformat;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_G_FMT || req_clean == (uint32_t)VIDIOC_S_FMT || req_clean == (uint32_t)VIDIOC_TRY_FMT) {
        struct v4l2_format *f = (struct v4l2_format *)arg;
        if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
            uint32_t cmd = (req_clean == (uint32_t)VIDIOC_G_FMT) ? CMD_G_FMT :
                           (req_clean == (uint32_t)VIDIOC_S_FMT) ? CMD_S_FMT : CMD_TRY_FMT;
            
            IpcV4l2Format send_fmt;
            memset(&send_fmt, 0, sizeof(IpcV4l2Format));
            
            if (cmd != CMD_G_FMT) {
                send_fmt.width = f->fmt.pix.width;
                send_fmt.height = f->fmt.pix.height;
                send_fmt.pixelformat = f->fmt.pix.pixelformat;
            }

            IpcV4l2Format ipc_fmt;
            void* payload_ptr = (cmd == CMD_G_FMT) ? NULL : &send_fmt;
            size_t payload_sz = (cmd == CMD_G_FMT) ? 0 : sizeof(IpcV4l2Format);

            if (shim_ipc_transaction(cmd, payload_ptr, payload_sz, &ipc_fmt, sizeof(IpcV4l2Format)) != 0) return -1;

            f->fmt.pix.width = ipc_fmt.width;
            f->fmt.pix.height = ipc_fmt.height;
            f->fmt.pix.pixelformat = ipc_fmt.pixelformat;
            f->fmt.pix.bytesperline = ipc_fmt.bytesperline;
            f->fmt.pix.sizeimage = ipc_fmt.sizeimage;
            f->fmt.pix.field = ipc_fmt.field;
            f->fmt.pix.colorspace = ipc_fmt.colorspace;
            f->fmt.pix.priv = 0;
            return 0; 
        }
    }

    if (req_clean == (uint32_t)VIDIOC_ENUM_FRAMESIZES) {
        struct v4l2_frmsizeenum *fsize = (struct v4l2_frmsizeenum *)arg;
        IpcV4l2FrmSizeEnum ipc_req;
        memset(&ipc_req, 0, sizeof(IpcV4l2FrmSizeEnum));
        ipc_req.index = fsize->index;
        ipc_req.pixel_format = fsize->pixel_format;

        if (shim_ipc_transaction(CMD_ENUM_FRAMESIZES, &ipc_req, sizeof(IpcV4l2FrmSizeEnum), &ipc_req, sizeof(IpcV4l2FrmSizeEnum)) != 0) return -1;
        if (ipc_req.width == 0) { errno = EINVAL; return -1; }
        
        fsize->type = ipc_req.type;
        fsize->discrete.width = ipc_req.width;
        fsize->discrete.height = ipc_req.height;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_ENUM_FRAMEINTERVALS) {
        struct v4l2_frmivalenum *fival = (struct v4l2_frmivalenum *)arg;
        IpcV4l2FrmIvalEnum ipc_req;
        memset(&ipc_req, 0, sizeof(IpcV4l2FrmIvalEnum));
        ipc_req.index = fival->index;
        ipc_req.width = fival->width;   
        ipc_req.height = fival->height; 
        
        if (shim_ipc_transaction(CMD_ENUM_FRAMEINTERVALS, &ipc_req, sizeof(IpcV4l2FrmIvalEnum), &ipc_req, sizeof(IpcV4l2FrmIvalEnum)) != 0) return -1;
        if (ipc_req.fps_numerator == 0) { errno = EINVAL; return -1; }
        
        fival->type = ipc_req.type;
        fival->discrete.numerator = ipc_req.fps_numerator;
        fival->discrete.denominator = ipc_req.fps_denominator;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *req = (struct v4l2_requestbuffers *)arg;
        
        IpcV4l2ReqBufs ipc_req;
        memset(&ipc_req, 0, sizeof(IpcV4l2ReqBufs));
        ipc_req.count = req->count;
        ipc_req.type = req->type;
        ipc_req.memory = req->memory;

        pthread_mutex_lock(&g_ipc_mutex);
        shim_clear_memfds();
        
        uint32_t cmd = CMD_REQBUFS;
        struct iovec iov[2] = { { &cmd, sizeof(uint32_t) }, { &ipc_req, sizeof(IpcV4l2ReqBufs) } };
        
        if (writev(g_broker_fd, iov, 2) <= 0) {
            pthread_mutex_unlock(&g_ipc_mutex);
            errno = EIO; return -1;
        }

        int32_t ipc_status = 0;
        if (safe_read(g_broker_fd, &ipc_status, sizeof(int32_t)) <= 0 || ipc_status < 0) {
            pthread_mutex_unlock(&g_ipc_mutex);
            errno = (ipc_status < 0) ? -ipc_status : EIO; 
            return -1;
        }

        if (safe_read(g_broker_fd, &ipc_req, sizeof(IpcV4l2ReqBufs)) <= 0) {
            pthread_mutex_unlock(&g_ipc_mutex);
            errno = EIO; return -1;
        }

        req->count = ipc_req.count;
        req->capabilities = V4L2_BUF_CAP_SUPPORTS_MMAP;
        
        for (uint32_t i = 0; i < ipc_req.count; i++) { 
            char ack = 'R';
            safe_write(g_broker_fd, &ack, 1); 
            int received_fd = recv_single_fd(g_broker_fd);
            if (received_fd >= 0) {
                g_shim_memfds[i] = received_fd;
            }
        }
        pthread_mutex_unlock(&g_ipc_mutex);
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_QUERYBUF) {
        struct v4l2_buffer *buf = (struct v4l2_buffer *)arg;
        IpcV4l2Buffer ipc_req;
        memset(&ipc_req, 0, sizeof(IpcV4l2Buffer));
        ipc_req.index = buf->index;

        if (shim_ipc_transaction(CMD_QUERYBUF, &ipc_req, sizeof(IpcV4l2Buffer), &ipc_req, sizeof(IpcV4l2Buffer)) != 0) return -1;
        if (ipc_req.length == 0) { errno = EINVAL; return -1; }

        buf->length = ipc_req.length;
        buf->m.offset = ipc_req.offset;
        buf->flags = ipc_req.flags;
        buf->memory = V4L2_MEMORY_MMAP;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_QBUF || req_clean == (uint32_t)VIDIOC_DQBUF) {
        struct v4l2_buffer *buf = (struct v4l2_buffer *)arg;
        uint32_t cmd = (req_clean == (uint32_t)VIDIOC_QBUF) ? CMD_QBUF : CMD_DQBUF;

        IpcV4l2Buffer ipc_req;
        memset(&ipc_req, 0, sizeof(IpcV4l2Buffer));
        ipc_req.index = buf->index;
        ipc_req.type = buf->type;
        ipc_req.memory = buf->memory;

        if (shim_ipc_transaction(cmd, &ipc_req, sizeof(IpcV4l2Buffer), &ipc_req, sizeof(IpcV4l2Buffer)) != 0) return -1;
        if (ipc_req.length == 0xFFFFFFFF) { errno = EINVAL; return -1; }

        buf->index = ipc_req.index;
        buf->bytesused = ipc_req.bytesused;
        buf->length = ipc_req.length; 
        buf->flags = ipc_req.flags;
        
        if (req_clean == (uint32_t)VIDIOC_DQBUF) {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            buf->timestamp = tv;
        }
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_STREAMON || req_clean == (uint32_t)VIDIOC_STREAMOFF) {
        int *type = (int *)arg;
        uint32_t cmd = (req_clean == (uint32_t)VIDIOC_STREAMON) ? CMD_STREAMON : CMD_STREAMOFF;
        char ack;
        if (shim_ipc_transaction(cmd, type, sizeof(int), &ack, 1) != 0) return -1;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_QUERYCTRL) {
        struct v4l2_queryctrl *ctrl = (struct v4l2_queryctrl *)arg;
        uint32_t req_id = ctrl->id;
        struct v4l2_queryctrl response;
        memset(&response, 0, sizeof(response));

        if (shim_ipc_transaction(CMD_QUERYCTRL, &req_id, sizeof(uint32_t), &response, sizeof(struct v4l2_queryctrl)) != 0) return -1;
        if (response.id == 0) { errno = EINVAL; return -1; }
        
        memcpy(ctrl, &response, sizeof(struct v4l2_queryctrl));
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_G_CTRL) {
        struct v4l2_control *ctrl = (struct v4l2_control *)arg;
        uint32_t req_id = ctrl->id;
        IpcControlPayload response;
        memset(&response, 0, sizeof(response));

        if (shim_ipc_transaction(CMD_G_CTRL, &req_id, sizeof(uint32_t), &response, sizeof(IpcControlPayload)) != 0) return -1;
        if (response.id == 0) { errno = EINVAL; return -1; }
        
        ctrl->value = response.value;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_S_CTRL) {
        struct v4l2_control *ctrl = (struct v4l2_control *)arg;
        IpcControlPayload payload = { .id = ctrl->id, .value = ctrl->value };
        int32_t ack = 0;
        if (shim_ipc_transaction(CMD_S_CTRL, &payload, sizeof(IpcControlPayload), &ack, sizeof(int32_t)) != 0) return -1;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_G_EXT_CTRLS || req_clean == (uint32_t)VIDIOC_S_EXT_CTRLS || req_clean == (uint32_t)VIDIOC_TRY_EXT_CTRLS) {
        struct v4l2_ext_controls *ext = (struct v4l2_ext_controls *)arg;
        for (uint32_t i = 0; i < ext->count; i++) {
            struct v4l2_ext_control *ctrl = &ext->controls[i];
            uint32_t req_id = ctrl->id;
            
            if (req_clean == (uint32_t)VIDIOC_G_EXT_CTRLS || req_clean == (uint32_t)VIDIOC_TRY_EXT_CTRLS) {
                IpcControlPayload response;
                memset(&response, 0, sizeof(response));

                if (shim_ipc_transaction(CMD_G_CTRL, &req_id, sizeof(uint32_t), &response, sizeof(IpcControlPayload)) != 0) {
                    ext->error_idx = i; return -1; 
                }
                if (response.id == 0) {
                    ext->error_idx = i; errno = EINVAL; return -1;
                }
                if (req_clean == (uint32_t)VIDIOC_G_EXT_CTRLS) ctrl->value = response.value;
            } 
            else if (req_clean == (uint32_t)VIDIOC_S_EXT_CTRLS) {
                IpcControlPayload payload = { .id = ctrl->id, .value = ctrl->value };
                int32_t ack = 0;
                if (shim_ipc_transaction(CMD_S_CTRL, &payload, sizeof(IpcControlPayload), &ack, sizeof(int32_t)) != 0) {
                    ext->error_idx = i; return -1;
                }
            }
        }
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_QUERYMENU) {
        struct v4l2_querymenu *menu = (struct v4l2_querymenu *)arg;
        if (menu->id == 0x009a0901) {
            if (menu->index == 1) { strcpy((char*)menu->name, "Manual Mode"); return 0; }
            if (menu->index == 3) { strcpy((char*)menu->name, "Aperture Priority Mode"); return 0; }
        }
        errno = EINVAL;
        return -1;
    }

    if (req_clean == (uint32_t)VIDIOC_G_STD || req_clean == (uint32_t)VIDIOC_S_STD || req_clean == (uint32_t)VIDIOC_ENUMSTD) {
        errno = EINVAL; 
        return -1;
    }
    
    errno = ENOTTY; 
    return -1;
}
