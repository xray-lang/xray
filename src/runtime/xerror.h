/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xerror.h - Error code type, ANSI color utilities, forward declarations
 *
 * KEY CONCEPT:
 *   All error codes are plain #define constants in xerror_codes.h.
 *   XrErrorCode is typedef'd to int (no enum).
 *   XrError is a legacy GC object used by exception conversion.
 */

#ifndef XERROR_H
#define XERROR_H

#include <stdbool.h>
#include <stdio.h>  // FILE, stderr
#include "../os/os_fd.h"

/* ========== ANSI Color Codes ========== */

#define XR_COLOR_SUPPORTED() (xr_isatty(xr_stderr_fd()))
#define XR_COLOR_RESET "\033[0m"
#define XR_COLOR_RED "\033[31m"
#define XR_COLOR_GREEN "\033[32m"
#define XR_COLOR_YELLOW "\033[33m"
#define XR_COLOR_BLUE "\033[34m"
#define XR_COLOR_MAGENTA "\033[35m"
#define XR_COLOR_CYAN "\033[36m"
#define XR_COLOR_WHITE "\033[37m"
#define XR_COLOR_GRAY "\033[90m"

#define XR_COLOR_BOLD "\033[1m"
#define XR_COLOR_BOLD_RED "\033[1;31m"
#define XR_COLOR_BOLD_YELLOW "\033[1;33m"
#define XR_COLOR_BOLD_CYAN "\033[1;36m"

/* ========== Error Code Type ========== */

typedef int XrErrorCode;

#include "xerror_codes.h"

/* ========== Forward Declarations ========== */

#include "../base/xforward_decl.h"
typedef struct XrError XrError;

#endif  // XERROR_H
