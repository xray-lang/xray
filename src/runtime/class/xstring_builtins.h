/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstring_builtins.h - String builtin methods
 */

#ifndef XSTRING_BUILTINS_H
#define XSTRING_BUILTINS_H

#include "../value/xvalue.h"
#include "../base/xdefs.h"


// Global String constructor
XR_FUNC XrValue xr_builtin_string_construct(XrayIsolate *isolate, XrValue *args, int nargs);

#endif // XSTRING_BUILTINS_H
