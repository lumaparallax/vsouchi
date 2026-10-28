/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Internal state and subsystem-agnostic routing definitions for the Broker daemon.
#ifndef BROKER_INTERNAL_H
#define BROKER_INTERNAL_H

#include <pthread.h>
#include <stdint.h>
#include "vsouchi_ipc.h"

#define MAX_CLIENTS 32
#define MAX_PAYLOAD 8192

typedef struct {
    int active;
    int client_fd;
    pthread_mutex_t lock;
    pthread_cond_t cond;

    int32_t resp_status;
    uint32_t resp_size;
    char resp_payload[MAX_PAYLOAD];
    int response_ready;
    int fatal_error;

    int received_fds[32];
    int fd_count;
    int fd_send_index;
} ClientContext;

extern ClientContext g_clients[MAX_CLIENTS];
extern pthread_mutex_t g_clients_mutex;

extern int g_bridge_fd;
extern pthread_mutex_t g_bridge_tx_mutex;
extern pthread_cond_t g_bridge_cond;

int broker_ensure_bridge(void);
void* clerk_thread_func(void* arg);

// Generic Subsystem Hooks (Implemented by specific subsystem monitors, e.g., V4L2)
int subsystem_validate_command(int client_fd, uint32_t cmd);
void subsystem_handle_disconnect(int client_fd);
void subsystem_init_watchdog();

#endif // BROKER_INTERNAL_H
