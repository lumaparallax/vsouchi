/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Handles the massive VIDIOC_* translation layer for the virtual camera.
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <sys/time.h>
#include "shim/internal.h"
#include "vsouchi_ipc.h"

#ifndef V4L2_CTRL_WHICH_DEF_VAL
#define V4L2_CTRL_WHICH_DEF_VAL 0x0f000000
#endif
#ifndef V4L2_CTRL_WHICH_REQUEST_VAL
#define V4L2_CTRL_WHICH_REQUEST_VAL 0x0f010000
#endif
#ifndef V4L2_CTRL_WHICH_MIN_VAL
#define V4L2_CTRL_WHICH_MIN_VAL 0x0f020000
#endif
#ifndef V4L2_CTRL_WHICH_MAX_VAL
#define V4L2_CTRL_WHICH_MAX_VAL 0x0f030000
#endif
#ifndef V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS
#define V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS 0x00000010
#endif

#define MAX_EVENTS_PER_FD 16
#define MAX_SUBS_PER_FD 16

#define MAX_VSOUCHI_MINORS 64

extern int g_shim_memfds[16];
extern int g_vsouchi_fd_flags[1024];
extern int g_vsouchi_fd_minor[1024];

// Helper exposed from ipc.c to safely write directly during complex transactions
extern int safe_write(int fd, const void *buf, size_t count);
extern int safe_read(int fd, void *buf, size_t count);

static int g_prio_owner_fd = -1;
static enum v4l2_priority g_current_prio = V4L2_PRIORITY_DEFAULT;

typedef struct {
    volatile int is_streaming;
    int buffer_owner_fd;
    enum v4l2_priority current_prio;
    int prio_owner_fd;
} vSouchiDeviceNode;

static vSouchiDeviceNode g_nodes[MAX_VSOUCHI_MINORS];
static int g_nodes_initialized = 0;

static inline int get_minor(int fd) {
    if (!g_nodes_initialized) {
        for (int i = 0; i < MAX_VSOUCHI_MINORS; i++) {
            g_nodes[i].buffer_owner_fd = -1;
            g_nodes[i].prio_owner_fd = -1;
            g_nodes[i].current_prio = V4L2_PRIORITY_DEFAULT;
            g_nodes[i].is_streaming = 0;
        }
        g_nodes_initialized = 1;
    }
    if (fd >= 0 && fd < 1024) return g_vsouchi_fd_minor[fd];
    return 0;
}

int shim_v4l2_check_and_clear_buffer_owner(int fd) {
    if (fd < 0 || fd >= 1024) return 0;
    int minor = get_minor(fd);
    if (g_nodes[minor].buffer_owner_fd == fd) {
        g_nodes[minor].buffer_owner_fd = -1;
        g_nodes[minor].is_streaming = 0;
        return 1;
    }
    return 0;
}

typedef struct {
    uint32_t id;
    uint32_t flags;
} EventSub;

typedef struct {
    EventSub subs[MAX_SUBS_PER_FD];
    int num_subs;
    struct v4l2_event events[MAX_EVENTS_PER_FD];
    int head;
    int tail;
    int count;
} FdEventState;

static FdEventState g_fd_events[1024];
static pthread_mutex_t g_event_lock = PTHREAD_MUTEX_INITIALIZER;

static int is_valid_control_id(uint32_t id) {
    if (id == 0x00980001 || id == 0x009a0001) return 1;
    if (id == 0x00980914 || id == 0x00980915 || id == 0x00980922) return 1;
    if (id == 0x009a0901 || id == 0x009a0902 || id == 0x009a090a || id == 0x009a090c) return 1;
    return 0;
}

static int is_class_control(uint32_t id) {
    return (id == 0x00980001 || id == 0x009a0001);
}

int shim_v4l2_has_event(int fd) {
    if (fd < 0 || fd >= 1024) return 0;
    pthread_mutex_lock(&g_event_lock);
    int has = (g_fd_events[fd].count > 0);
    pthread_mutex_unlock(&g_event_lock);
    return has;
}

void shim_v4l2_event_cleanup(int fd) {
    if (fd < 0 || fd >= 1024) return;
    pthread_mutex_lock(&g_event_lock);
    g_fd_events[fd].num_subs = 0;
    g_fd_events[fd].head = 0;
    g_fd_events[fd].tail = 0;
    g_fd_events[fd].count = 0;
    pthread_mutex_unlock(&g_event_lock);
}

static int shim_v4l2_subscribe_event(int fd, struct v4l2_event_subscription *sub) {
    if (fd < 0 || fd >= 1024) { errno = EBADF; return -1; }
    if (sub->type != V4L2_EVENT_CTRL) { errno = EINVAL; return -1; }
    if (!is_valid_control_id(sub->id)) { errno = EINVAL; return -1; }

    pthread_mutex_lock(&g_event_lock);
    FdEventState *st = &g_fd_events[fd];

    int sub_idx = -1;
    for (int i = 0; i < st->num_subs; i++) {
        if (st->subs[i].id == sub->id) {
            sub_idx = i;
            break;
        }
    }
    if (sub_idx == -1) {
        if (st->num_subs >= MAX_SUBS_PER_FD) {
            pthread_mutex_unlock(&g_event_lock);
            errno = ENOMEM;
            return -1;
        }
        sub_idx = st->num_subs++;
    }
    st->subs[sub_idx].id = sub->id;
    st->subs[sub_idx].flags = sub->flags;

    // V4L2 spec: class controls never fire events, even if SEND_INITIAL is requested
    if ((sub->flags & V4L2_EVENT_SUB_FL_SEND_INITIAL) && !is_class_control(sub->id)) {
        if (st->count < MAX_EVENTS_PER_FD) {
            struct v4l2_event *ev = &st->events[st->tail];
            memset(ev, 0, sizeof(*ev));
            ev->type = V4L2_EVENT_CTRL;
            ev->id = sub->id;
            ev->u.ctrl.changes = V4L2_EVENT_CTRL_CH_VALUE;

            uint32_t qid = sub->id;
            IpcControlPayload resp;
            if (shim_ipc_transaction(CMD_G_CTRL, &qid, sizeof(uint32_t), &resp, sizeof(resp)) == 0) {
                ev->u.ctrl.value = resp.value;
            }

            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ev->timestamp = ts;

            st->tail = (st->tail + 1) % MAX_EVENTS_PER_FD;
            st->count++;
        }
    }

    pthread_mutex_unlock(&g_event_lock);
    return 0;
}

static int shim_v4l2_unsubscribe_event(int fd, struct v4l2_event_subscription *sub) {
    if (fd < 0 || fd >= 1024) { errno = EBADF; return -1; }
    if (sub->type != V4L2_EVENT_CTRL) { errno = EINVAL; return -1; }

    pthread_mutex_lock(&g_event_lock);
    FdEventState *st = &g_fd_events[fd];

    int found = 0;
    for (int i = 0; i < st->num_subs; i++) {
        if (st->subs[i].id == sub->id || sub->id == 0 /* V4L2_EVENT_ALL */) {
            for (int j = i; j < st->num_subs - 1; j++) {
                st->subs[j] = st->subs[j + 1];
            }
            st->num_subs--;
            i--;
            found = 1;
            if (sub->id != 0) break;
        }
    }

    if (!found) {
        pthread_mutex_unlock(&g_event_lock);
        errno = EINVAL;
        return -1;
    }

    // Flush remaining queued events for this subscription
    int new_count = 0;
    struct v4l2_event new_events[MAX_EVENTS_PER_FD];
    for (int i = 0; i < st->count; i++) {
        int idx = (st->head + i) % MAX_EVENTS_PER_FD;
        if (st->events[idx].id != sub->id && sub->id != 0) {
            new_events[new_count++] = st->events[idx];
        }
    }
    for (int i = 0; i < new_count; i++) {
        st->events[i] = new_events[i];
    }
    st->head = 0;
    st->tail = new_count;
    st->count = new_count;

    pthread_mutex_unlock(&g_event_lock);
    return 0;
}

static int shim_v4l2_dqevent(int fd, struct v4l2_event *ev) {
    if (fd < 0 || fd >= 1024) { errno = EBADF; return -1; }
    pthread_mutex_lock(&g_event_lock);
    FdEventState *st = &g_fd_events[fd];

    if (st->count == 0) {
        pthread_mutex_unlock(&g_event_lock);
        errno = ENOENT;
        return -1;
    }

    *ev = st->events[st->head];
    st->head = (st->head + 1) % MAX_EVENTS_PER_FD;
    st->count--;

    pthread_mutex_unlock(&g_event_lock);
    return 0;
}

int shim_v4l2_handle_ioctl(int fd, int request, void *arg) {
    uint32_t req_clean = (uint32_t)request;

    if (req_clean == (uint32_t)VIDIOC_G_STD || req_clean == (uint32_t)VIDIOC_S_STD ||
        req_clean == (uint32_t)VIDIOC_ENUMSTD || req_clean == (uint32_t)VIDIOC_QUERYSTD) {
        errno = ENOTTY;
        return -1;
    }

    if (req_clean == (uint32_t)VIDIOC_CROPCAP || req_clean == (uint32_t)VIDIOC_G_CROP ||
        req_clean == (uint32_t)VIDIOC_S_CROP || req_clean == (uint32_t)VIDIOC_G_SELECTION ||
        req_clean == (uint32_t)VIDIOC_S_SELECTION) {
        errno = ENOTTY;
        return -1;
    }

    #define CHECK_ARG() do { if (!arg) { errno = EFAULT; return -1; } } while(0)

    if (req_clean == (uint32_t)VIDIOC_QUERYCAP) {
        CHECK_ARG();
        IpcV4l2Caps ipc_caps;
        if (shim_ipc_transaction(CMD_GET_CAPS, NULL, 0, &ipc_caps, sizeof(IpcV4l2Caps)) != 0) return -1;

        struct v4l2_capability *cap = (struct v4l2_capability *)arg;
        memset(cap, 0, sizeof(*cap));
        strncpy((char *)cap->driver, ipc_caps.driver, sizeof(cap->driver) - 1);
        strncpy((char *)cap->card, ipc_caps.card, sizeof(cap->card) - 1);
        strncpy((char *)cap->bus_info, ipc_caps.bus_info, sizeof(cap->bus_info) - 1);
        cap->version = ipc_caps.version;
        cap->capabilities = ipc_caps.capabilities;
        cap->device_caps = ipc_caps.device_caps;
        return 0; 
    }

    if (req_clean == (uint32_t)VIDIOC_ENUMINPUT) {
        CHECK_ARG();
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
        CHECK_ARG();
        int *index = (int *)arg;
        if (req_clean == (uint32_t)VIDIOC_S_INPUT && *index != 0) { errno = EINVAL; return -1; }
        *index = 0;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_CROPCAP) {
        CHECK_ARG();
        struct v4l2_cropcap *crop = (struct v4l2_cropcap *)arg;
        if (crop->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
            errno = EINVAL;
            return -1;
        }
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
        CHECK_ARG();
        struct v4l2_streamparm *parm = (struct v4l2_streamparm *)arg;
        if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
            errno = EINVAL;
            return -1;
        }

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
        parm->parm.capture.capability = 0x1000; // V4L2_CAP_TIMEPERFRAME
        parm->parm.capture.timeperframe.numerator = ipc_req.fps_numerator;
        parm->parm.capture.timeperframe.denominator = ipc_req.fps_denominator;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_ENUM_FMT) {
        CHECK_ARG();
        struct v4l2_fmtdesc *f = (struct v4l2_fmtdesc *)arg;
        IpcV4l2FmtDesc ipc_req = { .index = f->index, .type = f->type };

        if (shim_ipc_transaction(CMD_ENUM_FMT, &ipc_req, sizeof(IpcV4l2FmtDesc), &ipc_req, sizeof(IpcV4l2FmtDesc)) != 0) {
            return -1;
        }

        f->flags = ipc_req.flags;
        f->pixelformat = ipc_req.pixelformat;
        strncpy((char *)f->description, ipc_req.description, sizeof(f->description) - 1);
        memset(f->reserved, 0, sizeof(f->reserved));
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_G_FMT || req_clean == (uint32_t)VIDIOC_S_FMT || req_clean == (uint32_t)VIDIOC_TRY_FMT) {
        CHECK_ARG();
        struct v4l2_format *f = (struct v4l2_format *)arg;
        if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
            errno = EINVAL;
            return -1;
        }

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
        f->fmt.pix.ycbcr_enc = 0; // V4L2_YCBCR_ENC_DEFAULT
        f->fmt.pix.quantization = 0; // V4L2_QUANTIZATION_DEFAULT
        f->fmt.pix.xfer_func = 0; // V4L2_XFER_FUNC_DEFAULT
        f->fmt.pix.priv = 0;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_ENUM_FRAMESIZES) {
        CHECK_ARG();
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
        memset(fsize->reserved, 0, sizeof(fsize->reserved));
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_ENUM_FRAMEINTERVALS) {
        CHECK_ARG();
        struct v4l2_frmivalenum *fival = (struct v4l2_frmivalenum *)arg;
        IpcV4l2FrmIvalEnum ipc_req;
        memset(&ipc_req, 0, sizeof(IpcV4l2FrmIvalEnum));
        ipc_req.index = fival->index;
        ipc_req.pixel_format = fival->pixel_format;
        ipc_req.width = fival->width;   
        ipc_req.height = fival->height; 

        if (shim_ipc_transaction(CMD_ENUM_FRAMEINTERVALS, &ipc_req, sizeof(IpcV4l2FrmIvalEnum), &ipc_req, sizeof(IpcV4l2FrmIvalEnum)) != 0) return -1;
        if (ipc_req.fps_numerator == 0) { errno = EINVAL; return -1; }

        fival->type = ipc_req.type;
        fival->discrete.numerator = ipc_req.fps_numerator;
        fival->discrete.denominator = ipc_req.fps_denominator;
        memset(fival->reserved, 0, sizeof(fival->reserved));
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_REQBUFS) {
        CHECK_ARG();
        struct v4l2_requestbuffers *req = (struct v4l2_requestbuffers *)arg;
        
        if (req->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || req->memory != V4L2_MEMORY_MMAP) {
            errno = EINVAL;
            return -1;
        }

        int minor = get_minor(fd);

        if (g_nodes[minor].buffer_owner_fd != -1 && g_nodes[minor].buffer_owner_fd != fd) {
            errno = EBUSY;
            return -1;
        }

        IpcV4l2ReqBufs ipc_req;
        memset(&ipc_req, 0, sizeof(IpcV4l2ReqBufs));
        ipc_req.count = req->count;
        ipc_req.type = req->type;
        ipc_req.memory = req->memory;

        pthread_mutex_lock(&g_ipc_mutex);
        shim_clear_memfds();
        
        uint32_t cmd = CMD_REQBUFS;
        struct iovec iov[2] = { { &cmd, sizeof(uint32_t) }, { &ipc_req, sizeof(IpcV4l2ReqBufs) } };
        
        struct msghdr msg = {0};
        msg.msg_iov = iov;
        msg.msg_iovlen = 2;

        if (sendmsg(g_broker_fd, &msg, MSG_NOSIGNAL) <= 0) {
            pthread_mutex_unlock(&g_ipc_mutex);
            errno = ENODEV; return -1;
        }

        int32_t ipc_status = 0;
        if (safe_read(g_broker_fd, &ipc_status, sizeof(int32_t)) <= 0) {
            pthread_mutex_unlock(&g_ipc_mutex);
            errno = ENODEV; return -1;
        }

        if (ipc_status < 0) {
            pthread_mutex_unlock(&g_ipc_mutex);
            errno = -ipc_status;
            return -1;
        }

        if (safe_read(g_broker_fd, &ipc_req, sizeof(IpcV4l2ReqBufs)) <= 0) {
            pthread_mutex_unlock(&g_ipc_mutex);
            errno = EIO; return -1;
        }

        req->count = ipc_req.count;
        /* Driver safely orphans active mappings on REQBUFS(0) until munmap() */
        req->capabilities = V4L2_BUF_CAP_SUPPORTS_MMAP | V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS;
        req->flags = 0;
        memset(req->reserved, 0, sizeof(req->reserved));
        
        for (uint32_t i = 0; i < ipc_req.count; i++) { 
            char ack = 'R';
            safe_write(g_broker_fd, &ack, 1); 
            int received_fd = recv_single_fd(g_broker_fd);
            if (received_fd >= 0) {
                g_shim_memfds[i] = received_fd;
            }
        }

        if (ipc_req.count > 0) {
            char ack = 'R';
            safe_write(g_broker_fd, &ack, 1);
            int ev_fd = recv_single_fd(g_broker_fd);
            if (ev_fd >= 0) {
                int old_flags = fcntl(fd, F_GETFL);
                dup2(ev_fd, fd);

                // Enforce the descriptor's true open flags on the adopted eventfd
                int cur_flags = fcntl(fd, F_GETFL);
                if (cur_flags != -1) {
                    if (g_vsouchi_fd_flags[fd] & O_NONBLOCK) {
                        fcntl(fd, F_SETFL, cur_flags | O_NONBLOCK);
                    } else {
                        fcntl(fd, F_SETFL, cur_flags & ~O_NONBLOCK);
                    }
                }

                if (real_close) real_close(ev_fd);
                else close(ev_fd);
            }
            g_nodes[minor].buffer_owner_fd = fd;
        } else {
            g_nodes[minor].buffer_owner_fd = -1;
            g_nodes[minor].is_streaming = 0;
        }
        pthread_mutex_unlock(&g_ipc_mutex);
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_QUERYBUF) {
        CHECK_ARG();
        struct v4l2_buffer *buf = (struct v4l2_buffer *)arg;
        if (buf->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
            errno = EINVAL;
            return -1;
        }

        IpcV4l2Buffer ipc_req;
        memset(&ipc_req, 0, sizeof(IpcV4l2Buffer));
        ipc_req.index = buf->index;
        ipc_req.type = buf->type;
        ipc_req.memory = V4L2_MEMORY_MMAP;

        if (shim_ipc_transaction(CMD_QUERYBUF, &ipc_req, sizeof(IpcV4l2Buffer), &ipc_req, sizeof(IpcV4l2Buffer)) != 0) return -1;
        if (ipc_req.length == 0) { errno = EINVAL; return -1; }

        buf->length = ipc_req.length;
        buf->m.offset = ipc_req.offset;
        buf->flags = ipc_req.flags;
        buf->memory = V4L2_MEMORY_MMAP;
        buf->field = 1; // V4L2_FIELD_NONE
        buf->bytesused = 0;
        buf->sequence = 0;
        buf->timestamp.tv_sec = 0;
        buf->timestamp.tv_usec = 0;
        buf->reserved = 0;
        buf->reserved2 = 0;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_QBUF) {
        CHECK_ARG();
        struct v4l2_buffer *buf = (struct v4l2_buffer *)arg;
        if (buf->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || buf->memory != V4L2_MEMORY_MMAP) {
            errno = EINVAL;
            return -1;
        }

        if (buf->flags & 0x00080000 /* V4L2_BUF_FLAG_REQUEST_FD */) {
            errno = EINVAL;
            return -1;
        }

        IpcV4l2Buffer ipc_req;
        memset(&ipc_req, 0, sizeof(IpcV4l2Buffer));
        ipc_req.index = buf->index;
        ipc_req.type = buf->type;
        ipc_req.memory = buf->memory;

        if (shim_ipc_transaction(CMD_QBUF, &ipc_req, sizeof(IpcV4l2Buffer), &ipc_req, sizeof(IpcV4l2Buffer)) != 0) return -1;

        buf->flags = ipc_req.flags;
        buf->bytesused = 0;
        buf->sequence = 0;
        buf->timestamp.tv_sec = 0;
        buf->timestamp.tv_usec = 0;
        buf->reserved = 0;
        buf->reserved2 = 0;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_QBUF) {
        CHECK_ARG();
        struct v4l2_buffer *buf = (struct v4l2_buffer *)arg;
        if (buf->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || buf->memory != V4L2_MEMORY_MMAP) {
            errno = EINVAL;
            return -1;
        }

        if (buf->flags & 0x00080000 /* V4L2_BUF_FLAG_REQUEST_FD */) {
            errno = EINVAL;
            return -1;
        }

        IpcV4l2Buffer ipc_req;
        memset(&ipc_req, 0, sizeof(IpcV4l2Buffer));
        ipc_req.index = buf->index;
        ipc_req.type = buf->type;
        ipc_req.memory = buf->memory;

        if (shim_ipc_transaction(CMD_QBUF, &ipc_req, sizeof(IpcV4l2Buffer), &ipc_req, sizeof(IpcV4l2Buffer)) != 0) return -1;

        buf->flags = ipc_req.flags;
        buf->reserved = 0;
        buf->reserved2 = 0;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_DQBUF) {
        CHECK_ARG();
        struct v4l2_buffer *buf = (struct v4l2_buffer *)arg;
        if (buf->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || buf->memory != V4L2_MEMORY_MMAP) {
            errno = EINVAL;
            return -1;
        }

        int minor = get_minor(fd);

        if (!g_nodes[minor].is_streaming) {
            errno = EINVAL;
            return -1;
        }

        int is_nonblock = (g_vsouchi_fd_flags[fd] & O_NONBLOCK);
        int fl = fcntl(fd, F_GETFL);
        if (fl != -1 && (fl & O_NONBLOCK)) {
            is_nonblock = 1;
        }

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int timeout_ms = is_nonblock ? 0 : -1;
        
        int p_res = poll(&pfd, 1, timeout_ms);
        if (p_res == 0) {
            errno = EAGAIN;
            return -1;
        } else if (p_res < 0) {
            return -1;
        }

        uint64_t ev_drain = 0;
        read(fd, &ev_drain, sizeof(ev_drain));

        if (!g_nodes[minor].is_streaming) {
            errno = EINVAL;
            return -1;
        }

        IpcV4l2Buffer ipc_req;
        memset(&ipc_req, 0, sizeof(IpcV4l2Buffer));
        ipc_req.type = buf->type;
        ipc_req.memory = buf->memory;

        if (shim_ipc_transaction(CMD_DQBUF, &ipc_req, sizeof(IpcV4l2Buffer), &ipc_req, sizeof(IpcV4l2Buffer)) != 0) return -1;
        if (ipc_req.length == 0xFFFFFFFF) { errno = EINVAL; return -1; }

        buf->index = ipc_req.index;
        buf->bytesused = ipc_req.bytesused;
        buf->length = ipc_req.length; 
        buf->flags = ipc_req.flags;
        buf->field = 1; // V4L2_FIELD_NONE
        buf->sequence = ipc_req.sequence;
        buf->reserved = 0;
        buf->reserved2 = 0;

        /* Convert hardware monotonic nanoseconds into timeval */
        if (ipc_req.timestamp_ns > 0) {
            buf->timestamp.tv_sec = (time_t)(ipc_req.timestamp_ns / 1000000000ULL);
            buf->timestamp.tv_usec = (suseconds_t)((ipc_req.timestamp_ns % 1000000000ULL) / 1000ULL);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            buf->timestamp.tv_sec = ts.tv_sec;
            buf->timestamp.tv_usec = ts.tv_nsec / 1000;
        }
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_STREAMON || req_clean == (uint32_t)VIDIOC_STREAMOFF) {
        CHECK_ARG();
        int *type = (int *)arg;
        if (*type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
            errno = EINVAL;
            return -1;
        }

        int minor = get_minor(fd);
        uint32_t cmd = (req_clean == (uint32_t)VIDIOC_STREAMON) ? CMD_STREAMON : CMD_STREAMOFF;

        // If turning stream off, mark state immediately so any concurrently waking DQBUF aborts
        if (cmd == CMD_STREAMOFF) {
            g_nodes[minor].is_streaming = 0;
        }

        char ack;
        if (shim_ipc_transaction(cmd, type, sizeof(int), &ack, 1) != 0) return -1;

        if (cmd == CMD_STREAMON) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                uint64_t ev_drain = 0;
                read(fd, &ev_drain, sizeof(ev_drain));
            }
            g_nodes[minor].is_streaming = 1;
        }
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_QUERYCTRL) {
        CHECK_ARG();
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
        CHECK_ARG();
        struct v4l2_control *ctrl = (struct v4l2_control *)arg;
        if (ctrl->id == 0x00980001 || ctrl->id == 0x009a0001) { errno = EINVAL; return -1; }
        
        uint32_t req_id = ctrl->id;
        IpcControlPayload response;
        memset(&response, 0, sizeof(response));

        if (shim_ipc_transaction(CMD_G_CTRL, &req_id, sizeof(uint32_t), &response, sizeof(IpcControlPayload)) != 0) return -1;
        if (response.id == 0) { errno = EINVAL; return -1; }
        
        ctrl->value = response.value;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_S_CTRL) {
        CHECK_ARG();
        struct v4l2_control *ctrl = (struct v4l2_control *)arg;
        if (ctrl->id == 0x00980001 || ctrl->id == 0x009a0001) { errno = EINVAL; return -1; }
        
        IpcControlPayload payload = { .id = ctrl->id, .value = ctrl->value };
        int32_t ack = 0;
        if (shim_ipc_transaction(CMD_S_CTRL, &payload, sizeof(IpcControlPayload), &ack, sizeof(int32_t)) != 0) return -1;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_G_EXT_CTRLS || req_clean == (uint32_t)VIDIOC_S_EXT_CTRLS || req_clean == (uint32_t)VIDIOC_TRY_EXT_CTRLS) {
        CHECK_ARG();
        struct v4l2_ext_controls *ext = (struct v4l2_ext_controls *)arg;

        // 1. MIN_VAL, MAX_VAL, and REQUEST_VAL are unsupported by our controls -> return EINVAL per line 980
        if (ext->which == V4L2_CTRL_WHICH_MIN_VAL || ext->which == V4L2_CTRL_WHICH_MAX_VAL || ext->which == V4L2_CTRL_WHICH_REQUEST_VAL) {
            ext->error_idx = (req_clean == (uint32_t)VIDIOC_TRY_EXT_CTRLS) ? 0 : ext->count;
            errno = EINVAL;
            return -1;
        }

        // 2. DEF_VAL cannot be set or tried -> return EINVAL per line 991
        if ((req_clean == (uint32_t)VIDIOC_S_EXT_CTRLS || req_clean == (uint32_t)VIDIOC_TRY_EXT_CTRLS) && ext->which == V4L2_CTRL_WHICH_DEF_VAL) {
            ext->error_idx = (req_clean == (uint32_t)VIDIOC_TRY_EXT_CTRLS) ? 0 : ext->count;
            errno = EINVAL;
            return -1;
        }

        if (ext->count == 0) {
            if (ext->which != 0 && ext->which != V4L2_CTRL_WHICH_DEF_VAL && 
                ext->which != 0x00980000 && ext->which != 0x009a0000) {
                errno = EINVAL;
                return -1;
            }
            ext->error_idx = 0;
            return 0;
        }

        for (uint32_t i = 0; i < ext->count; i++) {
            struct v4l2_ext_control *ctrl = &ext->controls[i];
            uint32_t req_id = ctrl->id;

            // 3. Class validation: if ext->which specifies a class, all controls must belong to it
            if (ext->which != 0 && ext->which != V4L2_CTRL_WHICH_DEF_VAL) {
                if ((req_id & 0x0fff0000) != ext->which) {
                    ext->error_idx = (req_clean == (uint32_t)VIDIOC_TRY_EXT_CTRLS) ? i : ext->count;
                    errno = EINVAL;
                    return -1;
                }
            }

            // G_EXT_CTRLS on write-only classes
            if (req_clean == (uint32_t)VIDIOC_G_EXT_CTRLS && (req_id == 0x00980001 || req_id == 0x009a0001)) {
                ext->error_idx = ext->count; // Expected by line 852
                errno = EACCES;
                return -1;
            }

            // S_EXT_CTRLS or TRY_EXT_CTRLS on read-only controls
            if (req_id == 0x00980914 || req_id == 0x00980915 || req_id == 0x00980922 ||
                req_id == 0x00980001 || req_id == 0x009a0001) {
                if (req_clean == (uint32_t)VIDIOC_S_EXT_CTRLS) {
                    ext->error_idx = ext->count; // Expected by line 900 & line 1016
                    errno = EACCES;
                    return -1;
                }
                if (req_clean == (uint32_t)VIDIOC_TRY_EXT_CTRLS) {
                    ext->error_idx = i; // Expected by line 889 & line 1010
                    errno = EACCES;
                    return -1;
                }
            }

            if (req_clean == (uint32_t)VIDIOC_G_EXT_CTRLS || req_clean == (uint32_t)VIDIOC_TRY_EXT_CTRLS) {
                IpcControlPayload response;
                memset(&response, 0, sizeof(response));

                if (shim_ipc_transaction(CMD_G_CTRL, &req_id, sizeof(uint32_t), &response, sizeof(IpcControlPayload)) != 0) {
                    ext->error_idx = (req_clean == (uint32_t)VIDIOC_TRY_EXT_CTRLS) ? i : ext->count;
                    return -1; 
                }
                if (req_clean == (uint32_t)VIDIOC_G_EXT_CTRLS) ctrl->value = response.value;
            } 
            else if (req_clean == (uint32_t)VIDIOC_S_EXT_CTRLS) {
                IpcControlPayload payload = { .id = ctrl->id, .value = ctrl->value };
                int32_t ack = 0;
                if (shim_ipc_transaction(CMD_S_CTRL, &payload, sizeof(IpcControlPayload), &ack, sizeof(int32_t)) != 0) {
                    ext->error_idx = ext->count;
                    return -1;
                }
            }
        }
        
        ext->error_idx = 0;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_G_PRIORITY) {
        CHECK_ARG();
        enum v4l2_priority *prio = (enum v4l2_priority *)arg;
        *prio = g_current_prio;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_S_PRIORITY) {
        CHECK_ARG();
        enum v4l2_priority *prio = (enum v4l2_priority *)arg;
        if (*prio != V4L2_PRIORITY_INTERACTIVE && *prio != V4L2_PRIORITY_DEFAULT &&
            *prio != V4L2_PRIORITY_BACKGROUND && *prio != V4L2_PRIORITY_RECORD) {
            errno = EINVAL;
            return -1;
        }

        if (g_current_prio == V4L2_PRIORITY_RECORD && g_prio_owner_fd != fd && *prio != V4L2_PRIORITY_RECORD) {
            errno = EBUSY;
            return -1;
        }

        g_current_prio = *prio;
        g_prio_owner_fd = fd;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_QUERY_EXT_CTRL) {
        CHECK_ARG();
        struct v4l2_query_ext_ctrl *qec = (struct v4l2_query_ext_ctrl *)arg;
        uint32_t req_id = qec->id;

        struct v4l2_queryctrl qc;
        memset(&qc, 0, sizeof(qc));

        if (shim_ipc_transaction(CMD_QUERYCTRL, &req_id, sizeof(uint32_t), &qc, sizeof(struct v4l2_queryctrl)) != 0) return -1;
        if (qc.id == 0) { errno = EINVAL; return -1; }

        memset(qec, 0, sizeof(*qec));
        qec->id = qc.id;
        qec->type = qc.type;
        strncpy(qec->name, (const char *)qc.name, sizeof(qec->name) - 1);
        qec->minimum = qc.minimum;
        qec->maximum = qc.maximum;
        qec->step = qc.step;
        qec->default_value = qc.default_value;
        qec->flags = qc.flags;
        qec->elems = 1;
        qec->elem_size = 4;
        qec->nr_of_dims = 0;
        return 0;
    }

    if (req_clean == (uint32_t)VIDIOC_QUERYMENU) {
        CHECK_ARG();
        struct v4l2_querymenu *menu = (struct v4l2_querymenu *)arg;
        memset(menu->name, 0, sizeof(menu->name));
        menu->reserved = 0;

        if (menu->id == V4L2_CID_EXPOSURE_AUTO) {
            if (menu->index == 0) { 
                strcpy((char*)menu->name, "Auto Mode"); 
                return 0; 
            }
            if (menu->index == 1) { 
                strcpy((char*)menu->name, "Manual Mode"); 
                return 0; 
            }
        }
        errno = EINVAL;
        return -1;
    }
    
    if (req_clean == (uint32_t)VIDIOC_SUBSCRIBE_EVENT) {
        CHECK_ARG();
        return shim_v4l2_subscribe_event(fd, (struct v4l2_event_subscription *)arg);
    }

    if (req_clean == (uint32_t)VIDIOC_UNSUBSCRIBE_EVENT) {
        CHECK_ARG();
        return shim_v4l2_unsubscribe_event(fd, (struct v4l2_event_subscription *)arg);
    }

    if (req_clean == (uint32_t)VIDIOC_DQEVENT) {
        CHECK_ARG();
        return shim_v4l2_dqevent(fd, (struct v4l2_event *)arg);
    }

    if (req_clean == (uint32_t)VIDIOC_G_STD || req_clean == (uint32_t)VIDIOC_S_STD || req_clean == (uint32_t)VIDIOC_ENUMSTD) {
        errno = EINVAL; 
        return -1;
    }

    if (req_clean == (uint32_t)VIDIOC_LOG_STATUS) {
        return 0;
    }

    errno = ENOTTY; 
    return -1;
}
