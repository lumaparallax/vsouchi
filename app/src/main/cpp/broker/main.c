/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Handles node creation, socket binding, and routing Bridge IPC replies to the correct Clerk.
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include "broker/internal.h"

ClientContext g_clients[MAX_CLIENTS];
pthread_mutex_t g_clients_mutex = PTHREAD_MUTEX_INITIALIZER;

int g_bridge_fd = -1;
pthread_mutex_t g_bridge_tx_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_bridge_cond = PTHREAD_COND_INITIALIZER;

static JavaVM *g_jvm = NULL;
static jclass g_daemon_entry_class = NULL;
static jmethodID g_request_wake_mid = NULL;

void broker_trigger_wake(void) {
    if (!g_jvm || !g_daemon_entry_class || !g_request_wake_mid) return;
    JNIEnv *env = NULL;
    int attached = 0;
    jint res = (*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) == 0) {
            attached = 1;
        }
    }
    if (env) {
        (*env)->CallStaticVoidMethod(env, g_daemon_entry_class, g_request_wake_mid);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
    }
    if (attached) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
}

int broker_ensure_bridge(void) {
    pthread_mutex_lock(&g_bridge_tx_mutex);
    if (g_bridge_fd >= 0) {
        pthread_mutex_unlock(&g_bridge_tx_mutex);
        return 0;
    }

    printf("[Broker] Bridge connection offline. Requesting on-demand APK wake...\n");
    broker_trigger_wake();

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 4; // 4-second timeout for Android to start service

    while (g_bridge_fd == -1) {
        int rc = pthread_cond_timedwait(&g_bridge_cond, &g_bridge_tx_mutex, &ts);
        if (rc != 0 && g_bridge_fd == -1) {
            printf("[Broker] APK wake timeout after 4s.\n");
            pthread_mutex_unlock(&g_bridge_tx_mutex);
            return -1;
        }
    }

    pthread_mutex_unlock(&g_bridge_tx_mutex);
    return 0;
}

void* router_thread_func(void* arg) {
    while (1) {
        IpcInboundHeader hdr;
        struct msghdr msg = {0};
        struct iovec iov[1] = {{ &hdr, sizeof(IpcInboundHeader) }};
        msg.msg_iov = iov; msg.msg_iovlen = 1;
        
        char cmsg_buf[CMSG_SPACE(32 * sizeof(int))] = {0};
        msg.msg_control = cmsg_buf; msg.msg_controllen = sizeof(cmsg_buf);

        if (recvmsg(g_bridge_fd, &msg, MSG_WAITALL) <= 0) goto bridge_died;

        int fds[32]; int fd_count = 0;
        struct cmsghdr *cmsg;
        for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                fd_count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                memcpy(fds, CMSG_DATA(cmsg), fd_count * sizeof(int));
                break;
            }
        }

        ClientContext *ctx = NULL;
        pthread_mutex_lock(&g_clients_mutex);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].active && g_clients[i].client_fd == hdr.client_fd) {
                ctx = &g_clients[i]; break;
            }
        }
        pthread_mutex_unlock(&g_clients_mutex);

        char temp_payload[MAX_PAYLOAD];
        if (hdr.payload_size > 0) {
            int remaining = hdr.payload_size;
            int offset = 0;
            while (remaining > 0) {
                int r = read(g_bridge_fd, temp_payload + offset, remaining);
                if (r <= 0) goto bridge_died;
                remaining -= r; offset += r;
            }
        }

        if (ctx) {
            pthread_mutex_lock(&ctx->lock);
            ctx->resp_status = hdr.ipc_status;
            ctx->resp_size = hdr.payload_size;
            if (hdr.payload_size > 0) memcpy(ctx->resp_payload, temp_payload, hdr.payload_size);
            if (fd_count > 0) {
                memcpy(ctx->received_fds, fds, fd_count * sizeof(int));
                ctx->fd_count = fd_count;
                ctx->fd_send_index = 0;
            }
            ctx->response_ready = 1;
            pthread_cond_signal(&ctx->cond);
            pthread_mutex_unlock(&ctx->lock);
        } else {
            for(int i=0; i<fd_count; i++) close(fds[i]);
        }
    }

bridge_died:
    printf("[Broker] Bridge connection lost!\n");
    pthread_mutex_lock(&g_bridge_tx_mutex);
    if (g_bridge_fd >= 0) {
        close(g_bridge_fd);
        g_bridge_fd = -1;
    }
    pthread_mutex_unlock(&g_bridge_tx_mutex);

    pthread_mutex_lock(&g_clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active) {
            pthread_mutex_lock(&g_clients[i].lock);
            g_clients[i].fatal_error = 1;
            g_clients[i].response_ready = 1;
            pthread_cond_signal(&g_clients[i].cond);
            pthread_mutex_unlock(&g_clients[i].lock);
        }
    }
    pthread_mutex_unlock(&g_clients_mutex);
    return NULL;
}

JNIEXPORT jint JNICALL Java_io_github_lumaparallax_vsouchi_DaemonEntry_nativeGetUds(JNIEnv *env, jclass clazz) {
    pthread_mutex_lock(&g_bridge_tx_mutex);
    if (g_bridge_fd >= 0) {
        close(g_bridge_fd);
        g_bridge_fd = -1;
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        pthread_mutex_unlock(&g_bridge_tx_mutex);
        return -1;
    }
    g_bridge_fd = sv[0];

    pthread_t router_tid;
    pthread_create(&router_tid, NULL, router_thread_func, NULL);
    pthread_detach(router_tid);

    pthread_cond_broadcast(&g_bridge_cond);
    pthread_mutex_unlock(&g_bridge_tx_mutex);

    printf("[Broker] Reconnected to vSouchi Android App (Bridge FD %d)!\n", g_bridge_fd);
    return sv[1]; 
}

JNIEXPORT void JNICALL Java_io_github_lumaparallax_vsouchi_DaemonEntry_start(JNIEnv *env, jclass clazz) {
    (*env)->GetJavaVM(env, &g_jvm);
    g_daemon_entry_class = (jclass)(*env)->NewGlobalRef(env, clazz);
    g_request_wake_mid = (*env)->GetStaticMethodID(env, clazz, "requestWakeApp", "()V");

    const char* base_dir = vsouchi_get_vdev_path();
    mkdir(base_dir, 0777);

    char path[256];
    snprintf(path, sizeof(path), "%s/sys", base_dir); mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/sys/class", base_dir); mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/sys/class/video4linux", base_dir); mkdir(path, 0777);

    // Create empty file so target apps don't skip the node
    snprintf(path, sizeof(path), "%s/sys/class/video4linux/video0", base_dir); 
    int dummy_fd = open(path, O_CREAT | O_RDWR, 0666);
    if (dummy_fd >= 0) close(dummy_fd);

    char sock_path[256];
    snprintf(sock_path, sizeof(sock_path), "%s/video0", base_dir);
    unlink(sock_path);

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    
    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 10);

    printf("[Broker] Published Virtual Node: %s\n", sock_path);

    subsystem_init_watchdog();

    while (1) {
        int new_fd = accept(server_fd, NULL, NULL);
        if (new_fd >= 0) {
            pthread_mutex_lock(&g_clients_mutex);
            ClientContext *ctx = NULL;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (!g_clients[i].active) {
                    ctx = &g_clients[i];
                    ctx->active = 1;
                    ctx->client_fd = new_fd;
                    ctx->response_ready = 0;
                    ctx->fatal_error = 0;
                    ctx->fd_count = 0;
                    ctx->fd_send_index = 0;
                    
                    pthread_mutex_init(&ctx->lock, NULL);
                    pthread_cond_init(&ctx->cond, NULL);
                    break;
                }
            }
            pthread_mutex_unlock(&g_clients_mutex);

            if (ctx) {
                printf("[Broker] Shim connected (FD %d).\n", new_fd);
                pthread_t clerk_tid;
                pthread_create(&clerk_tid, NULL, clerk_thread_func, ctx);
                pthread_detach(clerk_tid);
            } else {
                printf("[Broker] Max clients reached. Rejecting connection.\n");
                close(new_fd); 
            }
        }
    }

    close(server_fd);
    unlink(sock_path);
}
