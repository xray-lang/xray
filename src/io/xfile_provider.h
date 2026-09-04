/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfile_provider.h - Host file-system provider for io.xr
 */

#ifndef XR_IO_XFILE_PROVIDER_H
#define XR_IO_XFILE_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/xdefs.h"
#include "../runtime/value/xvalue.h"
#include "../../include/xray_yieldable_abi.h"

struct XrVMRuntime;

typedef struct XrFileDirEntries {
    char **names;
    size_t count;
} XrFileDirEntries;

XR_FUNC int xr_file_open_read(const char *path);
XR_FUNC int xr_file_open_write(const char *path, bool append);
XR_FUNC int64_t xr_file_read_once(int64_t handle, void *buffer, size_t capacity);
XR_FUNC bool xr_file_close(int handle);
XR_FUNC XrCFuncResult xr_file_write_once(struct XrVMRuntime *isolate, int64_t handle,
                                         const char *data, size_t length, int64_t offset,
                                         XrValue *result);
XR_FUNC bool xr_file_dir_entries_read(const char *path, XrFileDirEntries *entries);
XR_FUNC void xr_file_dir_entries_release(XrFileDirEntries *entries);
XR_FUNC bool xr_file_temp_template(const char *root, char *output, size_t capacity);

#endif  // XR_IO_XFILE_PROVIDER_H
