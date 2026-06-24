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

typedef bool (*XrIoCoreLineFn)(void *ctx, const char *data, size_t len);
typedef int (*XrIoCoreMkdirFn)(void *ctx, const char *path);
typedef bool (*XrIoCoreIsDirFn)(void *ctx, const char *path);
typedef size_t (*XrIoCoreReadFn)(void *ctx, void *buf, size_t cap);
typedef size_t (*XrIoCoreWriteFn)(void *ctx, const void *buf, size_t len);
typedef bool (*XrIoCoreErrorFn)(void *ctx);
typedef bool (*XrIoCoreSeekFn)(void *ctx);
typedef long (*XrIoCoreTellFn)(void *ctx);
typedef void *(*XrIoCoreAllocFn)(void *ctx, size_t size);
typedef void *(*XrIoCoreReallocFn)(void *ctx, void *ptr, size_t size);
typedef void (*XrIoCoreFreeFn)(void *ctx, void *ptr);

#define XR_IO_CORE_COPY_BUFFER_SIZE 65536u

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

static const char *const XR_IO_CORE_STAT_FIELD_NAMES[XR_IO_CORE_STAT_FIELD_COUNT] = {
    "size", "mode", "mtime", "atime", "ctime", "uid", "gid", "isFile", "isDir", "isSymlink",
};

static inline int64_t xr_io_core_stat_perm_mode(int64_t mode) {
    return mode & 0777;
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

static inline size_t xr_io_core_trim_line_end(const char *data, size_t start, size_t end) {
    while (end > start && data[end - 1] == '\r')
        end--;
    return end;
}

static inline bool xr_io_core_read_lines_each(const char *data, size_t len, XrIoCoreLineFn on_line,
                                              void *ctx) {
    if ((!data && len != 0) || !on_line)
        return false;

    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] != '\n')
            continue;

        size_t end = xr_io_core_trim_line_end(data, start, i);
        if (!on_line(ctx, data + start, end - start))
            return false;
        start = i + 1;
    }

    if (start < len) {
        size_t end = xr_io_core_trim_line_end(data, start, len);
        if (!on_line(ctx, data + start, end - start))
            return false;
    }

    return true;
}

static inline bool xr_io_core_prepare_sized_read(void *ctx, XrIoCoreSeekFn seek_end_fn,
                                                 XrIoCoreTellFn tell_fn,
                                                 XrIoCoreSeekFn seek_start_fn, long max_read_bytes,
                                                 size_t *out_size) {
    if (out_size)
        *out_size = 0;
    if (!seek_end_fn || !tell_fn || !seek_start_fn || max_read_bytes < 0)
        return false;
    if (!seek_end_fn(ctx))
        return false;
    long size = tell_fn(ctx);
    if (size < 0 || size > max_read_bytes)
        return false;
    if (!seek_start_fn(ctx))
        return false;
    if (out_size)
        *out_size = (size_t) size;
    return true;
}

static inline bool xr_io_core_read_into(void *ctx, XrIoCoreReadFn read_fn, XrIoCoreErrorFn error_fn,
                                        void *buf, size_t size, bool nul_terminate,
                                        size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!read_fn || (!buf && (size != 0 || nul_terminate)))
        return false;

    size_t n = 0;
    if (size > 0)
        n = read_fn(ctx, buf, size);
    if (n > size)
        n = size;
    bool failed = error_fn ? error_fn(ctx) : false;
    if (failed)
        return false;
    if (nul_terminate)
        ((char *) buf)[n] = '\0';
    if (out_len)
        *out_len = n;
    return true;
}

static inline char *xr_io_core_read_sized_stream_alloc(
    void *ctx, XrIoCoreSeekFn seek_end_fn, XrIoCoreTellFn tell_fn, XrIoCoreSeekFn seek_start_fn,
    XrIoCoreReadFn read_fn, XrIoCoreErrorFn error_fn, XrIoCoreAllocFn alloc_fn,
    XrIoCoreFreeFn free_fn, void *alloc_ctx, long max_read_bytes, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!alloc_fn || !free_fn)
        return NULL;

    size_t size = 0;
    if (!xr_io_core_prepare_sized_read(ctx, seek_end_fn, tell_fn, seek_start_fn, max_read_bytes,
                                       &size))
        return NULL;

    char *buf = (char *) alloc_fn(alloc_ctx, size + 1);
    if (!buf)
        return NULL;
    if (!xr_io_core_read_into(ctx, read_fn, error_fn, buf, size, true, out_len)) {
        free_fn(alloc_ctx, buf);
        return NULL;
    }
    return buf;
}

static inline char *
xr_io_core_read_all_stream_alloc(void *ctx, XrIoCoreReadFn read_fn, XrIoCoreErrorFn error_fn,
                                 XrIoCoreAllocFn alloc_fn, XrIoCoreReallocFn realloc_fn,
                                 XrIoCoreFreeFn free_fn, void *alloc_ctx, size_t initial_cap,
                                 long max_read_bytes, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!read_fn || !alloc_fn || !realloc_fn || !free_fn || initial_cap == 0 || max_read_bytes <= 0)
        return NULL;

    size_t max_cap = (size_t) max_read_bytes;
    size_t cap = initial_cap > max_cap ? max_cap : initial_cap;
    char *buf = (char *) alloc_fn(alloc_ctx, cap + 1);
    if (!buf)
        return NULL;

    size_t len = 0;
    for (;;) {
        size_t avail = cap - len;
        size_t n = read_fn(ctx, buf + len, avail);
        if (n > avail)
            n = avail;
        len += n;

        if (n < avail) {
            if (error_fn && error_fn(ctx)) {
                free_fn(alloc_ctx, buf);
                return NULL;
            }
            break;
        }

        if (cap >= max_cap) {
            free_fn(alloc_ctx, buf);
            return NULL;
        }

        size_t new_cap = cap > max_cap / 2 ? max_cap : cap * 2;
        if (new_cap <= cap) {
            free_fn(alloc_ctx, buf);
            return NULL;
        }
        char *next = (char *) realloc_fn(alloc_ctx, buf, new_cap + 1);
        if (!next) {
            free_fn(alloc_ctx, buf);
            return NULL;
        }
        buf = next;
        cap = new_cap;
    }

    buf[len] = '\0';
    if (out_len)
        *out_len = len;
    return buf;
}

static inline bool xr_io_core_copy_stream(void *ctx, XrIoCoreReadFn read_fn,
                                          XrIoCoreWriteFn write_fn, XrIoCoreErrorFn error_fn,
                                          void *buffer, size_t buffer_cap) {
    if (!read_fn || !write_fn || !buffer || buffer_cap == 0)
        return false;

    for (;;) {
        size_t n = read_fn(ctx, buffer, buffer_cap);
        if (n > 0 && write_fn(ctx, buffer, n) != n)
            return false;
        if (n < buffer_cap)
            return error_fn ? !error_fn(ctx) : true;
    }
}

static inline bool xr_io_core_write_all(void *ctx, XrIoCoreWriteFn write_fn,
                                        XrIoCoreErrorFn error_fn, const void *data, size_t len) {
    if (!write_fn || (!data && len != 0))
        return false;
    const char *bytes = (const char *) data;
    size_t off = 0;
    while (off < len) {
        size_t n = write_fn(ctx, bytes + off, len - off);
        if (n == 0 || n > len - off)
            return false;
        off += n;
    }
    return error_fn ? !error_fn(ctx) : true;
}

static inline bool xr_io_core_is_sep(char ch) {
    return ch == '/' || ch == '\\';
}

static inline size_t xr_io_core_cstr_len(const char *s) {
    size_t len = 0;
    if (!s)
        return 0;
    while (s[len])
        len++;
    return len;
}

static inline bool xr_io_core_is_dot_dir_entry(const char *name) {
    return name && name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

static inline bool xr_io_core_join_child_len(const char *parent, const char *name,
                                             size_t *out_len) {
    if (!parent || !name || !out_len)
        return false;

    size_t parent_len = xr_io_core_cstr_len(parent);
    size_t name_len = xr_io_core_cstr_len(name);
    if (parent_len == 0 || name_len == 0)
        return false;
    if (parent_len > SIZE_MAX - name_len - 1)
        return false;

    *out_len = parent_len + 1 + name_len;
    return true;
}

static inline bool xr_io_core_join_child_path(const char *parent, char sep, const char *name,
                                              char *out, size_t out_cap) {
    size_t len = 0;
    if (!out || !xr_io_core_join_child_len(parent, name, &len) || len >= out_cap)
        return false;

    size_t pos = 0;
    for (size_t i = 0; parent[i]; i++)
        out[pos++] = parent[i];
    out[pos++] = sep;
    for (size_t i = 0; name[i]; i++)
        out[pos++] = name[i];
    out[pos] = '\0';
    return true;
}

static inline bool xr_io_core_temp_template(const char *root, char sep, const char *stem, char *out,
                                            size_t out_cap) {
    if (!root || root[0] == '\0' || !stem || stem[0] == '\0' || !out)
        return false;

    size_t root_len = xr_io_core_cstr_len(root);
    size_t stem_len = xr_io_core_cstr_len(stem);
    if (root_len > SIZE_MAX - stem_len - 1)
        return false;

    size_t total = root_len + 1 + stem_len;
    if (total >= out_cap)
        return false;

    size_t pos = 0;
    for (size_t i = 0; i < root_len; i++)
        out[pos++] = root[i];
    out[pos++] = sep;
    for (size_t i = 0; i < stem_len; i++)
        out[pos++] = stem[i];
    out[pos] = '\0';
    return true;
}

static inline const char *xr_io_core_relative_path_from_base(const char *fullpath,
                                                             size_t base_len) {
    if (!fullpath)
        return NULL;
    const char *relpath = fullpath + base_len;
    if (xr_io_core_is_sep(*relpath))
        relpath++;
    return relpath;
}

static inline bool xr_io_core_is_alpha_ascii(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

static inline size_t xr_io_core_root_len(const char *path) {
    if (!path || path[0] == '\0')
        return 0;

    if (xr_io_core_is_alpha_ascii(path[0]) && path[1] == ':') {
        if (xr_io_core_is_sep(path[2]))
            return 3;
        return 2;
    }

    if (xr_io_core_is_sep(path[0])) {
#if defined(XR_OS_WINDOWS)
        if (!xr_io_core_is_sep(path[1]))
            return 1;
        size_t i = 2;
        while (path[i] && xr_io_core_is_sep(path[i]))
            i++;
        while (path[i] && !xr_io_core_is_sep(path[i]))
            i++;
        while (path[i] && xr_io_core_is_sep(path[i]))
            i++;
        while (path[i] && !xr_io_core_is_sep(path[i]))
            i++;
        return i > 2 ? i : 1;
#else
        return 1;
#endif
    }

    return 0;
}

static inline bool xr_io_core_ensure_dir(void *ctx, const char *path, XrIoCoreMkdirFn mkdir_fn,
                                         XrIoCoreIsDirFn is_dir_fn) {
    if (!path || path[0] == '\0' || !mkdir_fn || !is_dir_fn)
        return false;
    if (mkdir_fn(ctx, path) == 0)
        return true;
    return is_dir_fn(ctx, path);
}

static inline bool xr_io_core_mkdirp(char *path, XrIoCoreMkdirFn mkdir_fn,
                                     XrIoCoreIsDirFn is_dir_fn, void *ctx) {
    if (!path || path[0] == '\0' || !mkdir_fn || !is_dir_fn)
        return false;

    size_t len = 0;
    while (path[len])
        len++;

    size_t root_len = xr_io_core_root_len(path);
    while (len > root_len + 1 && xr_io_core_is_sep(path[len - 1]))
        path[--len] = '\0';

    if (len <= root_len)
        return is_dir_fn(ctx, path);

    size_t segment_start = root_len;
    while (path[segment_start] && xr_io_core_is_sep(path[segment_start]))
        segment_start++;

    for (size_t i = segment_start; path[i]; i++) {
        if (!xr_io_core_is_sep(path[i]))
            continue;
        char saved = path[i];
        path[i] = '\0';
        bool ok = (i > root_len) && xr_io_core_ensure_dir(ctx, path, mkdir_fn, is_dir_fn);
        path[i] = saved;
        if (!ok)
            return false;
        while (path[i + 1] && xr_io_core_is_sep(path[i + 1]))
            i++;
    }

    return xr_io_core_ensure_dir(ctx, path, mkdir_fn, is_dir_fn);
}

#endif /* XRAY_SHARED_XR_IO_CORE_H */
