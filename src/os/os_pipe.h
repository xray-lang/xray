/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_pipe.h - Cross-platform anonymous pipe handles.
 */

#ifndef XR_OS_OS_PIPE_H
#define XR_OS_OS_PIPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/xdefs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef intptr_t XrPipeHandle;

#define XR_PIPE_INVALID ((XrPipeHandle) - 1)

typedef struct XrPipe {
    XrPipeHandle read;
    XrPipeHandle write;
} XrPipe;

typedef struct XrPipeOptions {
    bool read_inheritable;
    bool write_inheritable;
} XrPipeOptions;

// Create an anonymous byte pipe. When `options` is NULL, both ends are
// non-inheritable across child process exec/spawn. Process redirection
// can opt into inheriting exactly the end passed to the child.
XR_FUNC int xr_pipe_create(XrPipe *out, const XrPipeOptions *options);

// Close a pipe endpoint. Closing XR_PIPE_INVALID is a no-op success.
XR_FUNC int xr_pipe_close(XrPipeHandle handle);

// Read/write one chunk. Return byte count on success, 0 for EOF on read,
// and -1 on error. These functions retry EINTR where the platform has it
// but intentionally do not promise full-buffer writes.
XR_FUNC int64_t xr_pipe_read(XrPipeHandle handle, void *buf, size_t len);
XR_FUNC int64_t xr_pipe_write(XrPipeHandle handle, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif  // XR_OS_OS_PIPE_H
