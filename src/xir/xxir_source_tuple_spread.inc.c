/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_tuple_spread.inc.c - Owned prefix contexts for static Tuple spread
 *
 * KEY CONCEPT:
 *   A nested spread literal consumes only its own prefix of the remaining
 *   context. The enclosing literal checks exact arity after ordered evaluation.
 */
typedef struct SourceTuplePrefix {
    const XrXirCallableParameter *fields;
    uint32_t count;
    bool inferred;
    XrXirType repeated_type;
} SourceTuplePrefix;

typedef struct SourceTupleElements {
    SourceValue *values;
    uint32_t count, capacity;
    const SourceTuplePrefix *context;
} SourceTupleElements;

static SourceExpectedType source_tuple_field_context(const SourceTuplePrefix *context, uint32_t index) {
    XrXirType type = !context ? XR_XIR_UNIT : context->fields ? context->fields[index].type : context->repeated_type;
    return (SourceExpectedType){context != NULL, type,
        false, context && context->inferred};
}

static bool source_tuple_spread_value(SourceContext *ctx, AstNode *node,
    const SourceTuplePrefix *context, SourceValue *value) {
    SourceExpressionPlan *plan = source_plan_collect(ctx, node, (SourceExpectedType){0});
    if (!plan) return false;
    SourceExpressionPlan *literal = plan;
    while (literal->syntax->type == AST_GROUPING && literal->left) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        literal = literal->left;
    }
    if (context && literal->syntax->type == AST_TUPLE_LITERAL) {
        SourceTuplePrefix *owned = source_recipe_storage(ctx, 1, sizeof(*owned));
        if (!owned) return false;
        *owned = *context;
        literal->tuple_prefix = owned;
    }
    return source_plan_complete(ctx, plan, value);
}

static bool source_tuple_append(SourceContext *ctx, AstNode *node,
    SourceTupleElements *elements, SourceValue value, bool converted) {
    if (elements->count >= 65536)
        return source_fail(ctx, node, XR_XIR_BUDGET, "Tuple expanded arity exhausted");
    if (elements->context && elements->count >= elements->context->count)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Tuple literal requires an exact ordered arity context");
    SourceExpectedType hint = source_tuple_field_context(elements->context, elements->count);
    if (!converted) {
        XrHirConversion conversion;
        if (!source_conversion_plan(ctx, node, value.type, hint, &conversion) ||
            !source_conversion_emit(ctx, &conversion, &value)) return false;
    }
    if ((!hint.present || hint.inferred) && !source_root_join_record(ctx, &value)) return false;
    SourceValue *grown = source_query_append(ctx, elements->values, &elements->count,
        &elements->capacity, sizeof(*grown));
    if (!grown) return false;
    elements->values = grown;
    grown[elements->count - 1] = value;
    return true;
}

static bool source_tuple_expand(SourceContext *ctx, AstNode *node, SourceTupleElements *elements) {
    AstNode *source = node->as.spread_expr.expr;
    if (!source) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "Tuple spread source is missing");
    const SourceTuplePrefix *context = elements->context;
    SourceTuplePrefix tail = {context && context->fields ? context->fields + elements->count : NULL,
        context ? context->count - elements->count : 0, context && context->inferred,
        context ? context->repeated_type : XR_XIR_UNIT};
    SourceValue tuple;
    if (!source_tuple_spread_value(ctx, source, context ? &tail : NULL, &tuple)) return false;
    if (tuple.type == XR_XIR_UNIT) return true;
    const XrXirTypeNode *signature = xr_xir_tuple_signature(&ctx->types, tuple.type);
    if (!signature)
        return source_fail(ctx, source, XR_XIR_BAD_TYPE, "Tuple literal spread requires a statically known Tuple");
    uint32_t arity = signature->parameter_count;
    if (arity > 65536 - elements->count)
        return source_fail(ctx, source, XR_XIR_BUDGET, "Tuple expanded arity exhausted");
    /* Field emission and conversion can grow the type pool. Own the signature. */
    XrXirCallableParameter *types = source_alloc(ctx, arity, sizeof(*types));
    if (!types || !source_copy_bytes(ctx, source, types, signature->parameters, arity * sizeof(*types))) return false;
    for (uint32_t f = 0; f < arity; ++f) {
        SourceValue field;
        if (!((ctx)->diagnostic.status == XR_XIR_OK) ||
            !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_TUPLE_FIELD, types[f].type,
                {tuple.id}, {0}, f, {0}}, &field) ||
            !source_tuple_append(ctx, source, elements, field, false)) return false;
    }
    return true;
}

static bool source_tuple_expanded_literal(SourceContext *ctx, AstNode *node,
    SourceExpectedType expected, SourceValue *value) {
    const TupleLiteralNode *literal = &node->as.tuple_literal;
    const SourceTuplePrefix *context = ctx->active_expression->tuple_prefix;
    bool exact = !context && expected.present;
    SourceTuplePrefix owned = {0};
    if (exact) {
        const XrXirTypeNode *signature = xr_xir_tuple_signature(&ctx->types, expected.type);
        if (!signature && expected.type != XR_XIR_UNIT)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Tuple literal requires an exact ordered arity context");
        owned.count = signature ? signature->parameter_count : 0;
        owned.inferred = expected.inferred;
        XrXirCallableParameter *fields = owned.count ? source_alloc(ctx, owned.count, sizeof(*fields)) : NULL;
        if (owned.count && (!fields || !source_copy_bytes(ctx, node, fields,
                signature->parameters, owned.count * sizeof(*fields)))) return false;
        owned.fields = fields;
        context = &owned;
    }
    SourceTupleElements elements = {NULL, 0, 0, context};
    for (int i = 0; i < literal->count; ++i) {
        AstNode *child = literal->elements[i];
        if (!child) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "Tuple literal entry is missing");
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (child->type == AST_SPREAD_EXPR) {
            if (!source_tuple_expand(ctx, child, &elements)) return false;
        } else {
            if (elements.count >= 65536)
                return source_fail(ctx, child, XR_XIR_BUDGET, "Tuple expanded arity exhausted");
            if (context && elements.count >= context->count)
                return source_fail(ctx, child, XR_XIR_BAD_TYPE, "Tuple literal requires an exact ordered arity context");
            SourceValue element;
            if (!source_plan_expression(ctx, child, source_tuple_field_context(context, elements.count), &element) ||
                !source_tuple_append(ctx, child, &elements, element, true)) return false;
        }
    }
    if (exact && elements.count != context->count)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Tuple literal requires an exact ordered arity context");
    if (!elements.count) { *value = (SourceValue){.id=UINT32_MAX, .type=XR_XIR_UNIT,.place={0}}; return true; }
    XrXirCallableParameter *types = source_alloc(ctx, elements.count, sizeof(*types));
    SourceValue *payloads = source_recipe_storage(ctx, elements.count, sizeof(*payloads));
    if (!types || !payloads) return false;
    uint32_t payload_count = 0;
    for (uint32_t f = 0; f < elements.count; ++f) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        types[f] = (XrXirCallableParameter){elements.values[f].type, 0};
        if (elements.values[f].type != XR_XIR_UNIT) payloads[payload_count++] = elements.values[f];
    }
    XrXirType tuple;
    if (!source_intern_type(ctx, (XrXirTypeNode){.kind = XR_XIR_TYPE_TUPLE,
            .parameters = types, .parameter_count = elements.count}, &tuple)) return false;
    return source_recipe_group(ctx, (XrXirInstruction){XR_XIR_TUPLE_NEW, tuple, {0}, {0}, 0, {0}},
        payloads, payload_count, value);
}
