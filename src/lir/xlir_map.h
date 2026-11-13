/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_map.h - Receiving-owned Map roles and exact physical signatures
 *
 * KEY CONCEPT:
 *   Intrinsic IDs belong to the receiving module. Queries preserve the closed
 *   String/I64 family and the actual mutation receiver type.
 */
#ifndef XLIR_MAP_H
#define XLIR_MAP_H
#include "xlir_builder.h"

/* These roles carry the exact closed String/I64 physical family. Intrinsic
 * IDs are local to the receiving module and are obtained by this query. */
typedef enum XrLirMapOperation {
    XR_LIR_MAP_OP_NEW = 1024,
    XR_LIR_MAP_OP_LEN,
    XR_LIR_MAP_OP_GET,
    XR_LIR_MAP_OP_CONTAINS,
    XR_LIR_MAP_OP_SET,
    XR_LIR_MAP_OP_REMOVE,
    XR_LIR_MAP_OP_CLEAR
} XrLirMapOperation;
XR_FUNC XrLirStatus xr_lir_map_operation(const XrLirModule *, XrLirMapOperation,
    XrLirType receiver, uint32_t *id);
XR_FUNC bool xr_lir_map_intrinsic_valid(const XrLirModule *, const XrLirIntrinsic *);
XR_FUNC XrLirStatus xr_lir_map_catalog_install(XrLirModule *, bool views, bool arrays,
    bool display, bool strings, bool ranges);
#endif // XLIR_MAP_H
