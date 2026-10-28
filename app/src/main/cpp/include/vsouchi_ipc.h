/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Shared IPC structures and V4L2 command constants for the vSouchi routing pipeline.
#ifndef VSOUCHI_IPC_H
#define VSOUCHI_IPC_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define VSOUCHI_IPC_MAGIC    0x76654476 // "vDev"
#define VSOUCHI_PROTOCOL_VER 1
#define CMD_GET_CAPS   0x10
#define CMD_ENUM_FMT   0x11
#define CMD_G_FMT      0x12
#define CMD_S_FMT      0x13
#define CMD_TRY_FMT    0x14
#define CMD_ENUM_FRAMESIZES     0x15
#define CMD_ENUM_FRAMEINTERVALS 0x16
#define CMD_REQBUFS    0x17
#define CMD_QUERYBUF   0x18
#define CMD_QBUF       0x19
#define CMD_DQBUF      0x1A
#define CMD_STREAMON   0x1B
#define CMD_STREAMOFF  0x1C
#define CMD_DISCONNECT 0x1D
#define CMD_CROPCAP    0x1E
#define CMD_G_PARM     0x1F
#define CMD_S_PARM     0x20
#define CMD_POLL       0x21

#define CMD_QUERYCTRL  0x30
#define CMD_G_CTRL     0x31
#define CMD_S_CTRL     0x32

#define BROKER_NACK_EBUSY 0xEE 

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t reserved;
} IpcHandshakePacket;

typedef struct {
    char driver[16];
    char card[32];
    char bus_info[32];
    uint32_t version;
    uint32_t capabilities;
} IpcV4l2Caps;

typedef struct {
    uint32_t index;
    uint32_t type;
    uint32_t flags;
    char description[32];
    uint32_t pixelformat;
} IpcV4l2FmtDesc;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixelformat;
    uint32_t field;       
    uint32_t bytesperline;
    uint32_t sizeimage;
    uint32_t colorspace;  
} IpcV4l2Format;

typedef struct {
    uint32_t index;
    uint32_t pixel_format;
    uint32_t type;   
    uint32_t width;
    uint32_t height;
    uint32_t reserved[4];
} IpcV4l2FrmSizeEnum;

typedef struct {
    uint32_t index;
    uint32_t pixel_format;
    uint32_t width;
    uint32_t height;
    uint32_t type;   
    uint32_t fps_numerator;
    uint32_t fps_denominator;
    uint32_t reserved[4];
} IpcV4l2FrmIvalEnum;

typedef struct {
    uint32_t id;
    int32_t value;
} IpcControlPayload;

typedef struct {
    uint32_t client_fd;
    uint32_t cmd;
} IpcOutboundHeader;

typedef struct {
    uint32_t client_fd;
    uint32_t payload_size;
    int32_t ipc_status;
} IpcInboundHeader;

typedef struct {
    uint32_t fps_numerator;
    uint32_t fps_denominator;
} IpcV4l2StreamParm;

typedef struct {
    uint32_t type;
    int32_t bounds_left, bounds_top, bounds_width, bounds_height;
    int32_t defrect_left, defrect_top, defrect_width, defrect_height;
    uint32_t pixelaspect_num, pixelaspect_denom;
} IpcV4l2CropCap;

typedef struct {
    uint32_t count;
    uint32_t type;
    uint32_t memory;
} IpcV4l2ReqBufs;

typedef struct {
    uint32_t index;
    uint32_t type;
    uint32_t bytesused;
    uint32_t flags;
    uint32_t length;
    uint32_t offset;
    uint32_t memory;
} IpcV4l2Buffer;

static inline const char* vsouchi_get_vdev_path(void) {
    static _Thread_local char path[512];

    const char* env = getenv("VDEV_PATH");
    if (env && env[0] != '\0') {
        snprintf(path, sizeof(path), "%s", env);
        return path;
    }

    const char* tmp = getenv("TMPDIR");
    if (tmp && tmp[0] != '\0') {
        snprintf(path, sizeof(path), "%s/vdev", tmp);
        return path;
    }

    const char* prefix = getenv("PREFIX");
    if (prefix && prefix[0] != '\0') {
        snprintf(path, sizeof(path), "%s/tmp/vdev", prefix);
        return path;
    }
    return "/tmp/vdev";
}
#endif // VSOUCHI_IPC_H
