/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfile_provider.c - Host file-system provider for io.xr
 */

#include "xfile_provider.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "../base/xmalloc.h"
#include "../coro/xnetpoll.h"
#include "../coro/xyieldable.h"
#include "../coro/xworker.h"
#include "../runtime/xisolate_internal.h"

#ifdef XR_OS_WINDOWS
#include <direct.h>
#include <fcntl.h>
#if defined(_MSC_VER)
#include <corecrt_io.h>
#else
#include <io.h>
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#define XR_FILE_STDIN 0
#define XR_FILE_STDOUT 1
#define XR_FILE_STDERR 2

#ifdef XR_OS_WINDOWS
typedef int xr_file_ssize_t;
#define xr_file_os_open _open
#define xr_file_os_read _read
#define xr_file_os_write _write
#define xr_file_os_close _close
#define XR_FILE_OPEN_FLAGS (_O_BINARY | _O_NOINHERIT)
#define XR_FILE_CREATE_MODE (_S_IREAD | _S_IWRITE)
#else
typedef ssize_t xr_file_ssize_t;
#define xr_file_os_open open
#define xr_file_os_read read
#define xr_file_os_write write
#define xr_file_os_close close
#define XR_FILE_OPEN_FLAGS O_CLOEXEC
#define XR_FILE_CREATE_MODE 0644
#endif

static int file_open_checked(const char *path, int flags) {
    if (!path || path[0] == '\0')
        return -1;
    int descriptor = xr_file_os_open(path, flags, XR_FILE_CREATE_MODE);
    if (descriptor < 0 || descriptor > XR_FILE_STDERR)
        return descriptor;
#ifdef XR_OS_WINDOWS
    int lifted = _dup(descriptor);
    if (lifted >= 0 && lifted <= XR_FILE_STDERR) {
        _close(lifted);
        lifted = -1;
    }
#else
    int lifted = fcntl(descriptor, F_DUPFD_CLOEXEC, XR_FILE_STDERR + 1);
#endif
    xr_file_os_close(descriptor);
    return lifted;
}

int xr_file_open_read(const char *path) {
    return file_open_checked(path, O_RDONLY | XR_FILE_OPEN_FLAGS);
}

int xr_file_open_write(const char *path, bool append) {
    int flags = O_WRONLY | O_CREAT | XR_FILE_OPEN_FLAGS | (append ? O_APPEND : O_TRUNC);
    return file_open_checked(path, flags);
}

int64_t xr_file_read_once(int64_t handle, void *buffer, size_t capacity) {
    if (handle < 0 || (!buffer && capacity != 0))
        return -1;
    if (capacity == 0)
        return 0;
    if (handle == XR_FILE_STDIN) {
#ifdef XR_OS_WINDOWS
        if (_setmode(_fileno(stdin), _O_BINARY) == -1)
            return -1;
#endif
        size_t count = fread(buffer, 1, capacity, stdin);
        return count == 0 && ferror(stdin) ? -1 : (int64_t) count;
    }

    xr_file_ssize_t count;
    do {
        count = xr_file_os_read((int) handle, buffer, capacity);
    } while (count < 0 && errno == EINTR);
    return count < 0 ? -1 : (int64_t) count;
}

bool xr_file_close(int handle) {
    return xr_file_os_close(handle) == 0;
}

#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
typedef struct XrFileWriteState {
    XrPollDesc *poll_descriptor;
} XrFileWriteState;

static XrCFuncResult file_write_complete(XrVMRuntime *isolate, int status, XrValue resume_value,
                                         void *context, XrValue *result) {
    (void) status;
    (void) resume_value;
    XrFileWriteState *state = (XrFileWriteState *) context;
    XrUringXferKind kind;
    long count = xr_netpoll_uring_xfer_result(state->poll_descriptor, XR_POLL_WRITE, &kind);
    bool ok = kind == XR_URING_XFER_DATA && count >= 0;
    if (state->poll_descriptor)
        xr_netpoll_close(&((XrRuntime *) isolate->vm.scheduler)->netpoll, state->poll_descriptor);
    xr_free(state);
    *result = xr_int(ok ? (int64_t) count : -1);
    return XR_CFUNC_DONE;
}

static bool file_write_try_uring(XrVMRuntime *isolate, int descriptor, const char *data,
                                 size_t length, XrValue *result, XrCFuncResult *out) {
    XrRuntime *runtime = (XrRuntime *) isolate->vm.scheduler;
    if (!runtime || !xr_current_coro(isolate) || !xr_netpoll_uring_active(&runtime->netpoll))
        return false;
    XrPollDesc *poll_descriptor = xr_netpoll_open(&runtime->netpoll, descriptor);
    if (!poll_descriptor)
        return false;
    XrFileWriteState *state = (XrFileWriteState *) xr_calloc(1, sizeof(XrFileWriteState));
    if (!state) {
        xr_netpoll_close(&runtime->netpoll, poll_descriptor);
        return false;
    }
    state->poll_descriptor = poll_descriptor;
    XrUringReq request = {
        .kind = XR_URING_OP_FILE_WRITE,
        .buf = (void *) data,
        .len = (unsigned) (length > UINT_MAX ? UINT_MAX : length),
        .offset = (uint64_t) -1,
    };
    XrCFuncResult status;
    if (xr_yield_for_uring_io(isolate, poll_descriptor, XR_POLL_WRITE, &request,
                              file_write_complete, state, result, &status)) {
        *out = status;
        return true;
    }
    xr_netpoll_close(&runtime->netpoll, poll_descriptor);
    xr_free(state);
    return false;
}
#endif

XrCFuncResult xr_file_write_once(XrVMRuntime *isolate, int64_t handle, const char *data,
                                 size_t length, int64_t offset, XrValue *result) {
    (void) isolate;
    *result = xr_int(-1);
    if (handle < 0 || offset < 0 || (!data && length != 0))
        return XR_CFUNC_DONE;
    if ((uint64_t) offset >= (uint64_t) length) {
        *result = xr_int(0);
        return XR_CFUNC_DONE;
    }
    const char *from = data + offset;
    size_t remaining = length - (size_t) offset;

    if (handle == XR_FILE_STDOUT || handle == XR_FILE_STDERR) {
        FILE *stream = handle == XR_FILE_STDOUT ? stdout : stderr;
        size_t count = fwrite(from, 1, remaining, stream);
        *result = count == 0 && ferror(stream) ? xr_int(-1) : xr_int((int64_t) count);
        return XR_CFUNC_DONE;
    }
    if (handle == XR_FILE_STDIN)
        return XR_CFUNC_DONE;

#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
    XrCFuncResult outcome;
    if (file_write_try_uring(isolate, (int) handle, from, remaining, result, &outcome))
        return outcome;
#endif

    xr_file_ssize_t count;
    do {
        count = xr_file_os_write((int) handle, from, remaining);
    } while (count < 0 && errno == EINTR);
    *result = xr_int(count < 0 ? -1 : (int64_t) count);
    return XR_CFUNC_DONE;
}

static char *file_name_copy(const char *name) {
    size_t length = strlen(name);
    char *copy = (char *) xr_malloc(length + 1);
    if (copy)
        memcpy(copy, name, length + 1);
    return copy;
}

static bool file_dir_entries_push(XrFileDirEntries *entries, size_t *capacity, const char *name) {
    if (entries->count == *capacity) {
        size_t grown = *capacity == 0 ? 16 : *capacity * 2;
        char **names = (char **) xr_realloc(entries->names, grown * sizeof(char *));
        if (!names)
            return false;
        entries->names = names;
        *capacity = grown;
    }
    char *copy = file_name_copy(name);
    if (!copy)
        return false;
    entries->names[entries->count++] = copy;
    return true;
}

void xr_file_dir_entries_release(XrFileDirEntries *entries) {
    if (!entries)
        return;
    for (size_t i = 0; i < entries->count; i++)
        xr_free(entries->names[i]);
    xr_free(entries->names);
    entries->names = NULL;
    entries->count = 0;
}

bool xr_file_dir_entries_read(const char *path, XrFileDirEntries *entries) {
    if (!path || !entries)
        return false;
    *entries = (XrFileDirEntries) {0};
    size_t capacity = 0;
    bool ok = true;
#ifdef XR_OS_WINDOWS
    size_t length = strlen(path);
    char *pattern = (char *) xr_malloc(length + 4);
    if (!pattern)
        return false;
    memcpy(pattern, path, length);
    pattern[length++] = '\\';
    pattern[length++] = '*';
    pattern[length] = '\0';

    WIN32_FIND_DATAA entry;
    HANDLE directory = FindFirstFileA(pattern, &entry);
    xr_free(pattern);
    if (directory == INVALID_HANDLE_VALUE)
        return false;
    do {
        if (!file_dir_entries_push(entries, &capacity, entry.cFileName)) {
            ok = false;
            break;
        }
    } while (FindNextFileA(directory, &entry));
    FindClose(directory);
#else
    DIR *directory = opendir(path);
    if (!directory)
        return false;
    for (;;) {
        struct dirent *entry = readdir(directory);
        if (!entry)
            break;
        if (!file_dir_entries_push(entries, &capacity, entry->d_name)) {
            ok = false;
            break;
        }
    }
    closedir(directory);
#endif
    if (!ok)
        xr_file_dir_entries_release(entries);
    return ok;
}

bool xr_file_temp_template(const char *root, char *output, size_t capacity) {
    if (!root || root[0] == '\0')
        return false;
    int written = snprintf(output, capacity, "%s/xray_XXXXXX", root);
    return written > 0 && (size_t) written < capacity;
}
