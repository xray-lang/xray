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
static inline XrtModuleTypeKind xr_pod_module_kind(XrPodScalar scalar) {
    static const XrtModuleTypeKind kinds[] = {XRT_MODULE_UNIT, XRT_MODULE_BOOL,
        XRT_MODULE_I8, XRT_MODULE_I16, XRT_MODULE_I32, XRT_MODULE_I64,
        XRT_MODULE_U8, XRT_MODULE_U16, XRT_MODULE_U32, XRT_MODULE_U64, XRT_MODULE_F32, XRT_MODULE_F64};
    return (uint32_t)scalar < XR_POD_SCALAR_COUNT ? kinds[scalar] : XRT_MODULE_TYPE_INVALID;
}
#endif // XR_POD_STATE_H
