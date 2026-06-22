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

// Global Set constructor (self = class value, ignored)
XR_FUNC XrValue xr_builtin_set_construct(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                         int argc);

// Static methods (self = class value, ignored)
XR_FUNC XrValue xr_builtin_set_from(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc);
XR_FUNC XrValue xr_builtin_set_range(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc);

#endif  // XSET_BUILTINS_H
