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

typedef enum XrPipeIoStatus {
    XR_PIPE_IO_OK = 0,
    XR_PIPE_IO_WOULD_BLOCK = 1,
    XR_PIPE_IO_ERROR = 2,
} XrPipeIoStatus;

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

// Non-blocking one-shot variants for coroutine-friendly wrappers. They preserve
// the pipe endpoint's original blocking mode before returning.
XR_FUNC XrPipeIoStatus xr_pipe_try_read(XrPipeHandle handle, void *buf, size_t len, int64_t *out_n);
XR_FUNC XrPipeIoStatus xr_pipe_try_write(XrPipeHandle handle, const void *buf, size_t len,
                                         int64_t *out_n);

#ifdef __cplusplus
}
#endif

#endif  // XR_OS_OS_PIPE_H
