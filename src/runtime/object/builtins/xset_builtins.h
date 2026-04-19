/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xset_builtins.h - Set builtin methods
 */

#ifndef XSET_BUILTINS_H
#define XSET_BUILTINS_H

#include "xvalue.h"
#include "xdefs.h"

// Global Set constructor
XR_FUNC XrValue xr_builtin_set_construct(XrayIsolate *isolate, XrValue *args, int nargs);

// Static methods
XR_FUNC XrValue xr_builtin_set_from(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_builtin_set_range(XrayIsolate *isolate, XrValue *args, int nargs);

#endif // XSET_BUILTINS_H
