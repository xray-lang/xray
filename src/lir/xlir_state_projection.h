/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_state_projection.h - Physical module catalog projections
 */
#ifndef XLIR_STATE_PROJECTION_H
#define XLIR_STATE_PROJECTION_H
#include "xlir_pod.h"
#include "shared/xr_pod_state.h"

static inline XrtModuleTypeKind lir_state_payload_kind(XrLirType type, XrLirType element) {
    if (type == XR_LIR_STRING) return XRT_MODULE_STRING;
    if (type == XR_LIR_ARRAY) {
        switch (element) {
        case XR_LIR_I64: return XRT_MODULE_ARRAY_I64;
        case XR_LIR_STRING: return XRT_MODULE_ARRAY_STRING;
        case XR_LIR_U8: return XRT_MODULE_ARRAY_U8;
        case XR_LIR_U64: return XRT_MODULE_ARRAY_U64;
        default: return XRT_MODULE_TYPE_INVALID;
        }
    }
    if (type == XR_LIR_UNIT || type == XR_LIR_I1 || type == XR_LIR_I64 || type == XR_LIR_F64)
        return xr_pod_module_kind(lir_pod_scalar(type));
    return XRT_MODULE_TYPE_INVALID;
}

enum { LIR_STATE_CONSTRUCTED_BASE = XRT_MODULE_FIXED_U8 - XRT_MODULE_UNIT };
#endif // XLIR_STATE_PROJECTION_H
