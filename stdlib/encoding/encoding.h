/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * encoding.h - pure-Xray encoding module loader
 */

#ifndef XR_STDLIB_ENCODING_H
#define XR_STDLIB_ENCODING_H

#include "../../src/base/xdefs.h"

struct XrVMRuntime;
struct XrModule;

XR_FUNC struct XrModule *xr_native_module_create_encoding(struct XrVMRuntime *isolate);

#endif  // XR_STDLIB_ENCODING_H
