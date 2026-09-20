/*
 * Copyright (C) 2026 Luma Parallax
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Intercepts file system, memory, and raw syscalls to route virtual hardware into the Broker.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include "shim/internal.h"
#include "shim/log.h"
#include "vsouchi_ipc.h"

#ifdef __ANDROID__
typedef int ioctl_req_t;
#else
typedef unsigned long ioctl_req_t;
#endif

#define LOGI(...) SHIM_LOGI("vSouchi:ShimIO", __VA_ARGS__)

int g_vsouchi_fds[1024] = {0};
int g_shim_memfds[16] = {-1}; 
int g_broker_fd = -1; 
static int g_open_count = 0; 

int (*real_open)(const char *pathname, int flags, ...) = NULL;
int (*real_open64)(const char *pathname, int flags, ...) = NULL;
int (*real_openat)(int dirfd, const char *pathname, int flags, ...) = NULL;
int (*real_openat64)(int dirfd, const char *pathname, int flags, ...) = NULL;
int (*real_stat)(const char *pathname, struct stat *statbuf) = NULL;
int (*real_stat64)(const char *pathname, struct stat64 *statbuf) = NULL;
int (*real_lstat)(const char *pathname, struct stat *statbuf) = NULL;
int (*real_lstat64)(const char *pathname, struct stat64 *statbuf) = NULL;
int (*real_fstat)(int fd, struct stat *statbuf) = NULL;
int (*real_fstat64)(int fd, struct stat64 *statbuf) = NULL;
int (*real_fstatat)(int dirfd, const char *pathname, struct stat *statbuf, int flags) = NULL;
int (*real_fstatat64)(int dirfd, const char *pathname, struct stat64 *statbuf, int flags) = NULL;
int (*real_access)(const char *pathname, int mode) = NULL;
DIR *(*real_opendir)(const char *name) = NULL;
long (*real_syscall)(long number, ...) = NULL;
int (*real_close)(int fd) = NULL;
int (*real_ioctl)(int fd, ioctl_req_t request, ...) = NULL;
void *(*real_mmap)(void *addr, size_t length, int prot, int flags, int fd, off_t offset) = NULL;
void *(*real_mmap64)(void *addr, size_t length, int prot, int flags, int fd, off64_t offset) = NULL;
int (*real_poll)(struct pollfd *fds, nfds_t nfds, int timeout) = NULL;
int (*real_select)(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) = NULL;
int (*real_pselect)(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timespec *timeout, const sigset_t *sigmask) = NULL;
FILE *(*real_fopen)(const char *pathname, const char *mode) = NULL;
FILE *(*real_fopen64)(const char *pathname, const char *mode) = NULL;
ssize_t (*real_readlink)(const char *pathname, char *buf, size_t bufsiz) = NULL;

int shim_v4l2_handle_ioctl(int fd, int request, void* arg);

void shim_clear_memfds() {
    for (int i = 0; i < 16; i++) {
        if (g_shim_memfds[i] >= 0) {
            real_close(g_shim_memfds[i]); 
            g_shim_memfds[i] = -1;
        }
    }
}

void init_pointers() {
    if (real_open) return; 
    real_open = dlsym(RTLD_NEXT, "open");
    real_open64 = dlsym(RTLD_NEXT, "open64");
    real_openat = dlsym(RTLD_NEXT, "openat");
    real_openat64 = dlsym(RTLD_NEXT, "openat64");
    real_stat = dlsym(RTLD_NEXT, "stat");
    real_stat64 = dlsym(RTLD_NEXT, "stat64");
    real_lstat = dlsym(RTLD_NEXT, "lstat");
    real_lstat64 = dlsym(RTLD_NEXT, "lstat64");
    real_fstat = dlsym(RTLD_NEXT, "fstat");
    real_fstat64 = dlsym(RTLD_NEXT, "fstat64");
    real_fstatat = dlsym(RTLD_NEXT, "fstatat");
    real_fstatat64 = dlsym(RTLD_NEXT, "fstatat64");
    real_access = dlsym(RTLD_NEXT, "access");
    real_opendir = dlsym(RTLD_NEXT, "opendir");
    real_syscall = dlsym(RTLD_NEXT, "syscall");
    real_close = dlsym(RTLD_NEXT, "close");
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    real_mmap = dlsym(RTLD_NEXT, "mmap");
    real_mmap64 = dlsym(RTLD_NEXT, "mmap64");
    real_poll = dlsym(RTLD_NEXT, "poll");
    real_select = dlsym(RTLD_NEXT, "select");
    real_pselect = dlsym(RTLD_NEXT, "pselect");
    real_readlink = dlsym(RTLD_NEXT, "readlink");
    real_fopen = dlsym(RTLD_NEXT, "fopen");
    real_fopen64 = dlsym(RTLD_NEXT, "fopen64");
}

// Forward libv4l2 mmap variant to 64-bit offset handler
void *v4l2_mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset) {
    return mmap64(addr, length, prot, flags, fd, (off64_t)offset);
}

static int connect_to_broker() {
    pthread_mutex_lock(&g_ipc_mutex);
    
    int raw_fd = real_open ? real_open("/dev/null", O_RDWR) : -1;
    if (raw_fd < 0) {
        pthread_mutex_unlock(&g_ipc_mutex);
        return -1;
    }

    // F_DUPFD forces the kernel to assign an FD >= 10. 
    // Prevents target apps from accidentally closing the socket if they recycle stdin/stdout.
    int dummy_fd = fcntl(raw_fd, F_DUPFD, 10); 
    real_close(raw_fd); 

    if (dummy_fd < 0) {
        pthread_mutex_unlock(&g_ipc_mutex);
        return -1;
    }

    if (g_open_count == 0) {
        const char* base_dir = vsouchi_get_vdev_path();

        char real_path[256];
        snprintf(real_path, sizeof(real_path), "%s/video0", base_dir);

        g_broker_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        strncpy(addr.sun_path, real_path, sizeof(addr.sun_path) - 1);
        
        if (connect(g_broker_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            LOGI("Failed to connect to Broker at %s", real_path);
            close(g_broker_fd);
            g_broker_fd = -1;
            real_close(dummy_fd);
            pthread_mutex_unlock(&g_ipc_mutex);
            return -1;
        }
    }

    if (dummy_fd >= 0 && dummy_fd < 1024) g_vsouchi_fds[dummy_fd] = 1;
    g_open_count++;
    
    pthread_mutex_unlock(&g_ipc_mutex);
    return dummy_fd;
}

// --- VIRTUAL STAT GENERATORS ---
static int fill_virtual_videodev_stat(struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0666; 
    st->st_rdev = makedev(81, 0);
    st->st_uid = getuid();
    st->st_gid = getgid();
    return 0;
}

static int fill_virtual_videodev_stat64(struct stat64 *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0666; 
    st->st_rdev = makedev(81, 0);
    st->st_uid = getuid();
    st->st_gid = getgid();
    return 0;
}

// --- HELPER: VIRTUAL SYSFS UEVENT ---
static int provide_virtual_uevent() {
    const char* tmp_dir = getenv("TMPDIR");
    if (!tmp_dir) tmp_dir = "/data/data/com.termux/files/usr/tmp";
    
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.vsouchi_uevent", tmp_dir);
    
    int fd = real_open ? real_open(tmp_path, O_RDWR | O_CREAT | O_TRUNC, 0666) : -1;
    if (fd >= 0) {
        const char* uevent_data = "MAJOR=81\nMINOR=0\nDEVNAME=video0\n";
        write(fd, uevent_data, strlen(uevent_data));
        lseek(fd, 0, SEEK_SET);
        return fd;
    }
    return -1;
}

// ==========================================
// INTERCEPTORS: DIRECTORY READING
// ==========================================
DIR *opendir(const char *name) {
    init_pointers();
    if (name && strcmp(name, "/sys/class/video4linux") == 0) {
        const char* base_dir = vsouchi_get_vdev_path();
        char virtual_dir[256];
        snprintf(virtual_dir, sizeof(virtual_dir), "%s/sys/class/video4linux", base_dir);
        return real_opendir ? real_opendir(virtual_dir) : NULL;
    }
    return real_opendir ? real_opendir(name) : NULL;
}

// ==========================================
// INTERCEPTORS: STAT FAMILY
// ==========================================
int stat(const char *pathname, struct stat *statbuf) {
    init_pointers();
    if (pathname && strncmp(pathname, "/dev/video", 10) == 0) return fill_virtual_videodev_stat(statbuf);
    return real_stat ? real_stat(pathname, statbuf) : -1;
}

int stat64(const char *pathname, struct stat64 *statbuf) {
    init_pointers();
    if (pathname && strncmp(pathname, "/dev/video", 10) == 0) return fill_virtual_videodev_stat64(statbuf);
    return real_stat64 ? real_stat64(pathname, statbuf) : -1;
}

int lstat(const char *pathname, struct stat *statbuf) {
    init_pointers();
    if (pathname && strncmp(pathname, "/dev/video", 10) == 0) return fill_virtual_videodev_stat(statbuf);
    return real_lstat ? real_lstat(pathname, statbuf) : -1;
}

int lstat64(const char *pathname, struct stat64 *statbuf) {
    init_pointers();
    if (pathname && strncmp(pathname, "/dev/video", 10) == 0) return fill_virtual_videodev_stat64(statbuf);
    return real_lstat64 ? real_lstat64(pathname, statbuf) : -1;
}

int fstat(int fd, struct stat *statbuf) {
    init_pointers();
    if (fd >= 0 && fd < 1024 && g_vsouchi_fds[fd]) return fill_virtual_videodev_stat(statbuf);
    return real_fstat ? real_fstat(fd, statbuf) : -1;
}

int fstat64(int fd, struct stat64 *statbuf) {
    init_pointers();
    if (fd >= 0 && fd < 1024 && g_vsouchi_fds[fd]) return fill_virtual_videodev_stat64(statbuf);
    return real_fstat64 ? real_fstat64(fd, statbuf) : -1;
}

int fstatat(int dirfd, const char *pathname, struct stat *statbuf, int flags) {
    init_pointers();
    if (pathname && strncmp(pathname, "/dev/video", 10) == 0) return fill_virtual_videodev_stat(statbuf);
    return real_fstatat ? real_fstatat(dirfd, pathname, statbuf, flags) : -1;
}

int fstatat64(int dirfd, const char *pathname, struct stat64 *statbuf, int flags) {
    init_pointers();
    if (pathname && strncmp(pathname, "/dev/video", 10) == 0) return fill_virtual_videodev_stat64(statbuf);
    return real_fstatat64 ? real_fstatat64(dirfd, pathname, statbuf, flags) : -1;
}

int access(const char *pathname, int mode) {
    init_pointers();
    if (pathname && strncmp(pathname, "/dev/video", 10) == 0) return 0; 
    return real_access ? real_access(pathname, mode) : -1;
}

// ==========================================
// INTERCEPTORS: OPEN FAMILY
// ==========================================
int open(const char *pathname, int flags, ...) {
    init_pointers();
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    
    if (pathname && strstr(pathname, "/sys/dev/char/81:0")) {
        if (strstr(pathname, "uevent")) return provide_virtual_uevent();
        if (strstr(pathname, "device")) {
            const char* base_dir = vsouchi_get_vdev_path();
            char virtual_dir[256];
            snprintf(virtual_dir, sizeof(virtual_dir), "%s/sys/class/video4linux", base_dir);
            return real_open ? real_open(virtual_dir, flags, mode) : -1;
        }
    }
    if (pathname && strncmp(pathname, "/dev/video", 10) == 0) return connect_to_broker();
    return real_open ? real_open(pathname, flags, mode) : -1;
}

int open64(const char *pathname, int flags, ...) {
    init_pointers();
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    
    if (pathname && strstr(pathname, "/sys/dev/char/81:0")) {
        if (strstr(pathname, "uevent")) return provide_virtual_uevent();
        if (strstr(pathname, "device")) {
            const char* base_dir = vsouchi_get_vdev_path();
            char virtual_dir[256];
            snprintf(virtual_dir, sizeof(virtual_dir), "%s/sys/class/video4linux", base_dir);
            return real_open64 ? real_open64(virtual_dir, flags, mode) : -1;
        }
    }
    if (pathname && strncmp(pathname, "/dev/video", 10) == 0) return connect_to_broker();
    return real_open64 ? real_open64(pathname, flags, mode) : -1;
}

int openat(int dirfd, const char *pathname, int flags, ...) {
    init_pointers();
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    
    if (pathname && strstr(pathname, "/sys/dev/char/81:0")) {
        if (strstr(pathname, "uevent")) return provide_virtual_uevent();
        if (strstr(pathname, "device")) {
            const char* base_dir = vsouchi_get_vdev_path();
            char virtual_dir[256];
            snprintf(virtual_dir, sizeof(virtual_dir), "%s/sys/class/video4linux", base_dir);
            return real_openat ? real_openat(dirfd, virtual_dir, flags, mode) : -1;
        }
    }
    if (pathname && strcmp(pathname, "/sys/class/video4linux") == 0) {
        const char* base_dir = vsouchi_get_vdev_path();
        char virtual_dir[256];
        snprintf(virtual_dir, sizeof(virtual_dir), "%s/sys/class/video4linux", base_dir);
        return real_openat ? real_openat(dirfd, virtual_dir, flags, mode) : -1;
    }
    if (pathname && strncmp(pathname, "/dev/video", 10) == 0) return connect_to_broker();
    return real_openat ? real_openat(dirfd, pathname, flags, mode) : -1;
}

int openat64(int dirfd, const char *pathname, int flags, ...) {
    init_pointers();
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    
    if (pathname && strstr(pathname, "/sys/dev/char/81:0")) {
        if (strstr(pathname, "uevent")) return provide_virtual_uevent();
        if (strstr(pathname, "device")) {
            const char* base_dir = vsouchi_get_vdev_path();
            char virtual_dir[256];
            snprintf(virtual_dir, sizeof(virtual_dir), "%s/sys/class/video4linux", base_dir);
            return real_openat64 ? real_openat64(dirfd, virtual_dir, flags, mode) : -1;
        }
    }
    if (pathname && strcmp(pathname, "/sys/class/video4linux") == 0) {
        const char* base_dir = vsouchi_get_vdev_path();
        char virtual_dir[256];
        snprintf(virtual_dir, sizeof(virtual_dir), "%s/sys/class/video4linux", base_dir);
        return real_openat64 ? real_openat64(dirfd, virtual_dir, flags, mode) : -1;
    }
    if (pathname && strncmp(pathname, "/dev/video", 10) == 0) return connect_to_broker();
    return real_openat64 ? real_openat64(dirfd, pathname, flags, mode) : -1;
}

// ==========================================
// INTERCEPTORS: SYSFS TOPOLOGY & STANDARD IO
// ==========================================
ssize_t readlink(const char *pathname, char *buf, size_t bufsiz) {
    // Satisfy sysfs bus traversal checks
    if (pathname && (strstr(pathname, "81:0") || strstr(pathname, "video0"))) {
        const char *mock_link = "../../../devices/virtual/video4linux/video0";
        size_t len = strlen(mock_link);
        if (len > bufsiz) len = bufsiz;
        memcpy(buf, mock_link, len);
        return len;
    }

    if (!real_readlink) real_readlink = dlsym(RTLD_NEXT, "readlink");
    return real_readlink ? real_readlink(pathname, buf, bufsiz) : -1;
}

FILE *fopen(const char *pathname, const char *mode) {
    if (pathname && (strstr(pathname, "81:0") || strstr(pathname, "video0"))) {
        int fd = open(pathname, 0);
        if (fd >= 0) return fdopen(fd, mode);
    }

    if (!real_fopen) real_fopen = dlsym(RTLD_NEXT, "fopen");
    return real_fopen ? real_fopen(pathname, mode) : NULL;
}

FILE *fopen64(const char *pathname, const char *mode) {
    if (pathname && (strstr(pathname, "81:0") || strstr(pathname, "video0"))) {
        int fd = open64(pathname, 0);
        if (fd >= 0) return fdopen(fd, mode);
    }

    if (!real_fopen64) real_fopen64 = dlsym(RTLD_NEXT, "fopen64");
    return real_fopen64 ? real_fopen64(pathname, mode) : NULL;
}

int close(int fd) {
    init_pointers();
    if (fd >= 0 && fd < 1024 && g_vsouchi_fds[fd]) {
        pthread_mutex_lock(&g_ipc_mutex);
        g_vsouchi_fds[fd] = 0;
        g_open_count--;
        
        if (g_open_count == 0 && g_broker_fd != -1) {
            real_close(g_broker_fd);
            g_broker_fd = -1;
            shim_clear_memfds();
        }
        pthread_mutex_unlock(&g_ipc_mutex);
    }
    return real_close ? real_close(fd) : -1;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    init_pointers();
    if (fd >= 0 && fd < 1024 && g_vsouchi_fds[fd]) {
        int index = offset / 0x1000000; 
        if (index >= 0 && index < 16 && g_shim_memfds[index] >= 0) {
             return real_mmap ? real_mmap(addr, length, prot, flags, g_shim_memfds[index], 0) : MAP_FAILED;
        }
    }
    return real_mmap ? real_mmap(addr, length, prot, flags, fd, offset) : MAP_FAILED;
}

void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off64_t offset) {
    init_pointers();
    if (fd >= 0 && fd < 1024 && g_vsouchi_fds[fd]) {
        int index = (int)(offset / 0x1000000);
        if (index >= 0 && index < 16 && g_shim_memfds[index] >= 0) {
            if (real_mmap64) return real_mmap64(addr, length, prot, flags, g_shim_memfds[index], 0);
            return real_mmap ? real_mmap(addr, length, prot, flags, g_shim_memfds[index], 0) : MAP_FAILED;
        }
    }
    if (real_mmap64) return real_mmap64(addr, length, prot, flags, fd, offset);
    return real_mmap ? real_mmap(addr, length, prot, flags, fd, (off_t)offset) : MAP_FAILED;
}

int ioctl(int fd, ioctl_req_t request, ...) {
    init_pointers();
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);

    if (fd >= 0 && fd < 1024 && g_vsouchi_fds[fd]) {
        return shim_v4l2_handle_ioctl(fd, (int)request, arg);
    }
    
    return real_ioctl ? real_ioctl(fd, request, arg) : -1;
}

long syscall(long number, ...) {
    init_pointers();

    if (number != SYS_openat && number != SYS_ioctl && number != SYS_mmap && number != SYS_close) {
        va_list args;
        va_start(args, number);
        long a1 = va_arg(args, long); long a2 = va_arg(args, long);
        long a3 = va_arg(args, long); long a4 = va_arg(args, long);
        long a5 = va_arg(args, long); long a6 = va_arg(args, long);
        va_end(args);
        return real_syscall ? real_syscall(number, a1, a2, a3, a4, a5, a6) : -1;
    }

    va_list args;
    va_start(args, number);
    long a1 = va_arg(args, long); long a2 = va_arg(args, long);
    long a3 = va_arg(args, long); long a4 = va_arg(args, long);
    long a5 = va_arg(args, long); long a6 = va_arg(args, long);
    va_end(args);

    if (number == SYS_ioctl) {
        int fd = (int)a1;
        if (fd >= 0 && fd < 1024 && g_vsouchi_fds[fd]) return ioctl(fd, (unsigned long)a2, (void *)a3); 
    } 
    else if (number == SYS_mmap) {
        int fd = (int)a5; 
        if (fd >= 0 && fd < 1024 && g_vsouchi_fds[fd]) return (long)mmap((void *)a1, (size_t)a2, (int)a3, (int)a4, fd, (off_t)a6);
    } 
    else if (number == SYS_close) {
        int fd = (int)a1;
        if (fd >= 0 && fd < 1024 && g_vsouchi_fds[fd]) return close(fd);
    }
    else if (number == SYS_openat) {
        const char *path = (const char *)a2;
        if (path && path[0] == '/' && (path[1] == 'd' || path[1] == 's')) {
            
            // A. Catch raw Sysfs Hardware Probes
            if (strncmp(path, "/sys/dev/char/81:0", 18) == 0) {
                if (strstr(path, "uevent")) return provide_virtual_uevent();
                
                if (strstr(path, "device")) {
                    const char* base_dir = vsouchi_get_vdev_path();
                    char virtual_dir[256];
                    snprintf(virtual_dir, sizeof(virtual_dir), "%s/sys/class/video4linux", base_dir);
                    
                    return real_syscall ? real_syscall(SYS_openat, a1, (long)virtual_dir, a3, a4) : -1;
                }
            }
            
            // B. Catch raw Device Opens
            if (strncmp(path, "/dev/video", 10) == 0) {
                return connect_to_broker();
            }
            
            // C. Catch raw Sysfs Topology Scans
            if (strncmp(path, "/sys/class/video4linux", 22) == 0) {
                const char* base_dir = vsouchi_get_vdev_path();
                char virtual_dir[256];
                snprintf(virtual_dir, sizeof(virtual_dir), "%s/sys/class/video4linux", base_dir);
                return real_syscall ? real_syscall(SYS_openat, a1, (long)virtual_dir, a3, a4) : -1;
            }
        }
    }

    return real_syscall ? real_syscall(number, a1, a2, a3, a4, a5, a6) : -1;
}
