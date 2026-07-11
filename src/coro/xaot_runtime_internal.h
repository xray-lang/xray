/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_runtime_internal.h - Private AOT runtime owner layout
 */

#ifndef XAOT_RUNTIME_INTERNAL_H
#define XAOT_RUNTIME_INTERNAL_H

#include "xaot_coro.h"

#include "../base/xglobal_indices.h"

struct XrAotRuntime {
    uint32_t caps;
    struct XrRuntimeCore *core;
    struct XrRuntime *scheduler;
    XrValue builtins[XR_USER_GLOBALS_START];
};

#endif  // XAOT_RUNTIME_INTERNAL_H
