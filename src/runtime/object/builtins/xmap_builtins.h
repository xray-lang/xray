/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmap_builtins.h - Map builtin methods
 */

#ifndef XMAP_BUILTINS_H
#define XMAP_BUILTINS_H

#include "xvalue.h"
#include "xdefs.h"

// Global Map constructor (self = class value, ignored)
XR_FUNC XrValue xr_builtin_map_construct(XrayIsolate *isolate, XrValue self, XrValue *args,
                                         int argc);

// Static methods (self = class value, ignored)
XR_FUNC XrValue xr_builtin_map_from(XrayIsolate *isolate, XrValue self, XrValue *args, int argc);

#endif  // XMAP_BUILTINS_H
