/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_win_compat.h - Suppress Windows CRT error dialogs in unit tests
 *
 * Include this and call xr_test_suppress_dialogs() at the top of main().
 * On non-Windows platforms this is a no-op.
 */

#ifndef XR_TEST_WIN_COMPAT_H
#define XR_TEST_WIN_COMPAT_H

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <Windows.h>
#include <crtdbg.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <malloc.h>
#include <process.h>
#include <sys/stat.h>

#ifndef _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#define _SSIZE_T_DEFINED
#endif
#else
#include <unistd.h>
#endif

static inline void xr_test_suppress_dialogs(void) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}

static inline int xr_test_getpid(void) {
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}

static inline int xr_test_unlink(const char *path) {
#ifdef _WIN32
    return _unlink(path);
#else
    return unlink(path);
#endif
}

static inline int xr_test_rmdir(const char *path) {
#ifdef _WIN32
    return _rmdir(path);
#else
    return rmdir(path);
#endif
}

static inline ssize_t xr_test_write(int fd, const void *buf, size_t count) {
#ifdef _WIN32
    return (ssize_t) _write(fd, buf, (unsigned int) count);
#else
    return write(fd, buf, count);
#endif
}

static inline int xr_test_close(int fd) {
#ifdef _WIN32
    return _close(fd);
#else
    return close(fd);
#endif
}

static inline FILE *xr_test_fdopen(int fd, const char *mode) {
#ifdef _WIN32
    return _fdopen(fd, mode);
#else
    return fdopen(fd, mode);
#endif
}

static inline int xr_test_setenv(const char *name, const char *value, int overwrite) {
#ifdef _WIN32
    if (!overwrite) {
        size_t existing_len = 0;
        getenv_s(&existing_len, NULL, 0, name);
        if (existing_len > 0)
            return 0;
    }
    return _putenv_s(name, value ? value : "");
#else
    return setenv(name, value, overwrite);
#endif
}

static inline int xr_test_unsetenv(const char *name) {
#ifdef _WIN32
    return _putenv_s(name, "");
#else
    return unsetenv(name);
#endif
}

static inline int xr_test_mkstemps(char *path, int suffix_len) {
#ifdef _WIN32
    if (!path || suffix_len < 0)
        return -1;
    size_t len = strlen(path);
    if (len < (size_t) suffix_len + 6)
        return -1;
    char *xs = path + len - (size_t) suffix_len - 6;
    if (strncmp(xs, "XXXXXX", 6) != 0)
        return -1;
    for (unsigned int i = 0; i < 4096; i++) {
        char repl[7];
        unsigned long token =
            ((unsigned long) GetCurrentProcessId() + (unsigned long) GetTickCount() + i) &
            0xffffffUL;
        snprintf(repl, sizeof(repl), "%06lx", token);
        memcpy(xs, repl, 6);
        int fd = _open(path, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
        if (fd >= 0)
            return fd;
        if (errno != EEXIST)
            break;
    }
    memcpy(xs, "XXXXXX", 6);
    return -1;
#else
    return mkstemps(path, suffix_len);
#endif
}

static inline int xr_test_mkstemp(char *path) {
    return xr_test_mkstemps(path, 0);
}

static inline char *xr_test_mkdtemp(char *path) {
#ifdef _WIN32
    if (!path)
        return NULL;
    size_t len = strlen(path);
    if (len < 6)
        return NULL;
    char *xs = path + len - 6;
    if (strncmp(xs, "XXXXXX", 6) != 0)
        return NULL;
    for (unsigned int i = 0; i < 4096; i++) {
        char repl[7];
        unsigned long token =
            ((unsigned long) GetCurrentProcessId() + (unsigned long) GetTickCount() + i) &
            0xffffffUL;
        snprintf(repl, sizeof(repl), "%06lx", token);
        memcpy(xs, repl, 6);
        if (_mkdir(path) == 0)
            return path;
        if (errno != EEXIST)
            break;
    }
    memcpy(xs, "XXXXXX", 6);
    return NULL;
#else
    return mkdtemp(path);
#endif
}

static inline void *xr_test_alloc_aligned(size_t size, size_t align) {
#ifdef _WIN32
    return _aligned_malloc(size, align);
#else
    void *ptr = NULL;
    return posix_memalign(&ptr, align, size) == 0 ? ptr : NULL;
#endif
}

static inline void xr_test_free_aligned(void *ptr) {
#ifdef _WIN32
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

#endif  // XR_TEST_WIN_COMPAT_H
