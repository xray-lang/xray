/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstrcompat.h - libc string compatibility shims
 *
 * KEY CONCEPT:
 *   POSIX exposes case-insensitive bounded compares via
 *   <strings.h> as strncasecmp / strcasecmp. MSVC names the same
 *   functions _strnicmp / _stricmp in <string.h>. Wrap once here
 *   so callers do not embed #ifdef around every comparison.
 *
 *   This is libc compatibility, not OS abstraction; lives in
 *   src/base/ rather than src/os/.
 */

#ifndef XSTRCOMPAT_H
#define XSTRCOMPAT_H

#include "xplatform.h"

#ifdef XR_OS_WINDOWS
#include <string.h>
#define xr_strncasecmp(a, b, n) _strnicmp((a), (b), (n))
#define xr_strcasecmp(a, b) _stricmp((a), (b))
#else
#include <strings.h>
#define xr_strncasecmp(a, b, n) strncasecmp((a), (b), (n))
#define xr_strcasecmp(a, b) strcasecmp((a), (b))
#endif

#endif  // XSTRCOMPAT_H
