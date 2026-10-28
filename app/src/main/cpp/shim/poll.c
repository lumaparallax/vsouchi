/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE
#include <poll.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "shim/internal.h"

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    init_pointers();

    int v_pri_ready = 0;
    for (nfds_t i = 0; i < nfds; i++) {
        int fd = fds[i].fd;
        if (fd >= 0 && fd < 1024 && g_vsouchi_fds[fd]) {
            if ((fds[i].events & POLLPRI) && shim_v4l2_has_event(fd)) {
                fds[i].revents |= POLLPRI;
                v_pri_ready++;
            }
        }
    }

    if (v_pri_ready > 0) {
        if (real_poll) real_poll(fds, nfds, 0);
        for (nfds_t i = 0; i < nfds; i++) {
            int fd = fds[i].fd;
            if (fd >= 0 && fd < 1024 && g_vsouchi_fds[fd]) {
                if ((fds[i].events & POLLPRI) && shim_v4l2_has_event(fd)) {
                    fds[i].revents |= POLLPRI;
                }
            }
        }
        return v_pri_ready;
    }

    return real_poll ? real_poll(fds, nfds, timeout) : -1;
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    init_pointers();

    int vsouchi_except_ready = 0;
    if (exceptfds) {
        for (int fd = 0; fd < nfds && fd < 1024; fd++) {
            if (FD_ISSET(fd, exceptfds) && g_vsouchi_fds[fd]) {
                if (shim_v4l2_has_event(fd)) {
                    vsouchi_except_ready++;
                }
            }
        }
    }

    if (vsouchi_except_ready > 0) {
        struct timeval zero_tv = { 0, 0 };
        if (real_select) {
            real_select(nfds, readfds, writefds, NULL, &zero_tv);
        }
        if (exceptfds) {
            for (int fd = 0; fd < nfds && fd < 1024; fd++) {
                if (g_vsouchi_fds[fd]) {
                    if (!shim_v4l2_has_event(fd)) {
                        FD_CLR(fd, exceptfds);
                    }
                }
            }
        }
        return vsouchi_except_ready;
    }

    int has_vsouchi_except = 0;
    if (exceptfds) {
        for (int fd = 0; fd < nfds && fd < 1024; fd++) {
            if (FD_ISSET(fd, exceptfds) && g_vsouchi_fds[fd]) {
                has_vsouchi_except = 1;
                FD_CLR(fd, exceptfds);
            }
        }
    }

    int ret = real_select ? real_select(nfds, readfds, writefds, exceptfds, timeout) : -1;
    return ret;
}

int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timespec *timeout, const sigset_t *sigmask) {
    init_pointers();
    struct timeval tv;
    struct timeval *ptv = NULL;
    if (timeout) {
        tv.tv_sec = timeout->tv_sec;
        tv.tv_usec = timeout->tv_nsec / 1000;
        ptv = &tv;
    }
    return select(nfds, readfds, writefds, exceptfds, ptv);
}
