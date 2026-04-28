/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_yield.h - Mini Yieldable C function test library header
 */

#ifndef XR_STDLIB_TEST_YIELD_H
#define XR_STDLIB_TEST_YIELD_H

#include "xray.h"
#include "../../src/base/xdefs.h"

// Load test_yield module
XR_FUNC XrModule *xr_load_module_test_yield(XrayIsolate *isolate);

#endif
