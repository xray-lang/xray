/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_type_layout.c - One target-bound type layout for compiler and runtime
 *
 * KEY CONCEPT:
 *   Compact storage and compiler frames consume the same physical authority.
 */
#include "xxir_types.h"
#include "xxir_storage.h"
#include "../base/xmalloc.h"

XrXirStatus xr_xir_layout(const XrXirTypes *types, XrXirType type, const XrXirTarget *target,
                        XrXirLayoutContext context, XrXirLayout *layout) {
    if (!layout)
        return XR_XIR_BAD_LAYOUT;
    *layout = (XrXirLayout) {0, 0};
    if (!target || target->architecture != XR_XIR_ARCH_X86_64 ||
        target->abi_version != XR_XIR_VALUE_ABI_VERSION ||
        context < XR_XIR_LAYOUT_STORAGE || context > XR_XIR_LAYOUT_FRAME ||
        (type != XR_XIR_UNIT && type != XR_XIR_BOOL && !xr_xir_type_is_number(type) && !xr_xir_type_is_owned(types, type)) ||
        (xr_xir_type_is_nominal(types, type) && !xr_xir_type_is_class(types, type) && context == XR_XIR_LAYOUT_STORAGE) ||
        (type == XR_XIR_UNIT && context == XR_XIR_LAYOUT_PARAMETER) || xr_xir_type_span(types, type))
        return XR_XIR_BAD_LAYOUT;
    if (context == XR_XIR_LAYOUT_PARAMETER || context == XR_XIR_LAYOUT_RESULT ||
        context == XR_XIR_LAYOUT_BOXED)
        *layout = (XrXirLayout) {16, 8};
    else if (type == XR_XIR_UNIT)
        *layout = (XrXirLayout) {0, 1};
    else if (type == XR_XIR_BOOL && context == XR_XIR_LAYOUT_STORAGE)
        *layout = (XrXirLayout) {1, 1};
    else if (context == XR_XIR_LAYOUT_STORAGE && xr_xir_type_is_integer(type))
        *layout = (XrXirLayout) {xr_xir_integer_bits(type) / 8, xr_xir_integer_bits(type) / 8};
    else if (context == XR_XIR_LAYOUT_STORAGE && xr_xir_float_bits(type))
        *layout = (XrXirLayout) {xr_xir_float_bits(type) / 8, xr_xir_float_bits(type) / 8};
    else
        *layout = (XrXirLayout) {8, 8};
    return XR_XIR_OK;
}

#include "xxir_nominal_storage.inc.c"

XR_FUNC XrXirStatus xr_xir_storage_layouts(const XrXirTypes *types,
    const XrXirTarget *target, XrXirBudget *remaining,
    XrXirStorageLayout *layouts, uint32_t count) {
    if (!remaining || !target || target->architecture != XR_XIR_ARCH_X86_64 ||
        target->abi_version != XR_XIR_VALUE_ABI_VERSION ||
        count != (types ? types->count : 0) || (count != 0) != (layouts != NULL)) return XR_XIR_BAD_LAYOUT;
    XrXirBudget budget = *remaining;
    if (count > budget.work) return XR_XIR_BUDGET;
    budget.work -= count;
    bool nominal = false;
    for (uint32_t i = 0; i < count; ++i) {
        const XrXirTypeNode *node = &types->nodes[i];
        layouts[i].depth = 0; layouts[i].owned_depth = 0; layouts[i].tag_bytes = 0; layouts[i].body = (XrXirLayout){0,0};
        if (node->parameter_span || layouts[i].field_count != node->nominal.field_count ||
            (layouts[i].field_count != 0) != (layouts[i].field_offsets != NULL)) return XR_XIR_BAD_LAYOUT;
        if (node->kind == XR_XIR_TYPE_NOMINAL) nominal = true;
        else {
            XrXirStatus status = xr_xir_layout(types, (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + i),
                target, XR_XIR_LAYOUT_STORAGE, &layouts[i].value);
            if (status != XR_XIR_OK) return status;
        }
    }
    if (!nominal) { *remaining = budget; return XR_XIR_OK; }
    uint64_t bytes = (uint64_t) count * (sizeof(NominalStorageNode) + sizeof(uint32_t));
    if (bytes > SIZE_MAX || bytes > budget.scratch_bytes) return XR_XIR_BUDGET;
    NominalStorageNode *nodes = xr_calloc(1, (size_t) bytes);
    if (!nodes) return XR_XIR_OUT_OF_MEMORY;
    uint32_t *stack = (uint32_t *) (nodes + count);
    for (uint32_t i = 0; i < count; ++i) nodes[i].offsets = (uint32_t *) layouts[i].field_offsets;
    XrXirStatus status = XR_XIR_OK;
    for (uint32_t i = 0; i < count && status == XR_XIR_OK; ++i) {
        if (types->nodes[i].kind != XR_XIR_TYPE_NOMINAL) continue;
        if (!nodes[i].state) status = nominal_storage_walk(types, target, &budget, nodes, stack, i);
        if (status == XR_XIR_OK) {
            layouts[i].value = nodes[i].layout;
            layouts[i].depth = nodes[i].depth;
            layouts[i].owned_depth = nodes[i].owned_depth;
            layouts[i].tag_bytes = nodes[i].tag_bytes;
            if (xr_xir_type_is_class(types, (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i))) {
                layouts[i].body = nodes[i].layout; layouts[i].value = (XrXirLayout){8,8};
                layouts[i].depth = 0; layouts[i].owned_depth = 1;
            }
        }
    }
    xr_free(nodes);
    if (status == XR_XIR_OK) *remaining = budget;
    return status;
}
