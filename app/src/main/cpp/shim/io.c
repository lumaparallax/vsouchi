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
#include <sys/eventfd.h>
#include <dirent.h>
#include <linux/videodev2.h>
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
int g_vsouchi_fd_minor[1024] = {0};
int g_vsouchi_fd_flags[1024] = {0};
int g_shim_memfds[16] = {
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1
};
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
ssize_t (*real_readlinkat)(int dirfd, const char *pathname, char *buf, size_t bufsiz) = NULL;
int (*real_dup)(int oldfd) = NULL;
int (*real_dup2)(int oldfd, int newfd) = NULL;
int (*real_dup3)(int oldfd, int newfd, int flags) = NULL;

int shim_v4l2_handle_ioctl(int fd, int request, void* arg);

void shim_clear_memfds() {
    for (int i = 0; i < 16; i++) {
        if (g_shim_memfds[i] >= 0) {
            if (real_close) real_close(g_shim_memfds[i]);
            else close(g_shim_memfds[i]);
            g_shim_memfds[i] = -1;
        }
    }
}

__attribute__((constructor)) void init_pointers(void) {
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
    real_readlinkat = dlsym(RTLD_NEXT, "readlinkat");
    real_fopen = dlsym(RTLD_NEXT, "fopen");
    real_fopen64 = dlsym(RTLD_NEXT, "fopen64");
    real_dup = dlsym(RTLD_NEXT, "dup");
    real_dup2 = dlsym(RTLD_NEXT, "dup2");
    real_dup3 = dlsym(RTLD_NEXT, "dup3");
}

void *v4l2_mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset) {
    return mmap64(addr, length, prot, flags, fd, (off64_t)offset);
}

static int get_udev_data_minor(const char *path, int *out_minor) {
    if (!path) return 0;
    const char *p = NULL;
    if (strncmp(path, "/run/udev/data/c81:", 19) == 0) {
        p = path + 19;
    } else if (strncmp(path, "/var/run/udev/data/c81:", 23) == 0) {
        p = path + 23;
    }
    if (!p || *p < '0' || *p > '9') return 0;

    int minor = atoi(p);
    while (*p >= '0' && *p <= '9') p++;
    if (*p != '\0') return 0;

    if (out_minor) *out_minor = minor;
    return 1;
}

static int translate_udev_data_path(const char *pathname, char *out_path, size_t max_len) {
    int minor = 0;
    if (!get_udev_data_minor(pathname, &minor)) return 0;

    const char *base_dir = vsouchi_get_vdev_path();
    int n = snprintf(out_path, max_len, "%s/run/udev/data/c81:%d", base_dir, minor);
    return (n >= 0 && (size_t)n < max_len) ? 1 : 0;
}

static inline int is_potential_v4l2_path(const char* path) {
    if (!path) return 0;
    if (path[0] == '/') {
        if (path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && path[4] == '/' && path[5] == 'v') {
            return (strncmp(path, "/dev/video", 10) == 0 && path[10] >= '0' && path[10] <= '9');
        }
        if (path[1] == 's' && path[2] == 'y' && path[3] == 's' && path[4] == '/') {
            return (strncmp(path, "/sys/dev/char/81:", 17) == 0 ||
                    strncmp(path, "/sys/class/video4linux", 22) == 0 ||
                    strncmp(path, "/sys/devices/virtual/video4linux", 32) == 0);
        }
        if (strncmp(path, "/run/udev/data/c81:", 19) == 0 ||
            strncmp(path, "/var/run/udev/data/c81:", 23) == 0) {
            return 1;
        }
    } else {
        if (strncmp(path, "video", 5) == 0 && path[5] >= '0' && path[5] <= '9') return 1;
    }
    return 0;
}

static int parse_minor_from_path(const char* path) {
    if (!path) return 0;
    const char* p = strstr(path, "81:");
    if (p && p[3] >= '0' && p[3] <= '9') {
        return atoi(p + 3);
    }
    p = path;
    while ((p = strstr(p, "video")) != NULL) {
        if (strncmp(p, "video4linux", 11) == 0) {
            p += 11;
            continue;
        }
        if (p[5] >= '0' && p[5] <= '9') {
            return atoi(p + 5);
        }
        p += 5;
    }
    return 0;
}

static const char* get_rel_video_node(const char* pathname) {
    if (!pathname) return NULL;
    if (strncmp(pathname, "/dev/video", 10) == 0 && pathname[10] >= '0' && pathname[10] <= '9') {
        return pathname + 5;
    }
    if (strncmp(pathname, "video", 5) == 0 && pathname[5] >= '0' && pathname[5] <= '9') {
        return pathname;
    }
    return NULL;
}

static int get_virtual_sock_path(const char* pathname, char* out_path, size_t max_len) {
    const char* rel_node = get_rel_video_node(pathname);
    if (!rel_node) return 0;

    const char* base_dir = vsouchi_get_vdev_path();
    int n = snprintf(out_path, max_len, "%s/%s", base_dir, rel_node);
    return (n >= 0 && (size_t)n < max_len) ? 1 : 0;
}

static int is_virtual_node_active(const char* pathname) {
    if (!pathname) return 0;
    init_pointers();

    char sock_path[VSOUCHI_PATH_MAX];
    int udev_minor = 0;
    if (strncmp(pathname, "/sys/", 5) == 0) {
        int minor = parse_minor_from_path(pathname);
        const char* base_dir = vsouchi_get_vdev_path();
        int n = snprintf(sock_path, sizeof(sock_path), "%s/video%d", base_dir, minor);
        if (n < 0 || (size_t)n >= sizeof(sock_path)) return 0;
    } else if (get_udev_data_minor(pathname, &udev_minor)) {
        const char* base_dir = vsouchi_get_vdev_path();
        int n = snprintf(sock_path, sizeof(sock_path), "%s/video%d", base_dir, udev_minor);
        if (n < 0 || (size_t)n >= sizeof(sock_path)) return 0;
    } else if (!get_virtual_sock_path(pathname, sock_path, sizeof(sock_path))) {
        return 0;
    }

    return (real_access ? (real_access(sock_path, F_OK) == 0) : (access(sock_path, F_OK) == 0));
}

static int ensure_broker_connected_locked(const char* base_dir, const char* rel_node, const char* real_path) {
    if (!base_dir || !rel_node || !real_path) return -1;
    init_pointers();

    if (real_access ? (real_access(real_path, F_OK) != 0) : (access(real_path, F_OK) != 0)) {
        return -1;
    }

    if (g_broker_fd >= 0) return 0;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int dir_fd = -1;
    size_t path_len = strlen(real_path);
    if (path_len < sizeof(addr.sun_path)) {
        memcpy(addr.sun_path, real_path, path_len + 1);
    } else {
        dir_fd = real_open ? real_open(base_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC)
                           : open(base_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir_fd < 0) {
            if (real_close) real_close(fd);
            else close(fd);
            return -1;
        }
        int n = snprintf(addr.sun_path, sizeof(addr.sun_path), "/proc/self/fd/%d/%s", dir_fd, rel_node);
        if (n < 0 || (size_t)n >= sizeof(addr.sun_path)) {
            if (real_close) real_close(dir_fd);
            else close(dir_fd);
            if (real_close) real_close(fd);
            else close(fd);
            errno = ENAMETOOLONG;
            return -1;
        }
    }

    int rc = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (dir_fd >= 0) {
        int saved_err = errno;
        if (real_close) real_close(dir_fd);
        else close(dir_fd);
        errno = saved_err;
    }
    if (rc != 0) {
        if (real_close) real_close(fd);
        else close(fd);
        return -1;
    }

    IpcHandshakePacket hs = {
        .magic = VSOUCHI_IPC_MAGIC,
        .version = VSOUCHI_PROTOCOL_VER,
        .reserved = 0
    };

    if (safe_write(fd, &hs, sizeof(hs)) != sizeof(hs)) {
        int saved_err = errno;
        if (real_close) real_close(fd);
        else close(fd);
        errno = saved_err;
        return -1;
    }

    int32_t ack = 0;
    if (safe_read(fd, &ack, sizeof(int32_t)) != sizeof(int32_t) || ack != 0) {
        if (ack == -EPROTO) {
            fprintf(stderr, "\n[vSouchi Error] Protocol version mismatch between libvsouchi.so and vSouchi app!\n"
                            "You are running an incompatible or outdated shim library. Please update libvsouchi.so.\n\n");
        }
        if (real_close) real_close(fd);
        else close(fd);
        errno = (ack < 0) ? -ack : EPROTO;
        return -1;
    }

    g_broker_fd = fd;
    return 0;
}

static int connect_to_broker(const char* pathname, int flags) {
    const char* rel_node = get_rel_video_node(pathname);
    if (!rel_node) {
        return -1;
    }
    const char* base_dir = vsouchi_get_vdev_path();
    char real_path[VSOUCHI_PATH_MAX];
    int n = snprintf(real_path, sizeof(real_path), "%s/%s", base_dir, rel_node);
    if (n < 0 || (size_t)n >= sizeof(real_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    pthread_mutex_lock(&g_ipc_mutex);

    if (ensure_broker_connected_locked(base_dir, rel_node, real_path) != 0) {
        pthread_mutex_unlock(&g_ipc_mutex);
        return -1;
    }

    int eflags = EFD_CLOEXEC | EFD_SEMAPHORE;
    if (flags & O_NONBLOCK) {
        eflags |= EFD_NONBLOCK;
    }

    int raw_fd = eventfd(0, eflags);
    if (raw_fd < 0) {
        pthread_mutex_unlock(&g_ipc_mutex);
        return -1;
    }

    int dummy_fd = fcntl(raw_fd, F_DUPFD, 10);
    if (real_close) real_close(raw_fd);
    else close(raw_fd);

    if (dummy_fd < 0) {
        pthread_mutex_unlock(&g_ipc_mutex);
        return -1;
    }

    if (dummy_fd < 1024) {
        g_vsouchi_fds[dummy_fd] = 1;
        g_vsouchi_fd_minor[dummy_fd] = parse_minor_from_path(pathname);
        g_vsouchi_fd_flags[dummy_fd] = flags;
    }
    g_open_count++;

    pthread_mutex_unlock(&g_ipc_mutex);
    return dummy_fd;
}

/* Virtual stat helpers */
static int fill_virtual_videodev_stat(struct stat *st, int minor) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0666;
    st->st_rdev = makedev(81, minor);
    st->st_uid = getuid();
    st->st_gid = getgid();
    return 0;
}

static int fill_virtual_videodev_stat64(struct stat64 *st, int minor) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0666;
    st->st_rdev = makedev(81, minor);
    st->st_uid = getuid();
    st->st_gid = getgid();
    return 0;
}

/* Virtual sysfs uevent helper */
static int provide_virtual_uevent(int minor) {
    const char* base_dir = vsouchi_get_vdev_path();
    char tmp_path[VSOUCHI_PATH_MAX];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s/.vsouchi_uevent_%d", base_dir, minor);
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) return -1;

    init_pointers();
    int fd = real_open ? real_open(tmp_path, O_RDWR | O_CREAT | O_TRUNC, 0666) : -1;
    if (fd >= 0) {
        char uevent_data[128];
        snprintf(uevent_data, sizeof(uevent_data), "MAJOR=81\nMINOR=%d\nDEVNAME=video%d\n", minor, minor);
        write(fd, uevent_data, strlen(uevent_data));
        lseek(fd, 0, SEEK_SET);
        return fd;
    }
    return -1;
}

static int translate_sysfs_path(const char *pathname, char *out_path, size_t max_len) {
    if (!pathname) return 0;
    const char *base_dir = vsouchi_get_vdev_path();

    if (strncmp(pathname, "/sys/class/video4linux", 22) == 0 &&
        (pathname[22] == '\0' || pathname[22] == '/')) {
        int n = snprintf(out_path, max_len, "%s/sys/%s", base_dir, pathname + 5);
        return (n >= 0 && (size_t)n < max_len) ? 1 : 0;
    }

    if (strncmp(pathname, "/sys/devices/virtual/video4linux", 32) == 0 &&
        (pathname[32] == '\0' || pathname[32] == '/')) {
        int n = snprintf(out_path, max_len, "%s/sys/class/video4linux%s", base_dir, pathname + 32);
        return (n >= 0 && (size_t)n < max_len) ? 1 : 0;
    }

    if (strncmp(pathname, "/sys/dev/char/81:", 17) == 0) {
        const char *rest = pathname + 17;
        if (*rest < '0' || *rest > '9') return 0;

        int minor = atoi(rest);
        while (*rest >= '0' && *rest <= '9') rest++;
        if (*rest != '\0' && *rest != '/') return 0;

        char vpath[VSOUCHI_PATH_MAX];
        int vn = snprintf(vpath, sizeof(vpath), "%s/video%d", base_dir, minor);
        if (vn < 0 || (size_t)vn >= sizeof(vpath)) return 0;
        if (!(real_access ? (real_access(vpath, F_OK) == 0) : (access(vpath, F_OK) == 0))) {
            return 0;
        }

        int n;
        if (strcmp(rest, "/device") == 0) {
            n = snprintf(out_path, max_len, "%s/sys/class/video4linux", base_dir);
        } else {
            n = snprintf(out_path, max_len, "%s/sys/class/video4linux/video%d%s", base_dir, minor, rest);
        }
        return (n >= 0 && (size_t)n < max_len) ? 1 : 0;
    }

    return 0;
}

/* Directory interceptors */
DIR *opendir(const char *name) {
    init_pointers();
    char virtual_dir[VSOUCHI_PATH_MAX];
    if (translate_sysfs_path(name, virtual_dir, sizeof(virtual_dir))) {
        DIR *vdir = real_opendir ? real_opendir(virtual_dir) : NULL;
        if (vdir) return vdir;
    }
    return real_opendir ? real_opendir(name) : NULL;
}

/* Stat interceptors */
static int is_sys_dir_target(const char *path) {
    if (strcmp(path, "/sys/class/video4linux") == 0 ||
        strcmp(path, "/sys/class/video4linux/") == 0 ||
        strcmp(path, "/sys/devices/virtual/video4linux") == 0 ||
        strcmp(path, "/sys/devices/virtual/video4linux/") == 0) {
        return 1;
    }
    if (strncmp(path, "/sys/devices/virtual/video4linux/video", 38) == 0) {
        const char *p = path + 38;
        while (*p >= '0' && *p <= '9') p++;
        return (*p == '\0' || strcmp(p, "/") == 0);
    }
    return 0;
}

static int is_sys_symlink_target(const char *path) {
    if (strncmp(path, "/sys/dev/char/81:", 17) == 0) {
        const char *p = path + 17;
        while (*p >= '0' && *p <= '9') p++;
        return (*p == '\0' || strcmp(p, "/device") == 0 || strcmp(p, "/subsystem") == 0);
    }
    if (strncmp(path, "/sys/class/video4linux/video", 28) == 0) {
        const char *p = path + 28;
        while (*p >= '0' && *p <= '9') p++;
        return (*p == '\0' || strcmp(p, "/device") == 0 || strcmp(p, "/subsystem") == 0);
    }
    if (strncmp(path, "/sys/devices/virtual/video4linux/video", 38) == 0) {
        const char *p = path + 38;
        while (*p >= '0' && *p <= '9') p++;
        return (strcmp(p, "/subsystem") == 0);
    }
    return 0;
}

static int fill_virtual_path_stat(const char *pathname, struct stat *st, int is_lstat) {
    char v_udev[VSOUCHI_PATH_MAX];
    if (translate_udev_data_path(pathname, v_udev, sizeof(v_udev))) {
        if (real_stat && real_stat(v_udev, st) == 0) return 0;
        memset(st, 0, sizeof(*st));
        st->st_uid = getuid();
        st->st_gid = getgid();
        st->st_mode = S_IFREG | 0644;
        st->st_size = 128;
        return 0;
    }

    int minor = parse_minor_from_path(pathname);
    if (strncmp(pathname, "/sys/", 5) != 0) {
        return fill_virtual_videodev_stat(st, minor);
    }

    memset(st, 0, sizeof(*st));
    st->st_uid = getuid();
    st->st_gid = getgid();

    if (is_sys_dir_target(pathname)) {
        st->st_mode = S_IFDIR | 0755;
    } else if (is_sys_symlink_target(pathname)) {
        st->st_mode = is_lstat ? (S_IFLNK | 0777) : (S_IFDIR | 0755);
    } else {
        st->st_mode = S_IFREG | 0644;
        st->st_size = 4096;
    }
    return 0;
}

static int fill_virtual_path_stat64(const char *pathname, struct stat64 *st, int is_lstat) {
    char v_udev[VSOUCHI_PATH_MAX];
    if (translate_udev_data_path(pathname, v_udev, sizeof(v_udev))) {
        if (real_stat64 && real_stat64(v_udev, st) == 0) return 0;
        memset(st, 0, sizeof(*st));
        st->st_uid = getuid();
        st->st_gid = getgid();
        st->st_mode = S_IFREG | 0644;
        st->st_size = 128;
        return 0;
    }

    int minor = parse_minor_from_path(pathname);
    if (strncmp(pathname, "/sys/", 5) != 0) {
        return fill_virtual_videodev_stat64(st, minor);
    }

    memset(st, 0, sizeof(*st));
    st->st_uid = getuid();
    st->st_gid = getgid();

    if (is_sys_dir_target(pathname)) {
        st->st_mode = S_IFDIR | 0755;
    } else if (is_sys_symlink_target(pathname)) {
        st->st_mode = is_lstat ? (S_IFLNK | 0777) : (S_IFDIR | 0755);
    } else {
        st->st_mode = S_IFREG | 0644;
        st->st_size = 4096;
    }
    return 0;
}

int stat(const char *pathname, struct stat *statbuf) {
    init_pointers();
    if (!is_potential_v4l2_path(pathname)) return real_stat ? real_stat(pathname, statbuf) : -1;
    if (is_virtual_node_active(pathname)) return fill_virtual_path_stat(pathname, statbuf, 0);
    return real_stat ? real_stat(pathname, statbuf) : -1;
}

int stat64(const char *pathname, struct stat64 *statbuf) {
    init_pointers();
    if (!is_potential_v4l2_path(pathname)) return real_stat64 ? real_stat64(pathname, statbuf) : -1;
    if (is_virtual_node_active(pathname)) return fill_virtual_path_stat64(pathname, statbuf, 0);
    return real_stat64 ? real_stat64(pathname, statbuf) : -1;
}

int lstat(const char *pathname, struct stat *statbuf) {
    init_pointers();
    if (!is_potential_v4l2_path(pathname)) return real_lstat ? real_lstat(pathname, statbuf) : -1;
    if (is_virtual_node_active(pathname)) return fill_virtual_path_stat(pathname, statbuf, 1);
    return real_lstat ? real_lstat(pathname, statbuf) : -1;
}

int lstat64(const char *pathname, struct stat64 *statbuf) {
    init_pointers();
    if (!is_potential_v4l2_path(pathname)) return real_lstat64 ? real_lstat64(pathname, statbuf) : -1;
    if (is_virtual_node_active(pathname)) return fill_virtual_path_stat64(pathname, statbuf, 1);
    return real_lstat64 ? real_lstat64(pathname, statbuf) : -1;
}

int fstat(int fd, struct stat *statbuf) {
    init_pointers();
    if (fd >= 0 && fd < 1024 && g_vsouchi_fds[fd]) return fill_virtual_videodev_stat(statbuf, g_vsouchi_fd_minor[fd]);
    return real_fstat ? real_fstat(fd, statbuf) : -1;
}

int fstat64(int fd, struct stat64 *statbuf) {
    init_pointers();
    if (fd >= 0 && fd < 1024 && g_vsouchi_fds[fd]) return fill_virtual_videodev_stat64(statbuf, g_vsouchi_fd_minor[fd]);
    return real_fstat64 ? real_fstat64(fd, statbuf) : -1;
}

int fstatat(int dirfd, const char *pathname, struct stat *statbuf, int flags) {
    init_pointers();
    if (!is_potential_v4l2_path(pathname)) return real_fstatat ? real_fstatat(dirfd, pathname, statbuf, flags) : -1;
    if (is_virtual_node_active(pathname)) return fill_virtual_path_stat(pathname, statbuf, (flags & AT_SYMLINK_NOFOLLOW) != 0);
    return real_fstatat ? real_fstatat(dirfd, pathname, statbuf, flags) : -1;
}

int fstatat64(int dirfd, const char *pathname, struct stat64 *statbuf, int flags) {
    init_pointers();
    if (!is_potential_v4l2_path(pathname)) return real_fstatat64 ? real_fstatat64(dirfd, pathname, statbuf, flags) : -1;
    if (is_virtual_node_active(pathname)) return fill_virtual_path_stat64(pathname, statbuf, (flags & AT_SYMLINK_NOFOLLOW) != 0);
    return real_fstatat64 ? real_fstatat64(dirfd, pathname, statbuf, flags) : -1;
}

int access(const char *pathname, int mode) {
    init_pointers();
    if (!is_potential_v4l2_path(pathname)) return real_access ? real_access(pathname, mode) : -1;
    if (is_virtual_node_active(pathname)) return 0;
    return real_access ? real_access(pathname, mode) : -1;
}

/* Open interceptors */
int open(const char *pathname, int flags, ...) {
    init_pointers();
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    if (!is_potential_v4l2_path(pathname)) return real_open ? real_open(pathname, flags, mode) : -1;

    char v_udev[VSOUCHI_PATH_MAX];
    if (translate_udev_data_path(pathname, v_udev, sizeof(v_udev))) {
        int fd = real_open ? real_open(v_udev, flags, mode) : -1;
        if (fd >= 0) return fd;
    }

    char v_sysfs[VSOUCHI_PATH_MAX];
    if (translate_sysfs_path(pathname, v_sysfs, sizeof(v_sysfs))) {
        int fd = real_open ? real_open(v_sysfs, flags, mode) : -1;
        if (fd >= 0) return fd;
        if (strstr(pathname, "uevent")) return provide_virtual_uevent(parse_minor_from_path(pathname));
    }

    if (strncmp(pathname, "/dev/video", 10) == 0 || (strncmp(pathname, "video", 5) == 0 && pathname[5] >= '0' && pathname[5] <= '9')) {
        int vfd = connect_to_broker(pathname, flags);
        if (vfd >= 0) return vfd;
    }
    return real_open ? real_open(pathname, flags, mode) : -1;
}

int open64(const char *pathname, int flags, ...) {
    init_pointers();
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    if (!is_potential_v4l2_path(pathname)) return real_open64 ? real_open64(pathname, flags, mode) : -1;

    char v_udev[VSOUCHI_PATH_MAX];
    if (translate_udev_data_path(pathname, v_udev, sizeof(v_udev))) {
        int fd = real_open64 ? real_open64(v_udev, flags, mode) : -1;
        if (fd >= 0) return fd;
    }

    char v_sysfs[VSOUCHI_PATH_MAX];
    if (translate_sysfs_path(pathname, v_sysfs, sizeof(v_sysfs))) {
        int fd = real_open64 ? real_open64(v_sysfs, flags, mode) : -1;
        if (fd >= 0) return fd;
        if (strstr(pathname, "uevent")) return provide_virtual_uevent(parse_minor_from_path(pathname));
    }

    if (strncmp(pathname, "/dev/video", 10) == 0 || (strncmp(pathname, "video", 5) == 0 && pathname[5] >= '0' && pathname[5] <= '9')) {
        int vfd = connect_to_broker(pathname, flags);
        if (vfd >= 0) return vfd;
    }
    return real_open64 ? real_open64(pathname, flags, mode) : -1;
}

int openat(int dirfd, const char *pathname, int flags, ...) {
    init_pointers();
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    if (!is_potential_v4l2_path(pathname)) return real_openat ? real_openat(dirfd, pathname, flags, mode) : -1;

    char v_udev[VSOUCHI_PATH_MAX];
    if (translate_udev_data_path(pathname, v_udev, sizeof(v_udev))) {
        int fd = real_openat ? real_openat(dirfd, v_udev, flags, mode) : -1;
        if (fd >= 0) return fd;
    }

    char v_sysfs[VSOUCHI_PATH_MAX];
    if (translate_sysfs_path(pathname, v_sysfs, sizeof(v_sysfs))) {
        int fd = real_openat ? real_openat(dirfd, v_sysfs, flags, mode) : -1;
        if (fd >= 0) return fd;
        if (strstr(pathname, "uevent")) return provide_virtual_uevent(parse_minor_from_path(pathname));
    }

    if (strncmp(pathname, "/dev/video", 10) == 0 || (strncmp(pathname, "video", 5) == 0 && pathname[5] >= '0' && pathname[5] <= '9')) {
        int vfd = connect_to_broker(pathname, flags);
        if (vfd >= 0) return vfd;
    }
    return real_openat ? real_openat(dirfd, pathname, flags, mode) : -1;
}

int openat64(int dirfd, const char *pathname, int flags, ...) {
    init_pointers();
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    if (!is_potential_v4l2_path(pathname)) return real_openat64 ? real_openat64(dirfd, pathname, flags, mode) : -1;

    char v_udev[VSOUCHI_PATH_MAX];
    if (translate_udev_data_path(pathname, v_udev, sizeof(v_udev))) {
        int fd = real_openat64 ? real_openat64(dirfd, v_udev, flags, mode) : -1;
        if (fd >= 0) return fd;
    }

    char v_sysfs[VSOUCHI_PATH_MAX];
    if (translate_sysfs_path(pathname, v_sysfs, sizeof(v_sysfs))) {
        int fd = real_openat64 ? real_openat64(dirfd, v_sysfs, flags, mode) : -1;
        if (fd >= 0) return fd;
        if (strstr(pathname, "uevent")) return provide_virtual_uevent(parse_minor_from_path(pathname));
    }

    if (strncmp(pathname, "/dev/video", 10) == 0 || (strncmp(pathname, "video", 5) == 0 && pathname[5] >= '0' && pathname[5] <= '9')) {
        int vfd = connect_to_broker(pathname, flags);
        if (vfd >= 0) return vfd;
    }
    return real_openat64 ? real_openat64(dirfd, pathname, flags, mode) : -1;
}

/* Sysfs topology and standard I/O interceptors */
static ssize_t handle_virtual_readlink(const char *pathname, char *buf, size_t bufsiz, int *handled) {
    *handled = 0;
    if (!pathname) return -1;

    const char *sub = NULL;
    if (strncmp(pathname, "/sys/dev/char/81:", 17) == 0) {
        sub = pathname + 17;
    } else if (strncmp(pathname, "/sys/class/video4linux/video", 28) == 0) {
        sub = pathname + 28;
    } else if (strncmp(pathname, "/sys/devices/virtual/video4linux/video", 38) == 0) {
        sub = pathname + 38;
    }

    if (!sub || *sub < '0' || *sub > '9') return -1;

    int minor = atoi(sub);
    while (*sub >= '0' && *sub <= '9') sub++;

    int is_dev_link = (*sub == '\0' && strncmp(pathname, "/sys/devices/", 13) != 0);
    int is_subsys_link = (strcmp(sub, "/subsystem") == 0);
    if (!is_dev_link && !is_subsys_link) return -1;

    const char *base_dir = vsouchi_get_vdev_path();
    char vpath[VSOUCHI_PATH_MAX];
    int n = snprintf(vpath, sizeof(vpath), "%s/video%d", base_dir, minor);
    if (n < 0 || (size_t)n >= sizeof(vpath)) return -1;
    if (!(real_access ? (real_access(vpath, F_OK) == 0) : (access(vpath, F_OK) == 0))) {
        return -1;
    }

    *handled = 1;
    char mock_link[128];
    if (is_subsys_link) {
        snprintf(mock_link, sizeof(mock_link), "../../../../class/video4linux");
    } else {
        snprintf(mock_link, sizeof(mock_link), "../../../devices/virtual/video4linux/video%d", minor);
    }

    size_t len = strlen(mock_link);
    if (len > bufsiz) len = bufsiz;
    memcpy(buf, mock_link, len);
    return (ssize_t)len;
}

ssize_t readlink(const char *pathname, char *buf, size_t bufsiz) {
    init_pointers();
    int handled = 0;
    ssize_t res = handle_virtual_readlink(pathname, buf, bufsiz, &handled);
    if (handled) return res;
    return real_readlink ? real_readlink(pathname, buf, bufsiz) : -1;
}

ssize_t readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz) {
    init_pointers();
    int handled = 0;
    ssize_t res = handle_virtual_readlink(pathname, buf, bufsiz, &handled);
    if (handled) return res;
    return real_readlinkat ? real_readlinkat(dirfd, pathname, buf, bufsiz) : -1;
}

FILE *fopen(const char *pathname, const char *mode) {
    init_pointers();
    if (!is_potential_v4l2_path(pathname)) return real_fopen ? real_fopen(pathname, mode) : NULL;
    int fd = open(pathname, 0);
    if (fd >= 0) return fdopen(fd, mode);
    return real_fopen ? real_fopen(pathname, mode) : NULL;
}

FILE *fopen64(const char *pathname, const char *mode) {
    init_pointers();
    if (!is_potential_v4l2_path(pathname)) return real_fopen64 ? real_fopen64(pathname, mode) : NULL;
    int fd = open64(pathname, 0);
    if (fd >= 0) return fdopen(fd, mode);
    return real_fopen64 ? real_fopen64(pathname, mode) : NULL;
}

extern int g_buffer_owner_fd;

int close(int fd) {
    init_pointers();
    int saved_errno = errno;

    if (fd >= 0 && fd < 1024 && g_vsouchi_fds[fd]) {
        /* Purge control subscriptions before taking IPC lock to avoid lock inversion */
        shim_v4l2_event_cleanup(fd);

        pthread_mutex_lock(&g_ipc_mutex);

        if (shim_v4l2_check_and_clear_buffer_owner(fd)) {
            if (g_broker_fd != -1) {
                int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                uint32_t cmd = CMD_STREAMOFF;
                struct iovec iov[2] = { { &cmd, sizeof(uint32_t) }, { &type, sizeof(int) } };
                struct msghdr msg = { .msg_iov = iov, .msg_iovlen = 2 };
                sendmsg(g_broker_fd, &msg, MSG_NOSIGNAL);

                int32_t status = 0;
                safe_read(g_broker_fd, &status, sizeof(int32_t));
                char ack;
                safe_read(g_broker_fd, &ack, 1);
            }
        }

        g_vsouchi_fds[fd] = 0;
        g_vsouchi_fd_minor[fd] = 0;
        g_vsouchi_fd_flags[fd] = 0;
        g_open_count--;
        if (g_open_count < 0) g_open_count = 0;

        if (g_open_count == 0 && g_broker_fd != -1) {
            real_close(g_broker_fd);
            g_broker_fd = -1;
            shim_clear_memfds();
            shim_v4l2_reset_nodes();
        }
        pthread_mutex_unlock(&g_ipc_mutex);
    }
    int ret = real_close ? real_close(fd) : -1;
    if (ret == 0) {
        errno = saved_errno;
    }
    return ret;
}

static void track_dup_fd(int oldfd, int newfd, int cloexec) {
    if (newfd < 0 || newfd >= 1024) return;
    pthread_mutex_lock(&g_ipc_mutex);
    if (oldfd >= 0 && oldfd < 1024 && g_vsouchi_fds[oldfd]) {
        g_vsouchi_fds[newfd] = 1;
        g_vsouchi_fd_minor[newfd] = g_vsouchi_fd_minor[oldfd];
        g_vsouchi_fd_flags[newfd] = cloexec
            ? (g_vsouchi_fd_flags[oldfd] | O_CLOEXEC)
            : (g_vsouchi_fd_flags[oldfd] & ~O_CLOEXEC);
        g_open_count++;
    }
    pthread_mutex_unlock(&g_ipc_mutex);
}

int dup(int oldfd) {
    init_pointers();
    int newfd = real_dup ? real_dup(oldfd) : -1;
    if (newfd >= 0 && oldfd != newfd) track_dup_fd(oldfd, newfd, 0);
    return newfd;
}

int dup2(int oldfd, int newfd) {
    init_pointers();
    if (oldfd == newfd) return real_dup2 ? real_dup2(oldfd, newfd) : -1;
    if (newfd >= 0 && newfd < 1024 && g_vsouchi_fds[newfd]) close(newfd);
    int res = real_dup2 ? real_dup2(oldfd, newfd) : -1;
    if (res >= 0) track_dup_fd(oldfd, res, 0);
    return res;
}

int dup3(int oldfd, int newfd, int flags) {
    init_pointers();
    if (oldfd == newfd) { errno = EINVAL; return -1; }
    if (newfd >= 0 && newfd < 1024 && g_vsouchi_fds[newfd]) close(newfd);
    int res = real_dup3 ? real_dup3(oldfd, newfd, flags) : -1;
    if (res >= 0) track_dup_fd(oldfd, res, (flags & O_CLOEXEC) != 0);
    return res;
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

    va_list args;
    va_start(args, number);
    long a1 = va_arg(args, long);
    long a2 = va_arg(args, long);
    long a3 = va_arg(args, long);
    long a4 = va_arg(args, long);
    long a5 = va_arg(args, long);
    long a6 = va_arg(args, long);
    va_end(args);

    if (number == SYS_ioctl) {
        int fd = (int)a1;
        if (fd >= 0 && fd < 1024 && g_vsouchi_fds[fd]) {
            return ioctl(fd, (ioctl_req_t)a2, (void *)a3);
        }
    }
    else if (number == SYS_mmap) {
        int fd = (int)a5;
        if (fd >= 0 && fd < 1024 && g_vsouchi_fds[fd]) {
            return (long)mmap((void *)a1, (size_t)a2, (int)a3, (int)a4, fd, (off_t)a6);
        }
    }
    else if (number == SYS_openat) {
        const char *path = (const char *)a2;
        int flags = (int)a3;
        if (is_potential_v4l2_path(path)) {
            char v_udev[VSOUCHI_PATH_MAX];
            if (translate_udev_data_path(path, v_udev, sizeof(v_udev))) {
                long vfd = real_syscall ? real_syscall(SYS_openat, a1, (long)v_udev, a3, a4) : -1;
                if (vfd >= 0) return vfd;
            }
            char v_sysfs[VSOUCHI_PATH_MAX];
            if (translate_sysfs_path(path, v_sysfs, sizeof(v_sysfs))) {
                long vfd = real_syscall ? real_syscall(SYS_openat, a1, (long)v_sysfs, a3, a4) : -1;
                if (vfd >= 0) return vfd;
                if (strstr(path, "uevent")) return provide_virtual_uevent(parse_minor_from_path(path));
            }
            if (strncmp(path, "/dev/video", 10) == 0 || (strncmp(path, "video", 5) == 0 && path[5] >= '0' && path[5] <= '9')) {
                int vfd = connect_to_broker(path, flags);
                if (vfd >= 0) return vfd;
            }
        }
    }
#ifdef SYS_readlinkat
    else if (number == SYS_readlinkat) {
        int handled = 0;
        ssize_t res = handle_virtual_readlink((const char *)a2, (char *)a3, (size_t)a4, &handled);
        if (handled) return (long)res;
    }
#endif
#ifdef SYS_dup
    else if (number == SYS_dup) {
        return (long)dup((int)a1);
    }
#endif
#ifdef SYS_dup2
    else if (number == SYS_dup2) {
        return (long)dup2((int)a1, (int)a2);
    }
#endif
#ifdef SYS_dup3
    else if (number == SYS_dup3) {
        return (long)dup3((int)a1, (int)a2, (int)a3);
    }
#endif

    return real_syscall ? real_syscall(number, a1, a2, a3, a4, a5, a6) : -1;
}
