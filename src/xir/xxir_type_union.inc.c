/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_type_union.inc.c - Canonical declaration-local Error expressions
 *
 * KEY CONCEPT:
 *   A union owns only type metadata; actual errors keep their concrete enum.
 */
#include <stdlib.h>
static int error_union_order(const void *left, const void *right) {
    uint32_t a = (uint32_t)((const XrXirCallableParameter *)left)->type;
    uint32_t b = (uint32_t)((const XrXirCallableParameter *)right)->type;
    return a < b ? -1 : a > b;
}
static bool error_union_atom(const XrXirTypes *types, XrXirType type) {
    uint32_t id = (uint32_t)type;
    return type == XR_XIR_NEVER || type == XR_XIR_ERROR ||
        (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT) ||
        xr_xir_type_is_enum(types, type);
}
static const XrXirTypeNode *error_union_node(const XrXirTypes *types, XrXirType type) {
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node && node->kind == XR_XIR_TYPE_UNION ? node : NULL;
}
static bool error_union_shape(const XrXirTypes *types, XrXirType type) {
    const XrXirTypeNode *node = error_union_node(types, type);
    if (!node) return error_union_atom(types, type);
    if (node->parameter_count < 2 || !node->parameters || node->element || node->result || node->flags)
        return false;
    for (uint32_t p = 0; p < node->parameter_count; ++p) {
        XrXirType member = node->parameters[p].type;
        if (node->parameters[p].mode || member == XR_XIR_NEVER || member == XR_XIR_ERROR ||
            !error_union_atom(types, member) || (p && (uint32_t)node->parameters[p-1].type >= (uint32_t)member))
            return false;
    }
    return true;
}
XR_FUNC bool xr_xir_error_bound_accepts(const XrXirTypes *types, XrXirType actual, XrXirType bound) {
    if (!error_union_shape(types, actual) || !error_union_shape(types, bound)) return false;
    if (actual == XR_XIR_NEVER || bound == XR_XIR_ERROR || actual == bound) return true;
    const XrXirTypeNode *from = error_union_node(types, actual), *to = error_union_node(types, bound);
    if (!to) return false;
    uint32_t count = from ? from->parameter_count : 1, first = 0;
    for (uint32_t p = 0; p < count; ++p) {
        uint32_t member = (uint32_t)(from ? from->parameters[p].type : actual);
        while (first < to->parameter_count && (uint32_t)to->parameters[first].type < member) ++first;
        if (first == to->parameter_count || (uint32_t)to->parameters[first].type != member) return false;
    }
    return true;
}
XR_FUNC XrXirStatus xr_xir_compile_error_union(const XrXirCompileContext *context,
    const XrXirTypes *types, const XrXirType *members, uint32_t count, XrXirErrorUnion *output) {
    if (!xir_compile_context_valid(context) || !output || output->members || output->count ||
        output->scalar || (count && !members)) return XR_XIR_BAD_STRUCTURE;
    uint64_t total = 0;
    bool erased = false;
    /* Every input is checked before any absorbing expression is selected. The
     * caller's independent binder proof also checks every generic Error bound. */
    for (uint32_t i = 0; i < count; ++i) {
        if (!error_union_shape(types, members[i])) return XR_XIR_BAD_TYPE;
        const XrXirTypeNode *node = error_union_node(types, members[i]);
        total += node ? node->parameter_count : members[i] == XR_XIR_NEVER ? 0u : 1u;
        erased |= members[i] == XR_XIR_ERROR;
        if (total > UINT32_MAX || total > SIZE_MAX / sizeof(XrXirCallableParameter)) return XR_XIR_OVERFLOW;
    }
    if (erased || !total) {
        *output = (XrXirErrorUnion){.scalar = erased ? XR_XIR_ERROR : XR_XIR_NEVER};
        return XR_XIR_OK;
    }
    if (total > context->limits.parameters) return XR_XIR_BUDGET;
    XrXirStatus status = XR_XIR_OK;
    XrXirCallableParameter *vector = xir_compile_calloc(context, (size_t)total, sizeof(*vector), &status);
    if (!vector) return status;
    uint32_t used = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const XrXirTypeNode *node = error_union_node(types, members[i]);
        uint32_t n = node ? node->parameter_count : members[i] == XR_XIR_NEVER ? 0u : 1u;
        for (uint32_t p = 0; p < n; ++p) {
            XrXirType member = node ? node->parameters[p].type : members[i];
            vector[used++] = (XrXirCallableParameter){member,0};
        }
    }
    qsort(vector, used, sizeof(*vector), error_union_order);
    uint32_t distinct = 0;
    for (uint32_t p = 0; p < used; ++p)
        if (!distinct || vector[p].type != vector[distinct-1].type) vector[distinct++] = vector[p];
    used = distinct;
    if (used == 1) {
        XrXirType scalar = vector[0].type;
        xr_compile_resources_free(vector);
        *output = (XrXirErrorUnion){.scalar = scalar};
    } else *output = (XrXirErrorUnion){.members = vector, .count = used};
    return XR_XIR_OK;
}
