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
#include "xxir_compile_memory.h"
#include "xxir_storage.h"
#include "../base/xmalloc.h"
static XrXirStatus nullable_storage_layout(const XrXirCompileContext *compile_context,
    const XrXirTypes *types, XrXirType type, const XrXirTarget *target, XrXirLayout *layout);

XrXirStatus xr_xir_builtin_layout(XrXirType type, const XrXirTarget *target,
    XrXirLayoutContext context, XrXirLayout *layout) {
    bool carrier = type == XR_XIR_STRING ||
        type == XR_XIR_ERROR || type == XR_XIR_PANIC_INFO;
    if (!layout || !target || target->architecture != XR_XIR_ARCH_X86_64 ||
        target->abi_version != XR_XIR_VALUE_ABI_VERSION ||
        context < XR_XIR_LAYOUT_STORAGE || context > XR_XIR_LAYOUT_FRAME ||
        (type != XR_XIR_UNIT && type != XR_XIR_BOOL && type != XR_XIR_RUNE && !xr_xir_type_is_number(type) && !carrier) ||
        (type == XR_XIR_UNIT && context == XR_XIR_LAYOUT_PARAMETER)) return XR_XIR_BAD_LAYOUT;
    XrXirLayout result;
    if (context == XR_XIR_LAYOUT_PARAMETER || context == XR_XIR_LAYOUT_RESULT || context == XR_XIR_LAYOUT_BOXED)
        result = (XrXirLayout){16, 8};
    else if (type == XR_XIR_UNIT) result = (XrXirLayout){0, 1};
    else if (type == XR_XIR_BOOL && context == XR_XIR_LAYOUT_STORAGE) result = (XrXirLayout){1, 1};
    else if (type == XR_XIR_RUNE && context == XR_XIR_LAYOUT_STORAGE) result = (XrXirLayout){4, 4};
    else if (context == XR_XIR_LAYOUT_STORAGE && xr_xir_type_is_integer(type))
        result = (XrXirLayout){xr_xir_integer_bits(type) / 8, xr_xir_integer_bits(type) / 8};
    else if (context == XR_XIR_LAYOUT_STORAGE && xr_xir_float_bits(type))
        result = (XrXirLayout){xr_xir_float_bits(type) / 8, xr_xir_float_bits(type) / 8};
    else result = (XrXirLayout){8, 8};
    *layout = result; return XR_XIR_OK;
}
XrXirStatus xr_xir_compile_layout(const XrXirCompileContext *compile_context,
    const XrXirTypes *types, XrXirType type, const XrXirTarget *target,
    XrXirLayoutContext context, XrXirLayout *layout) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    if (!layout || !target || target->architecture != XR_XIR_ARCH_X86_64 ||
        target->abi_version != XR_XIR_VALUE_ABI_VERSION ||
        context < XR_XIR_LAYOUT_STORAGE || context > XR_XIR_LAYOUT_FRAME) return XR_XIR_BAD_LAYOUT;
    if (!xir_compile_work(compile_context, 1)) return XR_XIR_BUDGET;
    if (!xr_xir_type_node(types, type)) return xr_xir_builtin_layout(type, target, context, layout);
    if (!xr_xir_type_is_owned(types, type) || xr_xir_type_span(types, type) ||
        (xr_xir_type_is_nominal(types, type) && !xr_xir_type_is_class(types, type) &&
         context == XR_XIR_LAYOUT_STORAGE)) return XR_XIR_BAD_LAYOUT;
    if (context == XR_XIR_LAYOUT_STORAGE && xr_xir_type_is_nullable(types, type))
        return nullable_storage_layout(compile_context, types, type, target, layout);
    *layout = (context == XR_XIR_LAYOUT_PARAMETER || context == XR_XIR_LAYOUT_RESULT ||
        context == XR_XIR_LAYOUT_BOXED) ? (XrXirLayout){16, 8} : (XrXirLayout){8, 8};
    return XR_XIR_OK;
}

#include "xxir_nominal_storage.inc.c"

static XrXirStatus storage_layouts_into(const XrXirCompileContext *compile_context, const XrXirTypes *types, const XrXirTarget *target, XrXirStorageLayout *layouts, uint32_t count) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *remaining = &compile_state;
    if (!remaining || !target || target->architecture != XR_XIR_ARCH_X86_64 ||
        target->abi_version != XR_XIR_VALUE_ABI_VERSION ||
        count != (types ? types->count : 0) || (count != 0) != (layouts != NULL)) return XR_XIR_BAD_LAYOUT;
    XrXirCompileContext budget = *remaining;
    if (!xir_compile_work(&budget, count)) return XR_XIR_BUDGET;

    bool nominal = false;
    for (uint32_t i = 0; i < count; ++i) {
        const XrXirTypeNode *node = &types->nodes[i];
        layouts[i].depth = 0; layouts[i].owned_depth = 0; layouts[i].tag_bytes = 0; layouts[i].body = (XrXirLayout){0,0};
        if (node->parameter_span || layouts[i].field_count != node->nominal.field_count ||
            (layouts[i].field_count != 0) != (layouts[i].field_offsets != NULL)) return XR_XIR_BAD_LAYOUT;
        if (node->kind == XR_XIR_TYPE_NOMINAL || node->kind == XR_XIR_TYPE_NULLABLE) nominal = true;
        else {
            XrXirStatus status = xr_xir_compile_layout(compile_context, types, (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + i),
                target, XR_XIR_LAYOUT_STORAGE, &layouts[i].value);
            if (status != XR_XIR_OK) return status;
        }
    }
    if (!nominal) { *remaining = budget; return XR_XIR_OK; }
    uint64_t bytes = (uint64_t) count * (sizeof(NominalStorageNode) + sizeof(uint32_t));
    if (bytes > SIZE_MAX) return XR_XIR_BUDGET;
    NominalStorageNode *nodes = xir_compile_calloc(compile_context, 1, (size_t) bytes, &allocation_status);
    if (!nodes) return allocation_status;
    uint32_t *stack = (uint32_t *) (nodes + count);
    for (uint32_t i = 0; i < count; ++i) nodes[i].offsets = (uint32_t *) layouts[i].field_offsets;
    XrXirStatus status = XR_XIR_OK;
    for (uint32_t i = 0; i < count && status == XR_XIR_OK; ++i) {
        if (types->nodes[i].kind != XR_XIR_TYPE_NOMINAL && types->nodes[i].kind != XR_XIR_TYPE_NULLABLE) continue;
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
    xr_compile_resources_free(nodes);
    if (status == XR_XIR_OK) *remaining = budget;
    return status;
}

XrXirStatus xr_xir_compile_storage_layouts(const XrXirCompileContext *context,
    const XrXirTypes *types, const XrXirTarget *target, XrXirStorageLayout *layouts, uint32_t count) {
    if (!xir_compile_context_valid(context)) return XR_XIR_BAD_STRUCTURE;
    if (count != (types ? types->count : 0) || (!!count != !!layouts)) return XR_XIR_BAD_LAYOUT;
    if (!count) return storage_layouts_into(context, types, target, layouts, count);
    uint64_t fields = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        if (!!layouts[i].field_count != !!layouts[i].field_offsets) return XR_XIR_BAD_LAYOUT;
        if (layouts[i].field_count > SIZE_MAX / sizeof(uint32_t) - fields) return XR_XIR_BUDGET;
        fields += layouts[i].field_count;
    }
    XrXirStatus status = XR_XIR_OK;
    XrXirStorageLayout *result = xir_compile_calloc(context, count, sizeof(*result), &status);
    uint32_t *offsets = xir_compile_calloc(context, (size_t)fields, sizeof(*offsets), &status);
    if (status == XR_XIR_OK) {
        size_t offset = 0;
        for (uint32_t i = 0; i < count; ++i) {
            result[i].field_count = layouts[i].field_count;
            result[i].field_offsets = layouts[i].field_count ? offsets + offset : NULL;
            offset += layouts[i].field_count;
        }
        status = storage_layouts_into(context, types, target, result, count);
        uint64_t copy_bytes = (uint64_t)count * sizeof(*result) + fields * sizeof(*offsets);
        if (status == XR_XIR_OK && !xir_compile_work(context, copy_bytes)) status = XR_XIR_BUDGET;
        if (status == XR_XIR_OK) for (uint32_t i = 0; i < count; ++i) {
            uint32_t *destination = (uint32_t *)layouts[i].field_offsets;
            if (result[i].field_count) memcpy(destination, result[i].field_offsets,
                (size_t)result[i].field_count * sizeof(*destination));
            result[i].field_offsets = destination;
            layouts[i] = result[i];
        }
    }
    xr_compile_resources_free(offsets); xr_compile_resources_free(result); return status;
}
