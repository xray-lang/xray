/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_internal.h - Complete immutable bytecode admission without execution
 */
#ifndef XRBC_INTERNAL_H
#define XRBC_INTERNAL_H

#include "xrbc.h"

enum { XR_RBC_ARRAY_FAMILY_COUNT = 4 };
static inline XrRbcRegisterKind xr_rbc_array_kind_at(uint32_t family) {
    static const XrRbcRegisterKind kinds[] = {XR_RBC_ARRAY_I64, XR_RBC_ARRAY_STRING, XR_RBC_ARRAY_U8, XR_RBC_ARRAY_U64};
    return family < XR_RBC_ARRAY_FAMILY_COUNT ? kinds[family] : XR_RBC_BITS;
}
static inline XrRbcRegisterKind xr_rbc_array_cell_at(uint32_t family) {
    static const XrRbcRegisterKind kinds[] = {
        XR_RBC_ARRAY_I64_CELL, XR_RBC_ARRAY_STRING_CELL, XR_RBC_ARRAY_U8_CELL, XR_RBC_ARRAY_U64_CELL};
    return family < XR_RBC_ARRAY_FAMILY_COUNT ? kinds[family] : XR_RBC_BITS;
}
static inline uint32_t xr_rbc_array_family(XrRbcRegisterKind kind) {
    for (uint32_t i = 0; i < XR_RBC_ARRAY_FAMILY_COUNT; ++i)
        if (kind == xr_rbc_array_kind_at(i)) return i;
    return XR_RBC_ARRAY_FAMILY_COUNT;
}
static inline bool xr_rbc_state_array(uint32_t kind) {
    return kind >= XRT_MODULE_ARRAY_I64 && kind <= XRT_MODULE_ARRAY_U64;
}

/* Both compiler publication and preparation use this complete receiver.
 * It does not allocate an execution or retain the borrowed module/locations. */
XR_FUNC XrRbcStatus xr_rbc_module_admit(const XrRbcModule *, const XrtLocationOwner *);

#endif // XRBC_INTERNAL_H
