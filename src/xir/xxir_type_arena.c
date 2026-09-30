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
#include <string.h>

struct XrXirTypeArena {
    _Atomic(uint32_t) references;
    XrXirDomain *domain;
    size_t allocation_bytes;
    XrXirTypes types;
    XirNominalValue ***empty_variants;
    XrXirStorageLayout *storage;
};

#include "xxir_type_arena_nominal.inc.c"
#include "xxir_type_arena_enum.inc.c"

static XrXirStorageLayout *arena_storage(ArenaNominalCursor *c, const XrXirTypes *types) {
    uint32_t count = types ? types->count : 0;
    XrXirStorageLayout *layouts = arena_nominal_span(c, count, sizeof(*layouts), _Alignof(XrXirStorageLayout));
    for (uint32_t i = 0; i < count && c->status == XR_XIR_VALUE_OK; ++i) {
        uint32_t fields = types->nodes[i].nominal.field_count;
        uint32_t *offsets = arena_nominal_span(c, fields, sizeof(*offsets), _Alignof(uint32_t));
        if (layouts && c->status == XR_XIR_VALUE_OK)
            layouts[i] = (XrXirStorageLayout) {{0, 0}, offsets, fields, 0, 0, 0, {0,0}};
    }
    return layouts;
}

static XrXirValueStatus arena_pool_size(const XrXirTypes *types, XrXirBudget *budget,
                                       size_t *output) {
    XrXirBudget remaining = *budget;
    if (types && (types->interfaces || (types->nominals && types->nominals->declarations))) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirStatus verified = xr_xir_types_structure_verify(types, &remaining);
    if (verified != XR_XIR_OK)
        return verified == XR_XIR_BUDGET ? XR_XIR_VALUE_LIMIT :
            verified == XR_XIR_OUT_OF_MEMORY ? XR_XIR_VALUE_OOM : XR_XIR_VALUE_BAD_ARGUMENT;
    uint64_t bytes = sizeof(XrXirTypeArena);
    uint32_t count = types ? types->count : 0;
    if (count > remaining.work) return XR_XIR_VALUE_LIMIT;
    remaining.work -= count;
    bytes += (uint64_t) count * sizeof(XrXirTypeNode);
    for (uint32_t i = 0; i < count; ++i) {
        const XrXirTypeNode *node = &types->nodes[i];
        if (node->parameter_span) return XR_XIR_VALUE_BAD_ARGUMENT;
        if (node->parameter_count > remaining.work) return XR_XIR_VALUE_LIMIT;
        remaining.work -= node->parameter_count;
        uint64_t payload = (uint64_t) node->parameter_count * sizeof(*node->parameters);
        if (payload > UINT64_MAX - bytes) return XR_XIR_VALUE_LIMIT;
        bytes += payload;
    }
    uint64_t limit = budget->metadata_bytes < SIZE_MAX ? budget->metadata_bytes : SIZE_MAX;
    ArenaNominalCursor nominal = {NULL, bytes, limit, remaining.work, XR_XIR_VALUE_OK};
    arena_nominal_nodes(&nominal, types, NULL);
    arena_nominals(&nominal, types ? types->nominals : NULL);
    arena_empty_variants(&nominal, types, NULL);
    arena_storage(&nominal, types);
    if (nominal.status != XR_XIR_VALUE_OK) return nominal.status;
    bytes = nominal.offset; remaining.work = nominal.work;
    if (bytes > SIZE_MAX || bytes > budget->metadata_bytes) return XR_XIR_VALUE_LIMIT;
    remaining.metadata_bytes = budget->metadata_bytes - bytes;
    *budget = remaining;
    *output = (size_t) bytes;
    return XR_XIR_VALUE_OK;
}

XR_FUNC XrXirValueStatus xr_xir_type_arena_new(XrXirDomain *domain, const XrXirTypes *types,
    XrXirBudget *budget, XrXirTypeArena **output) {
    if (!output) return XR_XIR_VALUE_BAD_ARGUMENT;
    *output = NULL;
    if (!domain || !budget) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirBudget remaining = *budget;
    size_t bytes = 0;
    XrXirValueStatus status = arena_pool_size(types, &remaining, &bytes);
    if (status != XR_XIR_VALUE_OK) return status;
    if (!xr_xir_domain_retain(domain)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    XrXirTypeArena *arena = xr_xir_domain_allocate(domain, bytes, &status);
    if (!arena) { xr_xir_domain_drop(domain); return status; }
    atomic_init(&arena->references, 1);
    arena->domain = domain;
    arena->allocation_bytes = bytes;
    uint32_t count = types ? types->count : 0;
    XrXirTypeNode *nodes = (XrXirTypeNode *) (arena + 1);
    XrXirCallableParameter *parameters = (XrXirCallableParameter *) (nodes + count);
    arena->types = (XrXirTypes) {count ? nodes : NULL, count, NULL, NULL};
    for (uint32_t i = 0; i < count; ++i) {
        nodes[i] = types->nodes[i];
        nodes[i].parameters = nodes[i].parameter_count ? parameters : NULL;
        size_t parameter_bytes = (size_t) nodes[i].parameter_count * sizeof(*parameters);
        if (parameter_bytes) memcpy(parameters, types->nodes[i].parameters, parameter_bytes);
        parameters += nodes[i].parameter_count;
    }
    ArenaNominalCursor nominal = {(char *) arena, (uint64_t) ((char *) parameters - (char *) arena),
        bytes, UINT64_MAX, XR_XIR_VALUE_OK};
    arena_nominal_nodes(&nominal, types, nodes);
    arena->types.nominals = arena_nominals(&nominal, types ? types->nominals : NULL);
    arena->empty_variants = arena_empty_variants(&nominal, types, arena);
    arena->storage = arena_storage(&nominal, &arena->types);
    XR_CHECK(nominal.status == XR_XIR_VALUE_OK && nominal.offset == bytes, "type arena allocation mismatch");
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirStatus layout_status = xr_xir_storage_layouts(&arena->types, &target, &remaining, arena->storage, count);
    if (layout_status != XR_XIR_OK) {
        xr_xir_type_arena_drop(arena);
        return layout_status == XR_XIR_BUDGET ? XR_XIR_VALUE_LIMIT :
            layout_status == XR_XIR_OUT_OF_MEMORY ? XR_XIR_VALUE_OOM : XR_XIR_VALUE_BAD_ARGUMENT;
    }
    *budget = remaining;
    *output = arena;
    return XR_XIR_VALUE_OK;
}

XR_FUNC bool xr_xir_type_arena_retain(XrXirTypeArena *arena) {
    return arena && xr_xir_reference_retain(&arena->references);
}
XR_FUNC void xr_xir_type_arena_drop(XrXirTypeArena *arena) {
    if (arena && xr_xir_reference_release(&arena->references)) {
        XrXirDomain *domain = arena->domain;
        xr_xir_domain_deallocate(domain, arena, arena->allocation_bytes);
        xr_xir_domain_drop(domain);
    }
}
XR_FUNC const XrXirTypes *xr_xir_type_arena_types(const XrXirTypeArena *arena) {
    return arena ? &arena->types : NULL;
}
XR_FUNC const XrXirStorageLayout *xr_xir_type_arena_storage(const XrXirTypeArena *arena,
    XrXirType type) {
    return arena && xr_xir_type_node(&arena->types, type) ?
        &arena->storage[(uint32_t) type - XR_XIR_CONSTRUCTED_TYPE_BASE] : NULL;
}
XR_FUNC bool xr_xir_type_arena_layout(const XrXirTypeArena *arena,
    XrXirType type, XrXirLayout *layout) {
    if (!layout) return false;
    *layout = (XrXirLayout) {0, 0};
    const XrXirStorageLayout *stored = xr_xir_type_arena_storage(arena, type);
    if (stored) { *layout = stored->value; return true; }
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    return xr_xir_layout(NULL, type, &target, XR_XIR_LAYOUT_STORAGE, layout) == XR_XIR_OK;
}
