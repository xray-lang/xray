/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_native_output.h - Unbuffered standalone output with captured failures
 *
 * KEY CONCEPT:
 *   A result captures the error before another operation can overwrite it.
 */
#ifndef OS_NATIVE_OUTPUT_H
#define OS_NATIVE_OUTPUT_H
#include "base/xdefs.h"

typedef enum XrtIoDomain { XRT_IO_NONE, XRT_IO_ERRNO, XRT_IO_WINDOWS } XrtIoDomain;
typedef struct XrtIoResult {
    bool failed, broken_pipe;
    XrtIoDomain domain;
    uint32_t code;
} XrtIoResult;

XR_FUNC XrtIoResult xrt_io_flush_stdout(void);
XR_FUNC XrtIoResult xrt_io_write(unsigned stream, const char *bytes, size_t length);
#endif // OS_NATIVE_OUTPUT_H
