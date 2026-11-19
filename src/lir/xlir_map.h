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
    XR_LIR_MAP_OP_INDEX,
    XR_LIR_MAP_OP_CONTAINS,
    XR_LIR_MAP_OP_KEYS,
    XR_LIR_MAP_OP_VALUES,
    XR_LIR_MAP_OP_DISPLAY,
    XR_LIR_MAP_OP_SET,
    XR_LIR_MAP_OP_REMOVE,
    XR_LIR_MAP_OP_CLEAR
} XrLirMapOperation;
/* These operations take one borrowed key in argument 1. */
static inline bool xr_lir_map_borrows_key(uint32_t operation) {
    return operation == XR_LIR_MAP_OP_GET || operation == XR_LIR_MAP_OP_INDEX ||
        operation == XR_LIR_MAP_OP_CONTAINS || operation == XR_LIR_MAP_OP_SET || operation == XR_LIR_MAP_OP_REMOVE;
}
static inline XrLirType xr_lir_map_snapshot_element(uint32_t operation) {
    if (operation == XR_LIR_MAP_OP_KEYS) return XR_LIR_STRING;
    if (operation == XR_LIR_MAP_OP_VALUES) return XR_LIR_I64;
    return XR_LIR_TYPE_INVALID;
}
typedef struct XrLirMapLiteralOrigin { uint32_t kind,line,column; } XrLirMapLiteralOrigin;
typedef struct XrLirMapLiteral {
    const char *path;
    uint32_t path_length;
    const XrLirMapLiteralOrigin *items;
    uint32_t count;
} XrLirMapLiteral;
/* Ordinals start at one. All spans are copied into the module; views return
 * separately owned spans, independent of the descriptor vector's growth. */
XR_FUNC XrLirStatus xr_lir_map_literal_add(XrLirModule *,const XrLirMapLiteral *,uint32_t *);
XR_FUNC uint32_t xr_lir_map_literal_count(const XrLirModule *);
XR_FUNC XrLirStatus xr_lir_map_literal_view(const XrLirModule *,uint32_t,XrLirMapLiteral *);
XR_FUNC XrLirStatus xr_lir_map_literals_copy(XrLirModule *,const XrLirModule *);
XR_FUNC XrLirStatus xr_lir_map_operation(const XrLirModule *, XrLirMapOperation,
    XrLirType receiver, uint32_t *id);
XR_FUNC bool xr_lir_map_intrinsic_valid(const XrLirModule *, const XrLirIntrinsic *);
XR_FUNC XrLirStatus xr_lir_map_catalog_install(XrLirModule *, bool slices, bool arrays,
    bool display, bool strings, bool ranges);
#endif // XLIR_MAP_H
