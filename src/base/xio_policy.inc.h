/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xio_policy.inc.h - Shared synchronous I/O admission and first-failure helpers
 */
#ifndef XIO_POLICY_INTERNAL_H
#define XIO_POLICY_INTERNAL_H
#include "xio_policy.h"
#include <string.h>
#include <errno.h>

typedef struct XrIoContext {
    const XrOsIoPolicy *policy;
    XrOsIoStatus status;
} XrIoContext;
static inline bool io_policy_valid(const XrOsIoPolicy *policy) {
    return policy && policy->context && policy->alloc && policy->free && policy->work;
}
static inline bool io_status(XrIoContext *io, XrOsIoStatus status) {
    if (io->status == XR_OS_IO_OK) io->status = status;
    return io->status == XR_OS_IO_OK;
}
static inline bool io_work(XrIoContext *io, uint64_t units) {
    return io->status == XR_OS_IO_OK && io_status(io, io->policy->work(io->policy->context, units));
}
static inline void *io_alloc(XrIoContext *io, size_t bytes) {
    void *memory = NULL;
    if (io->status == XR_OS_IO_OK) io_status(io, io->policy->alloc(io->policy->context, bytes, &memory));
    return memory;
}
static inline void io_free(XrIoContext *io, void *memory) {
    if (memory) io->policy->free(io->policy->context, memory);
}
static inline bool io_length(XrIoContext *io, const char *text, size_t *length) {
    size_t n = 0;
    if (!text) return io_status(io, XR_OS_IO_BAD_ARGUMENT);
    while (io_work(io, 1)) {
        if (!text[n]) { *length = n; return true; }
        if (n == SIZE_MAX - 1) return io_status(io, XR_OS_IO_BUDGET);
        ++n;
    }
    return false;
}
static inline bool io_copy(XrIoContext *io, void *target, const void *source, size_t bytes) {
    if (!io_work(io, bytes)) return false;
    if (bytes) memcpy(target, source, bytes);
    return true;
}
static inline bool io_clear(XrIoContext *io, void *target, size_t bytes) {
    if (!io_work(io, bytes)) return false;
    if (bytes) memset(target, 0, bytes);
    return true;
}
static inline XrOsIoStatus io_errno_status(int error) {
    if (error == ENOMEM) return XR_OS_IO_OUT_OF_MEMORY;
    if (error == ENAMETOOLONG || error == ERANGE) return XR_OS_IO_BUDGET;
    if (error == ENOENT || error == ENOTDIR) return XR_OS_IO_NOT_FOUND;
    if (error == EEXIST) return XR_OS_IO_EXISTS;
    if (error == EINVAL) return XR_OS_IO_BAD_ARGUMENT;
    return XR_OS_IO_IO;
}
#endif /* XIO_POLICY_INTERNAL_H */
