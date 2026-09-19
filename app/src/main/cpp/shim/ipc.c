/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <string.h>
#include <android/log.h>
#include "shim/internal.h"
#include "vsouchi_ipc.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "vSouchi:ShimIPC", __VA_ARGS__)

pthread_mutex_t g_ipc_mutex = PTHREAD_MUTEX_INITIALIZER;

int safe_write(int fd, const void *buf, size_t count) {
    if (fd < 0) return -1;
    int res = send(fd, buf, count, MSG_NOSIGNAL);
    if (res <= 0) {
        if (real_close) real_close(fd);
        if (fd == g_broker_fd) g_broker_fd = -1;
        shim_clear_memfds();
    }
    return res;
}

int safe_read(int fd, void *buf, size_t count) {
    if (fd < 0) return -1;
    int res = read(fd, buf, count);
    if (res <= 0) {
        if (real_close) real_close(fd);
        if (fd == g_broker_fd) g_broker_fd = -1;
        shim_clear_memfds();
    }
    return res;
}

int shim_ipc_transaction(uint32_t cmd, const void* req_payload, size_t req_sz, void* resp_payload, size_t resp_sz) {
    pthread_mutex_lock(&g_ipc_mutex);

    struct iovec iov[2];
    iov[0].iov_base = &cmd;
    iov[0].iov_len = sizeof(uint32_t);
    int count = 1;
    
    if (req_payload && req_sz > 0) {
        iov[1].iov_base = (void*)req_payload;
        iov[1].iov_len = req_sz;
        count = 2;
    }

    if (writev(g_broker_fd, iov, count) <= 0) goto error;

    int32_t ipc_status = 0;
    if (safe_read(g_broker_fd, &ipc_status, sizeof(int32_t)) <= 0) goto error;

    if (ipc_status < 0) {
        pthread_mutex_unlock(&g_ipc_mutex);
        errno = -ipc_status;
        return -1;
    }

    if (resp_payload && resp_sz > 0) {
        if (safe_read(g_broker_fd, resp_payload, resp_sz) <= 0) goto error;
    }

    pthread_mutex_unlock(&g_ipc_mutex);
    return 0;

error:
    pthread_mutex_unlock(&g_ipc_mutex);
    errno = EIO;
    return -1;
}

int recv_single_fd(int socket) {
    struct msghdr msg = {0};
    char m_buffer[1];
    struct iovec io = { .iov_base = m_buffer, .iov_len = 1 };
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    
    char c_buffer[CMSG_SPACE(sizeof(int))];
    memset(c_buffer, 0, sizeof(c_buffer));
    msg.msg_control = c_buffer;
    msg.msg_controllen = sizeof(c_buffer);
    
    int res = recvmsg(socket, &msg, 0);
    if (res < 0) {
        LOGI("recvmsg failed! errno: %d", errno);
        return -1;
    }
    
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
        return *((int *)CMSG_DATA(cmsg));
    }
    
    LOGI("recvmsg succeeded but cmsg was missing or invalid!");
    return -1;
}
