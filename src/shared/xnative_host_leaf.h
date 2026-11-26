/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_host_leaf.h - Admission for the numbered synchronous host ABI
 */
#ifndef XNATIVE_HOST_LEAF_H
#define XNATIVE_HOST_LEAF_H
#include "xnative_declaration.h"
#include <string.h>

typedef enum XrNativeHostShape {
    XR_NATIVE_HOST_NONE, XR_NATIVE_HOST_PATH_BOOL,
    XR_NATIVE_HOST_FILE_OPEN_WRITE, XR_NATIVE_HOST_FILE_CLOSE,
    XR_NATIVE_HOST_FILE_WRITE_BYTES, XR_NATIVE_HOST_FILE_WRITE_STRING,
    XR_NATIVE_HOST_FILE_OPEN_READ, XR_NATIVE_HOST_FILE_READ,
    XR_NATIVE_HOST_ENV_GET, XR_NATIVE_HOST_PATH_PAIR_BOOL, XR_NATIVE_HOST_FILE_STAT,
    XR_NATIVE_HOST_TEXT_RESULT, XR_NATIVE_HOST_DIRECTORY
} XrNativeHostShape;

static inline XrNativeHostShape xr_native_host_shape(uint32_t callable) {
    switch (callable) {
#define XR_NATIVE_BLOCKING_ENV_GET(id) case id: return XR_NATIVE_HOST_ENV_GET;
#define XR_NATIVE_BLOCKING_PATH_BOOL(id, implementation) case id: return XR_NATIVE_HOST_PATH_BOOL;
#define XR_NATIVE_BLOCKING_OWNED_PATH_BOOL(id, implementation) case id: return XR_NATIVE_HOST_PATH_BOOL;
#define XR_NATIVE_BLOCKING_PATH_PAIR_BOOL(id, implementation) case id: return XR_NATIVE_HOST_PATH_PAIR_BOOL;
#define XR_NATIVE_BLOCKING_FILE_OPEN_READ(id) case id: return XR_NATIVE_HOST_FILE_OPEN_READ;
#define XR_NATIVE_BLOCKING_FILE_STAT(id) case id: return XR_NATIVE_HOST_FILE_STAT;
#define XR_NATIVE_BLOCKING_FILE_OPEN_WRITE(id) case id: return XR_NATIVE_HOST_FILE_OPEN_WRITE;
#define XR_NATIVE_BLOCKING_FILE_CLOSE(id) case id: return XR_NATIVE_HOST_FILE_CLOSE;
#define XR_NATIVE_BLOCKING_FILE_READ(id) case id: return XR_NATIVE_HOST_FILE_READ;
#define XR_NATIVE_BLOCKING_FILE_WRITE(id, kind) case id: return XR_NATIVE_HOST_FILE_WRITE_##kind;
#define XR_NATIVE_BLOCKING_TEXT_RESULT(id) case id: return XR_NATIVE_HOST_TEXT_RESULT;
#define XR_NATIVE_BLOCKING_DIRECTORY(id) case id: return XR_NATIVE_HOST_DIRECTORY;
#include "xnative_host_leaves.inc.h"
#undef XR_NATIVE_BLOCKING_DIRECTORY
#undef XR_NATIVE_BLOCKING_TEXT_RESULT
#undef XR_NATIVE_BLOCKING_FILE_WRITE
#undef XR_NATIVE_BLOCKING_FILE_READ
#undef XR_NATIVE_BLOCKING_FILE_CLOSE
#undef XR_NATIVE_BLOCKING_FILE_OPEN_WRITE
#undef XR_NATIVE_BLOCKING_FILE_OPEN_READ
#undef XR_NATIVE_BLOCKING_FILE_STAT
#undef XR_NATIVE_BLOCKING_PATH_PAIR_BOOL
#undef XR_NATIVE_BLOCKING_OWNED_PATH_BOOL
#undef XR_NATIVE_BLOCKING_PATH_BOOL
#undef XR_NATIVE_BLOCKING_ENV_GET
    default: return XR_NATIVE_HOST_NONE;
    }
}
#define XR_NATIVE_COUNT_STAT_FIELD(name, term) + 1
enum { XR_NATIVE_FILE_STAT_FIELD_COUNT = 0 XR_NATIVE_FILE_STAT_FIELDS(XR_NATIVE_COUNT_STAT_FIELD) };
#undef XR_NATIVE_COUNT_STAT_FIELD

static inline XrNativeTypeTerm xr_native_file_stat_field(uint32_t index, const char *name, size_t length) {
    static const struct { const char *name; size_t length; XrNativeTypeTerm term; } fields[] = {
#define XR_NATIVE_STAT_FIELD(name, term) {name, sizeof(name) - 1, XR_NATIVE_TERM_##term},
        XR_NATIVE_FILE_STAT_FIELDS(XR_NATIVE_STAT_FIELD)
#undef XR_NATIVE_STAT_FIELD
    };
    if (!name || index >= XR_NATIVE_FILE_STAT_FIELD_COUNT || fields[index].length != length ||
        memcmp(fields[index].name, name, length)) return XR_NATIVE_TERM_UNADMITTED;
    return fields[index].term;
}
#undef XR_NATIVE_FILE_STAT_FIELDS

static inline uint32_t xr_native_host_path_result_class(uint32_t callable) {
    switch (xr_native_host_shape(callable)) {
    case XR_NATIVE_HOST_FILE_OPEN_READ: return XR_NATIVE_DECLARATION_FILE_STORAGE;
    case XR_NATIVE_HOST_FILE_STAT: return XR_NATIVE_DECLARATION_FILE_STAT;
    default: return 0;
    }
}
static inline uint32_t xr_native_blocking_path_count(uint32_t callable) {
    switch (xr_native_host_shape(callable)) {
    case XR_NATIVE_HOST_PATH_BOOL: return 1;
    case XR_NATIVE_HOST_PATH_PAIR_BOOL: return 2;
    default: return 0;
    }
}
#endif // XNATIVE_HOST_LEAF_H
