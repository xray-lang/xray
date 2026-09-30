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
typedef struct TypeContextProof {
    const XrXirTypes *types;
    const XrXirConstraint *constraints;
    uint32_t parameter_count;
    XrXirBudget *remaining;
    unsigned char *pending;
    bool interface_proofs;
} TypeContextProof;
static XrXirStatus type_context_edge(TypeContextProof *c, XrXirType type, uint32_t earlier) {
    if (!c->remaining->work) return XR_XIR_BUDGET;
    --c->remaining->work;
    uint32_t id = (uint32_t) type;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT)
        return id - XR_XIR_TYPE_PARAMETER_BASE < c->parameter_count ? XR_XIR_OK : XR_XIR_BAD_TYPE;
    const XrXirTypeNode *node = xr_xir_type_node(c->types, type);
    if (!node) return type == XR_XIR_UNIT || type == XR_XIR_BOOL || xr_xir_type_is_number(type) ||
        type == XR_XIR_STRING || type == XR_XIR_ATOMIC_I64 || type == XR_XIR_ERROR ||
        type == XR_XIR_PANIC_INFO ? XR_XIR_OK : XR_XIR_BAD_TYPE;
    uint32_t index = id - XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (index >= earlier || node->parameter_span > c->parameter_count) return XR_XIR_BAD_TYPE;
    c->pending[index] = 1;
    return XR_XIR_OK;
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
    if (d && c->interface_proofs) {
        XrXirConstraintEnvironment environment = {c->types, c->constraints, c->parameter_count};
        XrXirStatus status = xr_xir_constraint_arguments(&environment, d->constraints,
            node->nominal.arguments, node->nominal.argument_count, c->remaining);
        if (status != XR_XIR_OK) return status;
    }
    for (uint32_t a = 0; a < node->nominal.argument_count; ++a) {
        XrXirType argument = node->nominal.arguments[a];
        XrXirStatus status = type_context_edge(c, argument, index);
        if (status != XR_XIR_OK) return status;
        if (!d) continue;
        if (d->constraints[a].markers & ~XR_XIR_CONSTRAINT_MASK) return XR_XIR_BAD_TYPE;
        if (d->constraints[a].markers) {
            status = xr_xir_type_markers(c->types, argument, d->constraints[a].markers, c->constraints,
                c->parameter_count, &c->remaining->work);
            if (status != XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus type_context_verify(TypeContextProof *proof, XrXirType type) {
    const XrXirTypes *types = proof->types;
    const XrXirConstraint *constraints = proof->constraints;
    uint32_t parameter_count = proof->parameter_count;
    XrXirBudget *remaining = proof->remaining;
    if (!remaining || (parameter_count && !constraints)) return XR_XIR_BAD_STRUCTURE;
    TypeContextProof c = *proof;
    const XrXirTypeNode *root = xr_xir_type_node(types, type);
    if (!root) return type_context_edge(&c, type, 0);
    uint32_t count = (uint32_t) type - XR_XIR_CONSTRUCTED_TYPE_BASE + 1;
    if (count > remaining->metadata_bytes || count > remaining->work) return XR_XIR_BUDGET;
    remaining->metadata_bytes -= count; remaining->work -= count;
    c.pending = xr_calloc(count, 1);
    if (!c.pending) return XR_XIR_OUT_OF_MEMORY;
    XrXirStatus status = type_context_edge(&c, type, count);
    /* Descending expression IDs visit each reachable node once without recursion. */
    for (uint32_t at = count; at && status == XR_XIR_OK; --at) {
        uint32_t i = at - 1;
        if (!c.pending[i]) continue;
        const XrXirTypeNode *node = &types->nodes[i];
        if (node->kind == XR_XIR_TYPE_NOMINAL) status = type_context_nominal(&c, node, i);
        else if (node->kind == XR_XIR_TYPE_ARRAY || node->kind == XR_XIR_TYPE_CELL)
            status = type_context_edge(&c, node->element, i);
        else if (node->kind == XR_XIR_TYPE_CALLABLE) {
            status = type_context_edge(&c, node->result, i);
            if (node->parameter_count && !node->parameters) status = XR_XIR_BAD_STRUCTURE;
            for (uint32_t p = 0; p < node->parameter_count && status == XR_XIR_OK; ++p)
                status = type_context_edge(&c, node->parameters[p].type, i);
        } else status = XR_XIR_BAD_TYPE;
    }
    xr_free(c.pending); return status;
}
XR_FUNC XrXirStatus xr_xir_type_context_verify(const XrXirTypes *types, XrXirType type,
    const XrXirConstraint *constraints, uint32_t parameter_count, XrXirBudget *remaining) {
    TypeContextProof proof = {types, constraints, parameter_count, remaining, NULL, true};
    return type_context_verify(&proof, type);
}
XR_FUNC XrXirStatus xr_xir_type_context_verify_shape(const XrXirTypes *types, XrXirType type,
    const XrXirConstraint *constraints, uint32_t parameter_count, XrXirBudget *remaining) {
    TypeContextProof proof = {types, constraints, parameter_count, remaining, NULL, false};
    return type_context_verify(&proof, type);
}
