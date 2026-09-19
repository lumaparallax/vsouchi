/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Manages V4L2 stream locks and the hardware debounce watchdog for the Broker.
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include "broker/internal.h"
#include "vsouchi_ipc.h"

static int g_stream_owner_fd = -1;
static pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t g_watchdog_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_watchdog_cond = PTHREAD_COND_INITIALIZER;
static int g_watchdog_pending = 0;

static void* disconnect_watchdog_func(void* arg) {
    pthread_mutex_lock(&g_watchdog_mutex);
    while (1) {
        while (g_watchdog_pending == 0) {
            pthread_cond_wait(&g_watchdog_cond, &g_watchdog_mutex);
        }

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 500000000; // Add 500ms
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000;
        }

        int rc = pthread_cond_timedwait(&g_watchdog_cond, &g_watchdog_mutex, &ts);

        // If timed out AND teardown wasn't aborted by a reconnect, kill the hardware
        if (rc == ETIMEDOUT && g_watchdog_pending == 1) {
            printf("[Broker/V4L2] 500ms elapsed. Issuing hardware teardown.\n");
            g_watchdog_pending = 0;

            IpcOutboundHeader hdr = { .client_fd = 0, .cmd = CMD_DISCONNECT };
            pthread_mutex_lock(&g_bridge_tx_mutex);
            if (g_bridge_fd >= 0) {
                write(g_bridge_fd, &hdr, sizeof(hdr));
            }
            pthread_mutex_unlock(&g_bridge_tx_mutex);
        }
    }
    pthread_mutex_unlock(&g_watchdog_mutex);
    return NULL;
}

void subsystem_init_watchdog() {
    pthread_t watchdog_tid;
    pthread_create(&watchdog_tid, NULL, disconnect_watchdog_func, NULL);
    pthread_detach(watchdog_tid);
}

int subsystem_validate_command(int client_fd, uint32_t cmd) {
    int is_exclusive = (cmd == CMD_S_FMT || cmd == CMD_REQBUFS || cmd == CMD_QBUF ||
                        cmd == CMD_DQBUF || cmd == CMD_STREAMON || cmd == CMD_STREAMOFF ||
                        cmd == CMD_S_PARM);
    int is_acquiring = (cmd == CMD_S_FMT || cmd == CMD_REQBUFS || cmd == CMD_STREAMON);

    pthread_mutex_lock(&g_state_mutex);
    if (is_exclusive) {
        if (g_stream_owner_fd == -1 && is_acquiring) {
            g_stream_owner_fd = client_fd;
            printf("[Broker/V4L2] Shim %d acquired hardware stream lock.\n", client_fd);

            // Abort any pending hardware teardowns
            pthread_mutex_lock(&g_watchdog_mutex);
            g_watchdog_pending = 0;
            pthread_cond_signal(&g_watchdog_cond);
            pthread_mutex_unlock(&g_watchdog_mutex);

        } else if (g_stream_owner_fd != -1 && g_stream_owner_fd != client_fd) {
            pthread_mutex_unlock(&g_state_mutex);
            return -1; // Reject (EBUSY)
        }
    }
    pthread_mutex_unlock(&g_state_mutex);
    return 0;
}

void subsystem_handle_disconnect(int client_fd) {
    pthread_mutex_lock(&g_state_mutex);
    if (g_stream_owner_fd == client_fd) {
        printf("[Broker/V4L2] Stream owner exited. Starting 500ms teardown watchdog.\n");
        g_stream_owner_fd = -1;

        pthread_mutex_lock(&g_watchdog_mutex);
        g_watchdog_pending = 1;
        pthread_cond_signal(&g_watchdog_cond);
        pthread_mutex_unlock(&g_watchdog_mutex);
    }
    pthread_mutex_unlock(&g_state_mutex);
}
