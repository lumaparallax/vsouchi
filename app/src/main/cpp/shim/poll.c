/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE
#include <poll.h>
#include <sys/select.h>
#include <unistd.h>
#include "shim/internal.h"
#include "vsouchi_ipc.h"

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    init_pointers();
    int has_camera = 0;
    int ready_count = 0;
    int elapsed = 0;

    while (1) {
        ready_count = 0;
        for (nfds_t i = 0; i < nfds; i++) {
            int fd = fds[i].fd;
            
            if (fd >= 0 && fd < 1024 && g_vsouchi_fds[fd]) {
                has_camera = 1;
                int ready = 0;
                
                if (shim_ipc_transaction(CMD_POLL, NULL, 0, &ready, sizeof(int)) == 0) {
                    if (ready) {
                        fds[i].revents |= fds[i].events;
                        ready_count++;
                    }
                } else {
                    fds[i].revents |= (POLLERR | POLLHUP); 
                    ready_count++;
                }
            }
        }

        if (!has_camera) return real_poll ? real_poll(fds, nfds, timeout) : -1;
        
        if (ready_count > 0) return ready_count;
        if (timeout >= 0 && elapsed >= timeout) return 0;

        usleep(10000); 
        if (timeout > 0) elapsed += 10;
    }
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    init_pointers();
    int has_camera = 0;
    int camera_fd = -1;

    for (int fd = 0; fd < nfds; fd++) {
        if (fd >= 0 && fd < 1024 && g_vsouchi_fds[fd]) {
            if (readfds && FD_ISSET(fd, readfds)) { has_camera = 1; camera_fd = fd; break; }
        }
    }

    if (!has_camera) return real_select ? real_select(nfds, readfds, writefds, exceptfds, timeout) : -1;

    long max_wait_ms = timeout ? (timeout->tv_sec * 1000 + timeout->tv_usec / 1000) : -1;
    long elapsed = 0;

    while (1) {
        int ready = 0;
        
        if (shim_ipc_transaction(CMD_POLL, NULL, 0, &ready, sizeof(int)) != 0) {
            if (exceptfds) { FD_ZERO(exceptfds); FD_SET(camera_fd, exceptfds); }
            return 1;
        }
        
        if (ready) {
            if (readfds) { FD_ZERO(readfds); FD_SET(camera_fd, readfds); }
            if (writefds) FD_ZERO(writefds);
            if (exceptfds) FD_ZERO(exceptfds);
            return 1;
        }

        if (max_wait_ms >= 0 && elapsed >= max_wait_ms) {
            if (readfds) FD_ZERO(readfds);
            if (writefds) FD_ZERO(writefds);
            if (exceptfds) FD_ZERO(exceptfds);
            return 0; 
        }

        usleep(10000); 
        if (max_wait_ms > 0) elapsed += 10;
    }
}

int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timespec *timeout, const sigset_t *sigmask) {
    init_pointers();
    struct timeval tv;
    if (timeout) {
        tv.tv_sec = timeout->tv_sec;
        tv.tv_usec = timeout->tv_nsec / 1000;
    }
    return select(nfds, readfds, writefds, exceptfds, timeout ? &tv : NULL);
}
