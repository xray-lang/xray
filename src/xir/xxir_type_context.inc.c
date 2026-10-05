/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_type_context.inc.c - Declaration-local type-expression obligations
 *
 * KEY CONCEPT:
 *   Shared expression identities never share evidence from different scopes.
 */
#include "xxir_type_scratch_internal.h"

typedef struct TypeContextProof {
    const XrXirTypes *types;
    uint32_t parameter_count;
    XrXirCompileContext *remaining;
    unsigned char *pending;
} TypeContextProof;
static XrXirStatus type_context_edge(TypeContextProof *c, XrXirType type, uint32_t earlier) {
    if (!xir_compile_work(c->remaining, 1)) return XR_XIR_BUDGET;

    uint32_t id = (uint32_t) type;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT)
        return id - XR_XIR_TYPE_PARAMETER_BASE < c->parameter_count ? XR_XIR_OK : XR_XIR_BAD_TYPE;
    const XrXirTypeNode *node = xr_xir_type_node(c->types, type);
    if (!node) return type == XR_XIR_UNIT || type == XR_XIR_BOOL || type == XR_XIR_RUNE || xr_xir_type_is_number(type) ||
        type == XR_XIR_STRING || type == XR_XIR_ERROR ||
        type == XR_XIR_PANIC_INFO ? XR_XIR_OK : XR_XIR_BAD_TYPE;
    uint32_t index = id - XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (index >= earlier || node->parameter_span > c->parameter_count) return XR_XIR_BAD_TYPE;
    return xir_type_pending_mark(c->remaining,c->pending,index);
}
static XrXirStatus type_context_nominal(TypeContextProof *c, const XrXirTypeNode *node, uint32_t index) {
    const XrXirNominalTable *table = c->types->nominals;
    if (!table || node->nominal.declaration >= table->count ||
        (node->nominal.argument_count && !node->nominal.arguments)) return XR_XIR_BAD_TYPE;
    const XrXirNominalDeclaration *d = table->declarations ?
        &table->declarations[node->nominal.declaration] : NULL;
    if (d && (d->parameter_count != node->nominal.argument_count ||
        (d->parameter_count && !d->constraints))) return XR_XIR_BAD_TYPE;
    if (!d && !table->identities) return XR_XIR_BAD_TYPE;
    for (uint32_t a = 0; a < node->nominal.argument_count; ++a) {
        XrXirType argument = node->nominal.arguments[a];
        XrXirStatus status = type_context_edge(c, argument, index);
        if (status != XR_XIR_OK) return status;

    }
    return XR_XIR_OK;
}
static XrXirStatus type_context_verify(TypeContextProof *proof, XrXirType type, XirTypeScratch *scratch) {
    XrXirStatus allocation_status = XR_XIR_OK;
    const XrXirTypes *types = proof->types;
    XrXirCompileContext *remaining = proof->remaining;
    if (!remaining) return XR_XIR_BAD_STRUCTURE;
    TypeContextProof c = *proof;
    const XrXirTypeNode *root = xr_xir_type_node(types, type);
    if (!root) return type_context_edge(&c, type, 0);
    uint32_t count = (uint32_t) type - XR_XIR_CONSTRUCTED_TYPE_BASE + 1;
    c.pending = xir_type_scratch_pending(proof->remaining, scratch, count, &allocation_status);
    if (!c.pending) {  return allocation_status; }
    XrXirStatus status = type_context_edge(&c, type, count);
    /* Descending expression IDs visit each reachable node once without recursion. */
    uint32_t at=count;
    while (at && status==XR_XIR_OK) {
        uint32_t i=0; bool found=false;
        status=xir_type_pending_next(remaining,c.pending,&at,&i,&found);
        if (status!=XR_XIR_OK || !found) break;
        const XrXirTypeNode *node = &types->nodes[i];
        if (node->kind == XR_XIR_TYPE_NOMINAL) status = type_context_nominal(&c, node, i);
        else if (node->kind == XR_XIR_TYPE_ARRAY || node->kind == XR_XIR_TYPE_CELL ||
                 (node->kind == XR_XIR_TYPE_NULLABLE || node->kind == XR_XIR_TYPE_ATOMIC))
            status = type_context_edge(&c, node->element, i);
        else if (node->kind == XR_XIR_TYPE_CALLABLE || node->kind == XR_XIR_TYPE_TUPLE) {
            if (node->kind == XR_XIR_TYPE_CALLABLE) status = type_context_edge(&c, node->result, i);
            if (node->parameter_count && !node->parameters) status = XR_XIR_BAD_STRUCTURE;
            for (uint32_t p = 0; p < node->parameter_count && status == XR_XIR_OK; ++p)
                status = type_context_edge(&c, node->parameters[p].type, i);
        } else status = XR_XIR_BAD_TYPE;
    }
    return status;
}
XR_FUNC XrXirStatus xr_xir_compile_type_expression_shape_scratch(const XrXirCompileContext *compile_context,
    const XrXirTypes *types, XrXirType type, uint32_t parameter_count, XirTypeScratch *scratch) {
    if (!xir_compile_context_valid(compile_context) || !scratch ||
        scratch->resources != compile_context->resources) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    TypeContextProof proof = {types, parameter_count, &compile_state, NULL};
    return type_context_verify(&proof, type, scratch);
}
XR_FUNC XrXirStatus xr_xir_compile_type_expression_shape(const XrXirCompileContext *compile_context,
    const XrXirTypes *types, XrXirType type, uint32_t parameter_count) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XirTypeScratch scratch = {compile_context->resources, NULL, 0};
    XrXirStatus status = xr_xir_compile_type_expression_shape_scratch(
        compile_context, types, type, parameter_count, &scratch);
    xir_type_scratch_free(&scratch);
    return status;
}
