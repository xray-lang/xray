/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_intern.inc.c - Declaration names and canonical Source types
 *
 * KEY CONCEPT:
 *   Canonical type nodes share the declaration checker's finite ownership ledger.
 */
static SourceName *find_name(SourceContext *ctx, SourceName *names, const char *name) {
    for (SourceName *p = names; p; p = p->next) {
        if (!source_work(ctx, p->node)) return NULL;
        if (source_text_same(ctx, NULL, p->name, name)) return p;
    }
    return NULL;
}
static SourceName *add_name(SourceContext *ctx, SourceName **head, const char *name, AstNode *node) {
    if (!name || !*name || find_name(ctx, *head, name)) {
        source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "duplicate or empty declaration name"); return NULL;
    }
    SourceName *symbol = source_alloc(ctx, 1, sizeof(*symbol));
    if (symbol) { symbol->name = name; symbol->node = node; symbol->next = *head; *head = symbol; }
    return symbol;
}
static bool source_type(SourceContext *ctx, XrTypeRef *ref, XrXirType *type);
static bool source_nominal_type(SourceContext *ctx, const char *name, XrXirType *type);
static SourceName *source_nominal_name(SourceContext *ctx, const char *name);
static bool source_nominal_arguments(SourceContext *ctx, const char *name, XrTypeRef **arguments,
    uint32_t count, XrXirType *type);
static bool source_intern_type(SourceContext *ctx, XrXirTypeNode node, XrXirType *type) {
    uint32_t span = xr_xir_type_span(&ctx->types,
        node.kind == XR_XIR_TYPE_CALLABLE ? node.result : node.element);
    for (uint32_t p = 0; p < node.parameter_count; ++p) {
        if (!source_work(ctx, NULL)) return false;
        if (!node.parameters[p].type && node.kind != XR_XIR_TYPE_TUPLE)
            return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "callable parameter must be a value type");
        uint32_t component = xr_xir_type_span(&ctx->types, node.parameters[p].type);
        if (component > span) span = component;
    }
    node.parameter_span = span;
    if (node.kind == XR_XIR_TYPE_NOMINAL) {
        for (uint32_t a = 0; a < node.nominal.argument_count; ++a) {
            if (!source_work(ctx, NULL)) return false;
            uint32_t component = xr_xir_type_span(&ctx->types, node.nominal.arguments[a]);
            if (component > node.parameter_span) node.parameter_span = component;
        }
    }
    for (uint32_t i = 0; i < ctx->types.count; ++i) {
        const XrXirTypeNode *s = &ctx->types.nodes[i];
        if (!source_work(ctx, NULL)) return false;
        if (s->kind != node.kind || s->element != node.element || s->parameter_count != node.parameter_count ||
            s->result != node.result || s->flags != node.flags) continue;
        if (node.kind == XR_XIR_TYPE_NOMINAL && (s->nominal.declaration != node.nominal.declaration ||
            s->nominal.argument_count != node.nominal.argument_count)) continue;
        bool same = true;
        for (uint32_t a = 0; a < node.nominal.argument_count; ++a) {
            if (!source_work(ctx, NULL)) return false;
            if (s->nominal.arguments[a] != node.nominal.arguments[a]) same = false;
        }
        for (uint32_t p = 0; p < node.parameter_count; ++p) {
            if (!source_work(ctx, NULL)) return false;
            if (s->parameters[p].type != node.parameters[p].type ||
                s->parameters[p].mode != node.parameters[p].mode) same = false;
        }
        if (same) { *type = (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + i); return true; }
    }
    const uint32_t limit = XR_XIR_CONSTRUCTED_TYPE_LIMIT - XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (ctx->types.count == limit)
        return source_fail(ctx, NULL, XR_XIR_BUDGET, "constructed type identity budget exhausted");
    if (ctx->types.count == ctx->type_capacity) {
        uint32_t capacity = ctx->type_capacity ? ctx->type_capacity * 2 : 8;
        if (capacity > limit) capacity = limit;
        XrXirTypeNode *table = source_alloc(ctx, capacity, sizeof(*table));
        if (!table) return false;
        if (!source_copy_bytes(ctx, NULL, table, ctx->types.nodes, ctx->types.count * sizeof(*table))) return false;
        ctx->types.nodes = table; ctx->type_capacity = capacity;
    }
    XrXirTypeNode *table = (XrXirTypeNode *) ctx->types.nodes;
    table[ctx->types.count] = node;
    *type = (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + ctx->types.count++); return true;
}
static bool source_signature(SourceContext *ctx, const XrXirCallableParameter *parameters,
    uint32_t count, XrXirType result, XrXirType *type) {
    return source_intern_type(ctx, (XrXirTypeNode) {XR_XIR_TYPE_CALLABLE, XR_XIR_UNIT,
        parameters, count, result, XR_XIR_CALLABLE_ROOT_UNRESOLVED, 0, {0}}, type);
}
static bool source_reference_promise(SourceContext *ctx, AstNode *node,
    uint32_t function, SourceExpectedType expected, XrXirType *type) {
    const XrXirTypeNode *context = expected.present ? xr_xir_callable_signature(&ctx->types, expected.type) : NULL;
    if (!context || !(context->flags & XR_XIR_CALLABLE_NO_SUSPEND)) return true;
    if (!(ctx->identities[function].promises & XR_XIR_FUNCTION_NO_SUSPEND))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "qualified reference requires an explicit target promise");
    XrXirTypeNode signature = *xr_xir_callable_signature(&ctx->types, *type);
    signature.flags |= XR_XIR_CALLABLE_NO_SUSPEND;
    return source_intern_type(ctx, signature, type);
}
static bool source_cell_type(SourceContext *ctx, XrXirType element, XrXirType *type) {
    return source_intern_type(ctx, (XrXirTypeNode) {XR_XIR_TYPE_CELL, element,
        NULL, 0, XR_XIR_UNIT, 0, 0, {0}}, type);
}
static bool source_nullable_type(SourceContext *ctx, XrXirType element, XrXirType *type) {
    if (!element || xr_xir_type_is_cell(&ctx->types,element))
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"nullable element must be an ordinary value type");
    return source_intern_type(ctx,(XrXirTypeNode){XR_XIR_TYPE_NULLABLE,element,NULL,0,XR_XIR_UNIT,0,0,{0}},type);
}
static XrGenericParam **source_type_parameters(SourceContext *ctx, int *count) {
    if (ctx->type_scope.active) {
        *count = (int)ctx->type_scope.count;
        return ctx->type_scope.parameters;
    }
    SourceFunction *body = &ctx->bodies[ctx->function];
    *count = (int)body->type_parameter_count;
    return body->type_parameters;
}
