/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_param_mode.h - Shared function parameter contract mode.
 */

#ifndef XR_PARAM_MODE_H
#define XR_PARAM_MODE_H

#include <stdbool.h>

typedef enum XrParamMode {
    XR_PARAM_VALUE = 0, /* ordinary value parameter */
    XR_PARAM_IN = 1,    /* readonly borrow parameter */
    XR_PARAM_REF = 2,   /* read/write borrow parameter */
    XR_PARAM_OUT = 3,   /* definite-write output parameter */
} XrParamMode;

static inline bool xr_param_mode_is_valid(XrParamMode mode) {
    return mode == XR_PARAM_VALUE || mode == XR_PARAM_IN || mode == XR_PARAM_REF ||
           mode == XR_PARAM_OUT;
}

static inline bool xr_param_mode_is_borrow(XrParamMode mode) {
    return mode == XR_PARAM_IN || mode == XR_PARAM_REF;
}

static inline const char *xr_param_mode_label(XrParamMode mode) {
    switch (mode) {
        case XR_PARAM_VALUE:
            return "value";
        case XR_PARAM_IN:
            return "in";
        case XR_PARAM_REF:
            return "ref";
        case XR_PARAM_OUT:
            return "out";
        default:
            return "invalid";
    }
}

#endif /* XR_PARAM_MODE_H */
