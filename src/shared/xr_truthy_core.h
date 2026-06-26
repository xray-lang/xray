/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_truthy_core.h - Runtime-neutral truthiness rules.
 */

#ifndef XR_TRUTHY_CORE_H
#define XR_TRUTHY_CORE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum XrTruthyCoreKind {
    XR_TRUTHY_CORE_NULL = 0,
    XR_TRUTHY_CORE_BOOL,
    XR_TRUTHY_CORE_INT,
    XR_TRUTHY_CORE_FLOAT,
    XR_TRUTHY_CORE_SIZED,
    XR_TRUTHY_CORE_OBJECT,
} XrTruthyCoreKind;

static inline bool xr_truthy_core_eval(XrTruthyCoreKind kind, int64_t i, double f, int64_t size) {
    switch (kind) {
        case XR_TRUTHY_CORE_NULL:
            return false;
        case XR_TRUTHY_CORE_BOOL:
        case XR_TRUTHY_CORE_INT:
            return i != 0;
        case XR_TRUTHY_CORE_FLOAT:
            return f != 0.0;
        case XR_TRUTHY_CORE_SIZED:
            return size != 0;
        case XR_TRUTHY_CORE_OBJECT:
        default:
            return true;
    }
}

#endif /* XR_TRUTHY_CORE_H */
