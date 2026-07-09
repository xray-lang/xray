/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * datetime.h - pure-Xray datetime module loader
 */

#ifndef XR_STDLIB_DATETIME_H
#define XR_STDLIB_DATETIME_H

#include "../../src/base/xdefs.h"

struct XrVMRuntime;
struct XrModule;

XR_FUNC struct XrModule *xr_load_module_datetime(struct XrVMRuntime *isolate);

#endif  // XR_STDLIB_DATETIME_H
