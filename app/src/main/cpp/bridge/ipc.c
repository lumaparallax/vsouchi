/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Helpers for securely passing file descriptors across UNIX domain sockets via SCM_RIGHTS.
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/uio.h>
#include <string.h>
#include <errno.h>
#include <android/log.h>
#include "bridge/ipc.h"

#define TAG "vSouchi:BridgeIPC"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

int send_single_fd(int socket, int fd_to_send) {
    struct msghdr msg = {0};
    char buf[CMSG_SPACE(sizeof(int))];
    memset(buf, 0, sizeof(buf));

    char m_buf[1] = {'F'};
    struct iovec io = { .iov_base = m_buf, .iov_len = 1 };
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *((int *)CMSG_DATA(cmsg)) = fd_to_send;

    int res = sendmsg(socket, &msg, 0);
    if (res < 0) {
        LOGE("send_single_fd failed! errno: %d", errno);
    }
    return res;
}
