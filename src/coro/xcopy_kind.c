/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcopy_kind.c - Lightweight value transfer classification.
 */

#include "xdeep_copy.h"

XrCopyKind xr_value_copy_kind(XrValue value) {
    if (XR_IS_NUM(value) || XR_IS_BOOL(value) || XR_IS_NULL(value))
        return XR_COPY_IMMEDIATE;
    if (!XR_IS_PTR(value))
        return XR_COPY_IMMEDIATE;

    uint8_t type = XR_HEAP_TYPE(value);
    switch (type) {
        case XR_TSTRING:
            return XR_COPY_SHARED;
        case XR_TCHANNEL:
        case XR_TATOMIC:
        case XR_TWORKQUEUE:
        case XR_TRESULTGROUP:
            return XR_COPY_SHARED_REF;
        case XR_TARRAY:
        case XR_TMAP:
        case XR_TFUNCTION:
            return XR_COPY_DEEP;
        default:
            return XR_COPY_DEEP;
    }
}
