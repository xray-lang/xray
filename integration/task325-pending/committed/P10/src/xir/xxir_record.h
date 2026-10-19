/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_record.h - Exact structural record metadata and owned value operations
 */
#ifndef XXIR_RECORD_H
#define XXIR_RECORD_H
#include "xxir_value_place.h"
#include "xxir_types.h"
XR_FUNC bool xr_xir_type_is_record(const XrXirTypes *types, XrXirType type);
XR_FUNC const XrXirTypeNode *xr_xir_record_signature(const XrXirTypes *types, XrXirType type);
/* Shared physical products retain distinct public Tuple and record identities. */
static inline const XrXirTypeNode *xir_product_signature(const XrXirTypes *types, XrXirType type) {
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node && (node->kind == XR_XIR_TYPE_TUPLE || node->kind == XR_XIR_TYPE_RECORD) ? node : NULL;
}
XR_FUNC XrXirValueStatus xr_xir_record_new(XrXirType type, const XrXirValue *fields,
    uint32_t count, XrXirValueAdmission *admission, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_record_get(const XrXirValue *value, uint32_t field,
    XrXirValueAdmission *admission, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_record_set(const XrXirValuePlace *place, uint32_t field,
    const XrXirValue *value, XrXirValueAdmission *admission);
#endif // XXIR_RECORD_H
