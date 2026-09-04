/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_io_core.h - Runtime-neutral IO stdlib core helpers.
 */

#ifndef XRAY_SHARED_XR_IO_CORE_H
#define XRAY_SHARED_XR_IO_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>

/* Preserve native-resource ownership across the integer handle ABI used by
 * the VM and generated C.  Clang's stream checker otherwise loses the FILE *
 * when it is encoded into an Xray value and reports a false leak at the
 * transfer boundary.  Other compilers see no attribute and the runtime ABI is
 * unchanged. */
#if defined(__clang__)
#if __has_attribute(acquire_handle)
#define XR_IO_CORE_ACQUIRE_HANDLE(tag) __attribute__((acquire_handle(tag)))
#define XR_IO_CORE_RELEASE_HANDLE(tag) __attribute__((release_handle(tag)))
#define XR_IO_CORE_USE_HANDLE(tag) __attribute__((use_handle(tag)))
#endif
#endif
#ifndef XR_IO_CORE_ACQUIRE_HANDLE
#define XR_IO_CORE_ACQUIRE_HANDLE(tag)
#define XR_IO_CORE_RELEASE_HANDLE(tag)
#define XR_IO_CORE_USE_HANDLE(tag)
#endif

typedef bool (*XrIoCoreDirEntryFn)(void *ctx, const char *name);

typedef enum XrIoCoreStatField {
    XR_IO_CORE_STAT_SIZE = 0,
    XR_IO_CORE_STAT_MODE,
    XR_IO_CORE_STAT_MTIME,
    XR_IO_CORE_STAT_ATIME,
    XR_IO_CORE_STAT_CTIME,
    XR_IO_CORE_STAT_UID,
    XR_IO_CORE_STAT_GID,
    XR_IO_CORE_STAT_IS_FILE,
    XR_IO_CORE_STAT_IS_DIR,
    XR_IO_CORE_STAT_IS_SYMLINK,
    XR_IO_CORE_STAT_FIELD_COUNT
} XrIoCoreStatField;

typedef struct XrIoCoreStatFields {
    int64_t size;
    int64_t mode;
    int64_t mtime;
    int64_t atime;
    int64_t ctime;
    int64_t uid;
    int64_t gid;
    bool is_file;
    bool is_dir;
    bool is_symlink;
} XrIoCoreStatFields;

typedef struct XrIoCorePathView {
    const char *data;
    size_t len;
} XrIoCorePathView;

static const char *const XR_IO_CORE_STAT_FIELD_NAMES[XR_IO_CORE_STAT_FIELD_COUNT] = {
    "size", "mode", "mtime", "atime", "ctime", "uid", "gid", "isFile", "isDir", "isSymlink",
};

static inline int64_t xr_io_core_stat_perm_mode(int64_t mode) {
    return mode & 0777;
}

static inline bool xr_io_core_chmod_mode(int64_t mode_value, int *out_mode) {
    if (out_mode)
        *out_mode = 0;
    if (!out_mode || mode_value < 0 || mode_value > INT_MAX)
        return false;
    *out_mode = (int) mode_value;
    return true;
}

static inline XrIoCoreStatFields xr_io_core_stat_fields(int64_t size, int64_t mode, int64_t mtime,
                                                        int64_t atime, int64_t ctime, int64_t uid,
                                                        int64_t gid, bool is_file, bool is_dir,
                                                        bool is_symlink) {
    XrIoCoreStatFields fields;
    fields.size = size;
    fields.mode = xr_io_core_stat_perm_mode(mode);
    fields.mtime = mtime;
    fields.atime = atime;
    fields.ctime = ctime;
    fields.uid = uid;
    fields.gid = gid;
    fields.is_file = is_file;
    fields.is_dir = is_dir;
    fields.is_symlink = is_symlink;
    return fields;
}

static inline size_t xr_io_core_cstr_len(const char *s) {
    size_t len = 0;
    if (!s)
        return 0;
    while (s[len])
        len++;
    return len;
}

static inline bool xr_io_core_path_result_view(const char *data, size_t len,
                                               XrIoCorePathView *out) {
    if (out) {
        out->data = NULL;
        out->len = 0;
    }
    if (!data || !out)
        return false;

    if (len >= 4 && data[0] == '\\' && data[1] == '\\' && data[2] == '?' && data[3] == '\\') {
        data += 4;
        len -= 4;
    }

    out->data = data;
    out->len = len;
    return true;
}

static inline bool xr_io_core_path_result_cstr_view(const char *data, XrIoCorePathView *out) {
    return xr_io_core_path_result_view(data, xr_io_core_cstr_len(data), out);
}

#endif /* XRAY_SHARED_XR_IO_CORE_H */
