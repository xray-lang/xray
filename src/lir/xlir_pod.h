/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_pod.h - Physical scalar and module catalog projections
 */
#ifndef XLIR_POD_H
#define XLIR_POD_H
#include "xlir.h"
#include "shared/xr_pod_layout.h"

static inline XrPodScalar lir_pod_scalar(XrLirType type) {
    switch (type) {
    case XR_LIR_UNIT: return XR_POD_UNIT;
    case XR_LIR_I1: return XR_POD_BOOL;
    case XR_LIR_I8: return XR_POD_I8;
    case XR_LIR_I16: return XR_POD_I16;
    case XR_LIR_I32: return XR_POD_I32;
    case XR_LIR_I64: return XR_POD_I64;
    case XR_LIR_U8: return XR_POD_U8;
    case XR_LIR_U16: return XR_POD_U16;
    case XR_LIR_U32: return XR_POD_U32;
    case XR_LIR_U64: return XR_POD_U64;
    case XR_LIR_F32: return XR_POD_F32;
    case XR_LIR_F64: return XR_POD_F64;
    default: return XR_POD_SCALAR_COUNT;
    }
}

#endif // XLIR_POD_H
