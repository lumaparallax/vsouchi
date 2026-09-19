/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Helpers for securely passing file descriptors across UNIX domain sockets via SCM_RIGHTS.
#ifndef BRIDGE_IPC_H
#define BRIDGE_IPC_H

int send_single_fd(int socket, int fd_to_send);

#endif // BRIDGE_IPC_H
