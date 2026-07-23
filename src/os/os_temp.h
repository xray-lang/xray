/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_temp.h - Secure private temporary directory creation
 */

#ifndef XR_OS_TEMP_H
#define XR_OS_TEMP_H

#include "../base/xdefs.h"

#include <stddef.h>

/* Creates an atomically reserved, user-private directory below the platform
 * temp root. `prefix` may contain ASCII letters, digits, '-' and '_' only. */
XR_FUNC int xr_temp_dir_create(const char *prefix, char *out, size_t out_size);

#endif /* XR_OS_TEMP_H */
