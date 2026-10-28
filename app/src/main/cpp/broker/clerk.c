/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Generic routing loop for shim clients. Forwards commands to Bridge and returns replies.
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include "broker/internal.h"

void* clerk_thread_func(void* arg) {
    ClientContext *ctx = (ClientContext*)arg;
    int client_fd = ctx->client_fd;

    IpcHandshakePacket hs;
    size_t total_hs = 0;
    char *hs_buf = (char*)&hs;
    while (total_hs < sizeof(IpcHandshakePacket)) {
        int r = read(client_fd, hs_buf + total_hs, sizeof(IpcHandshakePacket) - total_hs);
        if (r <= 0) {
            printf("[Broker] Handshake read failed from client FD %d.\n", client_fd);
            goto cleanup;
        }
        total_hs += (size_t)r;
    }

    if (hs.magic != VSOUCHI_IPC_MAGIC) {
        printf("[Broker] Invalid magic 0x%08X from client FD %d (expected 0x%08X).\n",
               hs.magic, client_fd, VSOUCHI_IPC_MAGIC);
        goto cleanup;
    }

    if (hs.version != VSOUCHI_PROTOCOL_VER) {
        printf("[Broker] Protocol version mismatch on FD %d (client v%u != broker v%u).\n",
               client_fd, hs.version, (uint32_t)VSOUCHI_PROTOCOL_VER);
        int32_t proto_err = -EPROTO;
        write(client_fd, &proto_err, sizeof(int32_t));
        goto cleanup;
    }

    int32_t ack = 0;
    if (write(client_fd, &ack, sizeof(int32_t)) <= 0) {
        goto cleanup;
    }

    while (1) {
        char buf[MAX_PAYLOAD];
        int n = read(client_fd, buf, sizeof(buf));
        
        if (n <= 0) {
            goto cleanup;
        }

        // Catch Shim's raw FD ACK 'R' to securely trigger kernel-level FD transfer
        if (n == 1 && buf[0] == 'R') {
            int fd_to_send = -1;
            pthread_mutex_lock(&ctx->lock);
            if (ctx->fd_send_index < ctx->fd_count) {
                fd_to_send = ctx->received_fds[ctx->fd_send_index++];
            }
            pthread_mutex_unlock(&ctx->lock);

            if (fd_to_send >= 0) {
                struct msghdr out_msg = {0};
                char dummy = 'R'; 
                struct iovec out_iov[1] = {{ &dummy, 1 }};
                char out_cmsg_buf[CMSG_SPACE(sizeof(int))] = {0};
                out_msg.msg_iov = out_iov; out_msg.msg_iovlen = 1;
                out_msg.msg_control = out_cmsg_buf; out_msg.msg_controllen = sizeof(out_cmsg_buf);
                
                struct cmsghdr *out_cmsg = CMSG_FIRSTHDR(&out_msg);
                out_cmsg->cmsg_level = SOL_SOCKET; out_cmsg->cmsg_type = SCM_RIGHTS;
                out_cmsg->cmsg_len = CMSG_LEN(sizeof(int));
                *((int *)CMSG_DATA(out_cmsg)) = fd_to_send;
                
                sendmsg(client_fd, &out_msg, 0);
                close(fd_to_send); // Close broker's kernel-duplicated copy
            }
            continue; 
        }

        if (n >= sizeof(uint32_t)) {
            uint32_t cmd = *(uint32_t*)buf;

            // 1. Subsystem Monitor Hook (Validates hardware/stream locks)
            if (subsystem_validate_command(client_fd, cmd) < 0) {
                int32_t ebusy = -16;
                write(client_fd, &ebusy, sizeof(int32_t));
                continue; 
            }

            // Ensure Android APK bridge is alive (triggers on-demand wake if dead)
            if (broker_ensure_bridge() < 0) {
                int32_t enodev = -19;
                write(client_fd, &enodev, sizeof(int32_t));
                continue;
            }

            // 2. Forward securely to Bridge
            IpcOutboundHeader hdr = { .client_fd = client_fd, .cmd = cmd };
            pthread_mutex_lock(&g_bridge_tx_mutex);
            if (g_bridge_fd < 0) {
                pthread_mutex_unlock(&g_bridge_tx_mutex);
                int32_t enodev = -19;
                write(client_fd, &enodev, sizeof(int32_t));
                continue;
            }
            struct iovec iov[2];
            iov[0].iov_base = &hdr; iov[0].iov_len = sizeof(hdr);
            iov[1].iov_base = buf + sizeof(uint32_t); iov[1].iov_len = n - sizeof(uint32_t);
            writev(g_bridge_fd, iov, (iov[1].iov_len > 0) ? 2 : 1);
            pthread_mutex_unlock(&g_bridge_tx_mutex);

            // 3. Sleep until Router wakes us
            pthread_mutex_lock(&ctx->lock);
            while (!ctx->response_ready && !ctx->fatal_error) {
                pthread_cond_wait(&ctx->cond, &ctx->lock);
            }
            
            if (ctx->fatal_error) {
                pthread_mutex_unlock(&ctx->lock);
                int32_t enodev = -19;
                write(client_fd, &enodev, sizeof(int32_t));
                break;
            }

            // 4. Reply to Shim
            struct iovec riov[2];
            riov[0].iov_base = &ctx->resp_status; riov[0].iov_len = sizeof(int32_t);
            riov[1].iov_base = ctx->resp_payload; riov[1].iov_len = ctx->resp_size;
            writev(client_fd, riov, (ctx->resp_size > 0) ? 2 : 1);

            ctx->response_ready = 0;
            pthread_mutex_unlock(&ctx->lock);
        }

    }
cleanup:
     // Ensure socket cleanup runs on both client EOF and bridge fatal error
    printf("[Broker] Shim (FD %d) cleaned up.\n", client_fd);
    subsystem_handle_disconnect(client_fd);

    pthread_mutex_lock(&ctx->lock);
    for (int i = ctx->fd_send_index; i < ctx->fd_count; i++) {
        if (ctx->received_fds[i] >= 0) {
            close(ctx->received_fds[i]);
            ctx->received_fds[i] = -1;
        }
    }
    ctx->fd_count = 0;
    ctx->fd_send_index = 0;
    pthread_mutex_unlock(&ctx->lock);

    pthread_mutex_lock(&g_clients_mutex);
    ctx->active = 0;
    close(client_fd);
    pthread_mutex_unlock(&g_clients_mutex);
    return NULL;
}
