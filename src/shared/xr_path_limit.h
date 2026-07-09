/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_path_limit.h - Shared path buffer size policy.
 */

#ifndef XR_PATH_LIMIT_H
#define XR_PATH_LIMIT_H

#include "../base/xplatform.h"

#include <limits.h>

#ifndef XR_PATH_LIMIT_MAX_PATH
#if defined(XR_OS_WINDOWS)
#define XR_PATH_LIMIT_MAX_PATH 4096
#elif defined(PATH_MAX)
#define XR_PATH_LIMIT_MAX_PATH PATH_MAX
#else
#define XR_PATH_LIMIT_MAX_PATH 4096
#endif
#endif

#endif  // XR_PATH_LIMIT_H
