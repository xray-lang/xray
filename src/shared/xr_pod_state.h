/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_pod_state.h - Exact scalar identities in the runtime type catalog
 */
#ifndef XR_POD_STATE_H
#define XR_POD_STATE_H
#include "xray_native_pod.h"
#include "xray_native_state.h"
static inline bool xr_pod_value_kind(XrPodScalar scalar, XrtValueKind *output) {
    static const XrtValueKind kinds[] = {XRT_VALUE_UNIT, XRT_VALUE_BOOL,
        XRT_VALUE_I8, XRT_VALUE_I16, XRT_VALUE_I32, XRT_VALUE_I64,
        XRT_VALUE_U8, XRT_VALUE_U16, XRT_VALUE_U32, XRT_VALUE_U64, XRT_VALUE_F32, XRT_VALUE_F64};
    if (!output || (uint32_t)scalar >= XR_POD_SCALAR_COUNT) return false;
    *output = kinds[scalar]; return true;
}
#endif // XR_POD_STATE_H
