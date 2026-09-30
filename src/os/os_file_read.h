/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_file_read.h - Bounded regular-file reads under an explicit physical root
 *
 * KEY CONCEPT:
 *   Check physical ancestry and read through the same open file handle.
 */
#ifndef XR_OS_FILE_READ_H
#define XR_OS_FILE_READ_H
#include "../base/xdefs.h"
#include <stddef.h>

typedef enum XrFileReadStatus {
    XR_FILE_READ_OK,
    XR_FILE_READ_MISSING,
    XR_FILE_READ_FORBIDDEN,
    XR_FILE_READ_LIMIT,
    XR_FILE_READ_IO,
    XR_FILE_READ_OUT_OF_MEMORY
} XrFileReadStatus;

typedef struct XrFileBytes {
    char *data;
    size_t size;
} XrFileBytes;

/* Root is an absolute UTF-8 directory locator; logical_path uses '/' segments.
 * The caller keeps the input tree stable during the call. This is not a
 * transaction against arbitrary concurrent filesystem mutation. Success owns
 * a NUL-terminated xr_malloc buffer; size excludes the terminator. On error,
 * output is empty. Only a missing target under an opened root is MISSING. */
XR_FUNC XrFileReadStatus xr_file_read_under_root(const char *root, const char *logical_path,
    size_t limit, XrFileBytes *output);
#endif // XR_OS_FILE_READ_H
