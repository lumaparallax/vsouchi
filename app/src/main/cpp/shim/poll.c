/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE
#include <poll.h>
#include <sys/select.h>
#include <unistd.h>
#include "shim/internal.h"

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    init_pointers();
    return real_poll ? real_poll(fds, nfds, timeout) : -1;
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    init_pointers();
    return real_select ? real_select(nfds, readfds, writefds, exceptfds, timeout) : -1;
}

int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timespec *timeout, const sigset_t *sigmask) {
    init_pointers();
    return real_pselect ? real_pselect(nfds, readfds, writefds, exceptfds, timeout, sigmask) : -1;
}
