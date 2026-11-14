/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_map.inc.c - One typed catalog for canonical Map operations
 *
 * KEY CONCEPT:
 *   Constructors, observations and mutations share the receiving registry.
 *   Nullable results retain their actual constructed I64 payload type.
 */
enum { LIR_MAP_CATALOG_COUNT = 11 };
typedef struct LirMapSignature {
    XrLirMapOperation operation;
    XrLirType result;
    XrLirType parameters[3];
    uint32_t count;
} LirMapSignature;
static const LirMapSignature lir_map_signatures[LIR_MAP_CATALOG_COUNT] = {
    {XR_LIR_MAP_OP_NEW, XR_LIR_MAP_STRING_I64, {0}, 0},
    {XR_LIR_MAP_OP_LEN, XR_LIR_I64, {XR_LIR_MAP_STRING_I64}, 1},
    {XR_LIR_MAP_OP_GET, XR_LIR_TYPE_INVALID, {XR_LIR_MAP_STRING_I64, XR_LIR_STRING}, 2},
    {XR_LIR_MAP_OP_INDEX, XR_LIR_I64, {XR_LIR_MAP_STRING_I64, XR_LIR_STRING}, 2},
    {XR_LIR_MAP_OP_CONTAINS, XR_LIR_I1, {XR_LIR_MAP_STRING_I64, XR_LIR_STRING}, 2},
    {XR_LIR_MAP_OP_SET, XR_LIR_UNIT, {XR_LIR_MAP_STRING_I64_CELL, XR_LIR_STRING, XR_LIR_I64}, 3},
    {XR_LIR_MAP_OP_REMOVE, XR_LIR_I1, {XR_LIR_MAP_STRING_I64_CELL, XR_LIR_STRING}, 2},
    {XR_LIR_MAP_OP_CLEAR, XR_LIR_UNIT, {XR_LIR_MAP_STRING_I64_CELL}, 1},
    {XR_LIR_MAP_OP_SET, XR_LIR_UNIT, {XR_LIR_MAP_STRING_I64_REF, XR_LIR_STRING, XR_LIR_I64}, 3},
    {XR_LIR_MAP_OP_REMOVE, XR_LIR_I1, {XR_LIR_MAP_STRING_I64_REF, XR_LIR_STRING}, 2},
    {XR_LIR_MAP_OP_CLEAR, XR_LIR_UNIT, {XR_LIR_MAP_STRING_I64_REF}, 1}
};
XR_FUNC bool xr_lir_map_intrinsic_valid(const XrLirModule *m, const XrLirIntrinsic *entry) {
    if (!m || !entry || !entry->scalar_sync || (entry->parameter_count && !entry->parameters)) return false;
    for (uint32_t n = 0; n < LIR_MAP_CATALOG_COUNT; ++n) {
        const LirMapSignature *shape = &lir_map_signatures[n];
        if (entry->operation != (uint32_t)shape->operation || entry->parameter_count != shape->count) continue;
        if (shape->operation == XR_LIR_MAP_OP_GET) {
            XrLirTypeView result = {0};
            if (xr_lir_type_view(m, entry->result, &result) != XR_LIR_OK ||
                result.kind != XR_LIR_TYPE_NULLABLE || result.element != XR_LIR_I64) continue;
        } else if (entry->result != shape->result) continue;
        bool same = true;
        for (uint32_t p = 0; p < shape->count; ++p)
            same &= entry->parameters[p] == shape->parameters[p];
        if (same) return true;
    }
    return false;
}
XR_FUNC XrLirStatus xr_lir_map_operation(const XrLirModule *m, XrLirMapOperation operation,
    XrLirType receiver, uint32_t *output) {
    if (!m || !output || m->failure != XR_LIR_OK) return XR_LIR_INVALID_ARGUMENT;
    *output = 0;
    for (uint32_t n = 0; n < m->intrinsic_count; ++n) {
        const XrLirIntrinsic *entry = &m->intrinsics[n];
        if (entry->operation == (uint32_t)operation && xr_lir_map_intrinsic_valid(m, entry) &&
            (operation == XR_LIR_MAP_OP_NEW || entry->parameters[0] == receiver)) {
            *output = entry->id; return XR_LIR_OK;
        }
    }
    return XR_LIR_UNSUPPORTED;
}
XR_FUNC XrLirStatus xr_lir_map_catalog_install(XrLirModule *m, bool views, bool arrays,
    bool display, bool strings, bool ranges) {
    return lir_library_catalog(m, (views ? LIR_LIBRARY_VIEW : 0) | (arrays ? LIR_LIBRARY_ARRAY : 0) |
        (display ? LIR_LIBRARY_DISPLAY : 0) | (strings ? LIR_LIBRARY_STRING : 0) |
        (ranges ? LIR_LIBRARY_RANGE : 0) | LIR_LIBRARY_MAP);
}
