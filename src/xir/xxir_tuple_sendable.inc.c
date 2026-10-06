/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_tuple_sendable.inc.c - Bounded conjunction from complete type facts
 *
 * KEY CONCEPT:
 *   Enum identity is insufficient: every variant payload contributes, after
 *   the actual field projection has matched its owning declaration.
 */
typedef struct SendableFrame { uint32_t index, next; } SendableFrame;
static XrXirStatus sendable_enum_shape(const XrXirConstraintEnvironment *environment,
    const XrXirTypeNode *node, const XrXirCompileContext *work) {
    const XrXirNominalTable *table = environment->types->nominals;
    if (!table || (!!table->declarations == !!table->identities) ||
        node->nominal.declaration >= table->count) return XR_XIR_BAD_TYPE;
    const XrXirNominalDeclaration *d = table->declarations ?
        &table->declarations[node->nominal.declaration] : NULL;
    const XrXirNominalIdentity *identity = table->identities ?
        &table->identities[node->nominal.declaration] : NULL;
    uint32_t kind = d ? d->kind : identity->kind, count = d ? d->field_count : identity->field_count;
    uint32_t arity = d ? d->parameter_count : identity->arity;
    uint32_t variant_count = d ? d->variant_count : identity->variant_count;
    const XrXirNominalVariant *variants = d ? d->variants : identity->variants;
    if (kind != XR_XIR_NOMINAL_ENUM || !variant_count || !variants ||
        node->nominal.argument_count != arity || (!!node->nominal.arguments != !!arity) ||
        (!!node->nominal.fields != !!node->nominal.field_count)) return XR_XIR_BAD_TYPE;
    /* Identity-only pools have no declaration from which missing fields can
     * be reconstructed. Generic declarations require a real projection. */
    if (node->nominal.field_count != count &&
        (!d || arity || node->nominal.field_count || (count && !d->fields))) return XR_XIR_BAD_TYPE;
    uint32_t end = 0;
    for (uint32_t v = 0; v < variant_count; ++v) {
        if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
        if (variants[v].field_begin != end || variants[v].field_count > count - end)
            return XR_XIR_BAD_STRUCTURE;
        end += variants[v].field_count;
    }
    if (end != count) return XR_XIR_BAD_STRUCTURE;
    if (d && node->nominal.field_count) {
        if (!d->fields) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t f = 0; f < count; ++f) {
            XrXirStatus status = xr_xir_compile_type_substitution_matches(work, environment->types,
                node->nominal.arguments, arity, d->fields[f].type, node->nominal.fields[f]);
            if (status != XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
static uint32_t sendable_child_count(const XrXirConstraintEnvironment *environment,
    const XrXirTypeNode *node) {
    if (node->kind == XR_XIR_TYPE_TUPLE) return node->parameter_count;
    if (node->kind != XR_XIR_TYPE_NOMINAL) return 1;
    if (node->nominal.field_count) return node->nominal.field_count;
    const XrXirNominalTable *table = environment->types->nominals;
    return table->declarations ? table->declarations[node->nominal.declaration].field_count : 0;
}
static XrXirType sendable_child(const XrXirConstraintEnvironment *environment,
    const XrXirTypeNode *node, uint32_t at) {
    if (node->kind == XR_XIR_TYPE_TUPLE) return node->parameters[at].type;
    if (node->kind != XR_XIR_TYPE_NOMINAL) return node->element;
    return node->nominal.field_count ? node->nominal.fields[at] :
        environment->types->nominals->declarations[node->nominal.declaration].fields[at].type;
}
static XrXirStatus sendable_enter(const XrXirConstraintEnvironment *environment,
    XrXirType type, unsigned char *state, SendableFrame *frames, uint32_t *depth,
    const XrXirCompileContext *work) {
    if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
    if (type == XR_XIR_UNIT || type == XR_XIR_BOOL || type == XR_XIR_RUNE ||
        xr_xir_type_is_number(type) || type == XR_XIR_STRING) return XR_XIR_OK;
    uint32_t id = (uint32_t)type;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT) {
        const XrXirConstraint *fact = constraint_fact(environment, id - XR_XIR_TYPE_PARAMETER_BASE);
        return fact && (constraint_marker_closure(fact->markers) & XR_XIR_CONSTRAINT_SENDABLE) ?
            XR_XIR_OK : XR_XIR_BAD_TYPE;
    }
    const XrXirTypeNode *node = xr_xir_type_node(environment->types, type);
    if (!node || node->parameter_span > environment->parameter_count) return XR_XIR_BAD_TYPE;
    uint32_t index = id - XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (state[index] == 2) return XR_XIR_OK;
    if (state[index] || *depth >= environment->types->count) return XR_XIR_BAD_TYPE;
    if (node->kind == XR_XIR_TYPE_ATOMIC)
        return constraint_atomic(environment, node->element, XR_XIR_CONSTRAINT_ATOMIC_VALUE, work);
    if (node->kind == XR_XIR_TYPE_NOMINAL) {
        XrXirStatus status = sendable_enum_shape(environment, node, work);
        if (status != XR_XIR_OK) return status;
    } else if (node->kind == XR_XIR_TYPE_TUPLE) {
        if (!node->parameter_count || !node->parameters) return XR_XIR_BAD_STRUCTURE;
    } else if (node->kind != XR_XIR_TYPE_ARRAY && node->kind != XR_XIR_TYPE_NULLABLE &&
               node->kind != XR_XIR_TYPE_TASK) return XR_XIR_BAD_TYPE;
    state[index] = 1; frames[(*depth)++] = (SendableFrame){index, 0};
    return XR_XIR_OK;
}
static XrXirStatus constraint_sendable_graph(const XrXirConstraintEnvironment *environment,
    XrXirType type, const XrXirCompileContext *work) {
    const XrXirTypes *types = environment->types;
    if (!types || !types->count || !types->nodes) return XR_XIR_BAD_TYPE;
    uint64_t bytes = (uint64_t)types->count * (sizeof(SendableFrame) + 1);
    if (bytes > SIZE_MAX || !xir_compile_work(work, types->count)) return XR_XIR_BUDGET;
    XrXirStatus status = XR_XIR_OK;
    SendableFrame *frames = xir_compile_calloc(work, 1, (size_t)bytes, &status);
    if (!frames) return status;
    unsigned char *state = (unsigned char *)(frames + types->count);
    uint32_t depth = 0;
    status = sendable_enter(environment, type, state, frames, &depth, work);
    while (depth && status == XR_XIR_OK) {
        if (!xir_compile_work(work, 1)) { status = XR_XIR_BUDGET; break; }
        SendableFrame *frame = &frames[depth - 1];
        const XrXirTypeNode *node = &types->nodes[frame->index];
        if (frame->next == sendable_child_count(environment, node)) {
            state[frame->index] = 2; --depth; continue;
        }
        if (node->kind == XR_XIR_TYPE_TUPLE && node->parameters[frame->next].mode) {
            status = XR_XIR_BAD_TYPE; break;
        }
        XrXirType child = sendable_child(environment, node, frame->next++);
        /* Nominal projections can name later descriptors. All structural
         * edges retain their strict DAG order; active nominal cycles reject. */
        const XrXirTypeNode *child_node = xr_xir_type_node(types, child);
        if (node->kind != XR_XIR_TYPE_NOMINAL && child_node &&
            (uint32_t)child - XR_XIR_CONSTRUCTED_TYPE_BASE >= frame->index) {
            status = XR_XIR_BAD_TYPE; break;
        }
        status = sendable_enter(environment, child, state, frames, &depth, work);
    }
    xr_compile_resources_free(frames); return status;
}
