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
};

static XrXirValueStatus arena_pool_size(const XrXirTypes *types, XrXirBudget *budget,
                                       size_t *output) {
    XrXirBudget remaining = *budget;
    XrXirStatus verified = xr_xir_types_verify(types, &remaining);
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
    arena->types = (XrXirTypes) {count ? nodes : NULL, count};
    for (uint32_t i = 0; i < count; ++i) {
        nodes[i] = types->nodes[i];
        nodes[i].parameters = nodes[i].parameter_count ? parameters : NULL;
        size_t parameter_bytes = (size_t) nodes[i].parameter_count * sizeof(*parameters);
        if (parameter_bytes) memcpy(parameters, types->nodes[i].parameters, parameter_bytes);
        parameters += nodes[i].parameter_count;
    }
    XR_CHECK((char *) parameters == (char *) arena + bytes, "type arena allocation mismatch");
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
