/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef SHIM_INTERNAL_H
#define SHIM_INTERNAL_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <poll.h>
#include <sys/select.h>
#include <dirent.h>
#include <pthread.h>

extern int g_vsouchi_fds[1024];
extern int g_vsouchi_fd_minor[1024];
extern int g_vsouchi_fd_flags[1024];
extern int g_broker_fd;
extern pthread_mutex_t g_ipc_mutex;

int shim_v4l2_check_and_clear_buffer_owner(int fd);
int shim_ipc_transaction(uint32_t cmd, const void* req_payload, size_t req_sz, void* resp_payload, size_t resp_sz);
int recv_single_fd(int socket);
int safe_write(int fd, const void *buf, size_t count);
int safe_read(int fd, void *buf, size_t count);
void shim_clear_memfds();
void init_pointers();
int shim_v4l2_has_event(int fd);
void shim_v4l2_event_cleanup(int fd);

extern int (*real_open)(const char *pathname, int flags, ...);
extern int (*real_open64)(const char *pathname, int flags, ...);
extern int (*real_openat)(int dirfd, const char *pathname, int flags, ...);
extern int (*real_openat64)(int dirfd, const char *pathname, int flags, ...);
extern int (*real_close)(int fd);
extern int (*real_poll)(struct pollfd *fds, nfds_t nfds, int timeout);
extern int (*real_select)(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
extern int (*real_pselect)(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timespec *timeout, const sigset_t *sigmask);

#endif // SHIM_INTERNAL_H
