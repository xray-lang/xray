/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_type_arena.c - Closed type pools with independent atomic ownership
 *
 * KEY CONCEPT:
 *   One charged allocation owns flat nodes and parameters without code leases.
 */
#include "xxir_type_arena.h"
#include "xxir_types.h"
#include "xxir_value_internal.h"
#include "xxir_compile_memory.h"

struct XrXirTypeArena {
    _Atomic(uint32_t) references;
    XrXirCompileContext context;
    XrXirTypes types;
    XirNominalValue ***empty_variants;
    XrXirStorageLayout *storage;
};

#include "xxir_type_arena_nominal.inc.c"
#include "xxir_type_arena_enum.inc.c"

static XrXirValueStatus arena_status(XrXirStatus status) {
    return status == XR_XIR_OK ? XR_XIR_VALUE_OK :
        status == XR_XIR_BUDGET ? XR_XIR_VALUE_LIMIT :
        status == XR_XIR_OUT_OF_MEMORY ? XR_XIR_VALUE_OOM : XR_XIR_VALUE_BAD_ARGUMENT;
}
static XrXirStorageLayout *arena_storage(ArenaNominalCursor *c, const XrXirTypes *types) {
    uint32_t count = types ? types->count : 0;
    XrXirStorageLayout *layouts = arena_nominal_span(c, count, sizeof(*layouts), _Alignof(XrXirStorageLayout));
    for (uint32_t i = 0; i < count && c->status == XR_XIR_VALUE_OK; ++i) {
        if (!arena_nominal_work(c, 1)) break;
        uint32_t fields = types->nodes[i].nominal.field_count;
        uint32_t *offsets = arena_nominal_span(c, fields, sizeof(*offsets), _Alignof(uint32_t));
        if (layouts && arena_nominal_work(c, sizeof(*layouts)))
            layouts[i] = (XrXirStorageLayout) {{0, 0}, offsets, fields, 0, 0, 0, {0,0}};
    }
    return layouts;
}
/* Both traversals consume the caller's ledger. The first only computes spans;
 * the second additionally charges the actual metadata writes and byte copies. */
static XrXirValueStatus arena_pool_walk(const XrXirCompileContext *context,
    const XrXirTypes *types, XrXirTypeArena *arena, size_t limit, size_t *size) {
    ArenaNominalCursor cursor = {(char *)arena, sizeof(XrXirTypeArena), limit, context, XR_XIR_VALUE_OK};
    uint32_t count = types ? types->count : 0;
    XrXirTypeNode *nodes = arena_nominal_span(&cursor, count, sizeof(*nodes), _Alignof(XrXirTypeNode));
    for (uint32_t i = 0; i < count && cursor.status == XR_XIR_VALUE_OK; ++i) {
        if (!arena_nominal_work(&cursor, 1)) break;
        const XrXirTypeNode *source = &types->nodes[i];
        if (source->parameter_span) return XR_XIR_VALUE_BAD_ARGUMENT;
        XrXirCallableParameter *parameters = arena_nominal_span(&cursor, source->parameter_count,
            sizeof(*parameters), _Alignof(XrXirCallableParameter));
        uint64_t parameter_bytes = (uint64_t)source->parameter_count * sizeof(*parameters);
        if (nodes && arena_nominal_work(&cursor, sizeof(*nodes) + sizeof(nodes[i].parameters) + parameter_bytes)) {
            nodes[i] = *source;
            nodes[i].parameters = parameters;
            if (parameter_bytes) memcpy(parameters, source->parameters, (size_t)parameter_bytes);
        }
    }
    arena_nominal_nodes(&cursor, types, nodes);
    XrXirNominalTable *nominals = arena_nominals(&cursor, types ? types->nominals : NULL);
    XirNominalValue ***empty_variants = arena_empty_variants(&cursor, types, arena);
    XrXirStorageLayout *storage = arena_storage(&cursor, types);
    if (cursor.status != XR_XIR_VALUE_OK) return cursor.status;
    if (arena) {
        if (!arena_nominal_work(&cursor, sizeof(arena->types) + sizeof(arena->empty_variants) + sizeof(arena->storage)))
            return cursor.status;
        arena->types = (XrXirTypes) {nodes, count, nominals, NULL};
        arena->empty_variants = empty_variants;
        arena->storage = storage;
    }
    *size = (size_t)cursor.offset;
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_compile_type_arena_new(const XrXirCompileContext *context,
    const XrXirTypes *types, XrXirTypeArena **output) {
    if (!output || *output || !xir_compile_context_valid(context)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (types && (types->interfaces || (types->nominals && types->nominals->declarations)))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirStatus status = xr_xir_compile_types_structure_verify(context, types);
    if (status != XR_XIR_OK) return arena_status(status);
    size_t bytes = 0;
    XrXirValueStatus value_status = arena_pool_walk(context, types, NULL, SIZE_MAX, &bytes);
    if (value_status != XR_XIR_VALUE_OK) return value_status;
    XrXirTypeArena *arena = xir_compile_calloc(context, 1, bytes, &status);
    if (!arena) return arena_status(status);
    if (!xir_compile_work(context, sizeof(arena->references) + sizeof(arena->context))) {
        value_status = XR_XIR_VALUE_LIMIT;
        goto failed;
    }
    atomic_init(&arena->references, 1);
    arena->context = *context;
    size_t copied = 0;
    value_status = arena_pool_walk(context, types, arena, bytes, &copied);
    if (value_status != XR_XIR_VALUE_OK) goto failed;
    if (copied != bytes) { value_status = XR_XIR_VALUE_BAD_ARGUMENT; goto failed; }
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    value_status = arena_status(xr_xir_compile_storage_layouts(context, &arena->types, &target,
        arena->storage, arena->types.count));
    if (value_status != XR_XIR_VALUE_OK) goto failed;
    *output = arena;
    return XR_XIR_VALUE_OK;
failed:
    xr_compile_resources_free(arena);
    return value_status;
}
XR_FUNC bool xr_xir_compile_type_arena_retain(XrXirTypeArena *arena) {
    return arena && xr_xir_reference_retain(&arena->references);
}
XR_FUNC void xr_xir_compile_type_arena_drop(XrXirTypeArena *arena) {
    if (arena && xr_xir_reference_release(&arena->references)) xr_compile_resources_free(arena);
}
XR_FUNC const XrXirTypes *xr_xir_compile_type_arena_types(const XrXirTypeArena *arena) {
    return arena ? &arena->types : NULL;
}
XR_FUNC const XrXirStorageLayout *xr_xir_compile_type_arena_storage(const XrXirTypeArena *arena,
    XrXirType type) {
    return arena && xr_xir_type_node(&arena->types, type) ?
        &arena->storage[(uint32_t)type - XR_XIR_CONSTRUCTED_TYPE_BASE] : NULL;
}
XR_FUNC bool xr_xir_compile_type_arena_layout(const XrXirTypeArena *arena,
    XrXirType type, XrXirLayout *layout) {
    if (!layout) return false;
    *layout = (XrXirLayout) {0, 0};
    const XrXirStorageLayout *stored = xr_xir_compile_type_arena_storage(arena, type);
    if (stored) { *layout = stored->value; return true; }
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    return xr_xir_builtin_layout(type, &target, XR_XIR_LAYOUT_STORAGE, layout) == XR_XIR_OK;
}
