/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * csv.h - CSV standard library module loader
 *
 * KEY CONCEPT:
 *   CSV's public API is implemented in stdlib/csv/csv.xr. This header only
 *   exposes the native loader that anchors `import csv` in the stdlib registry.
 */

#ifndef XR_STDLIB_CSV_H
#define XR_STDLIB_CSV_H

#include "../../src/base/xdefs.h"

struct XrVMRuntime;
struct XrModule;

XR_FUNC struct XrModule *xr_native_module_create_csv(struct XrVMRuntime *isolate);

#endif  // XR_STDLIB_CSV_H
