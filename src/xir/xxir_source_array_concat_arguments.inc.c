/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_array_concat_arguments.inc.c - Ordered fixed Tuple concat inputs
 *
 * KEY CONCEPT:
 *   Read authentic argument syntax once and retain every input snapshot before
 *   constructing the result. Expansion never changes the caller's AST.
 */
typedef struct SourceConcatArguments {
    SourceValue *values;
    uint32_t count, capacity;
    XrXirType array_type;
} SourceConcatArguments;

static bool source_concat_append(SourceContext *ctx, AstNode *node,
    SourceConcatArguments *arguments, SourceValue value) {
    if (arguments->count >= 65537)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array concat expanded argument limit exceeded");
    if (value.type != arguments->array_type)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array concat argument type differs from its receiver");
    SourceValue *values = source_query_append(ctx, arguments->values, &arguments->count,
        &arguments->capacity, sizeof(*values));
    if (!values) return false;
    arguments->values = values;
    values[arguments->count - 1] = value;
    return true;
}

static bool source_concat_spread_value(SourceContext *ctx, AstNode *node,
    XrXirType array_type, SourceValue *value) {
    SourceExpectedType expected = {false, XR_XIR_UNIT, false, false};
    if (!node)
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "Array concat spread expression is missing");
    if (node->type == AST_TUPLE_LITERAL && node->as.tuple_literal.count > 0) {
        uint32_t count = (uint32_t)node->as.tuple_literal.count;
        if (count > 65536)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array concat expanded argument limit exceeded");
        XrXirCallableParameter *fields = source_alloc(ctx, count, sizeof(*fields));
        if (!fields) return false;
        for (uint32_t i = 0; i < count; ++i) {
            if (!source_work(ctx, node)) return false;
            fields[i] = (XrXirCallableParameter){array_type, 0};
        }
        if (!source_intern_type(ctx, (XrXirTypeNode){.kind = XR_XIR_TYPE_TUPLE,
                .parameters = fields, .parameter_count = count}, &expected.type)) return false;
        expected.present = true;
    }
    return source_plan_expression(ctx, node, expected, value);
}

static bool source_concat_spread(SourceContext *ctx, AstNode *node,
    SourceConcatArguments *arguments) {
    SourceValue tuple;
    if (!source_concat_spread_value(ctx, node->as.spread_expr.expr, arguments->array_type, &tuple)) return false;
    if (tuple.type == XR_XIR_UNIT) return true;
    const XrXirTypeNode *signature = xr_xir_tuple_signature(&ctx->types, tuple.type);
    if (!signature)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array concat spread requires a fixed Tuple");
    uint32_t count = signature->parameter_count;
    if (count > 65537 - arguments->count)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array concat expanded argument limit exceeded");
    /* Emitting fields can grow the type pool. Keep the signature in owned storage. */
    XrXirCallableParameter *fields = source_alloc(ctx, count, sizeof(*fields));
    if (!fields || !source_copy_bytes(ctx, node, fields, signature->parameters, count * sizeof(*fields))) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (!source_work(ctx, node)) return false;
        if (fields[i].type != arguments->array_type || fields[i].mode)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Array concat spread fields require its receiver Array type");
        SourceValue value;
        if (!source_recipe_record(ctx, (XrXirInstruction){XR_XIR_TUPLE_FIELD, fields[i].type,
                {tuple.id}, {0}, i, {0}}, &value) ||
            !source_concat_append(ctx, node, arguments, value)) return false;
    }
    return true;
}

static bool source_concat_arguments(SourceContext *ctx, AstNode *node,
    SourceValue receiver, SourceConcatArguments *arguments) {
    arguments->array_type = receiver.type;
    if (!source_concat_append(ctx, node, arguments, receiver)) return false;
    const CallExprNode *call = &node->as.call_expr;
    for (int i = 0; i < call->arg_count; ++i) {
        AstNode *argument = call->arguments[i];
        if (!source_work(ctx, node)) return false;
        if (!argument)
            return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "Array concat argument expression is missing");
        if (argument->type == AST_SPREAD_EXPR) {
            if (!source_concat_spread(ctx, argument, arguments)) return false;
        } else {
            SourceValue value;
            if (!source_plan_expression(ctx, argument,
                    (SourceExpectedType){true, receiver.type, false, false}, &value) ||
                !source_concat_append(ctx, argument, arguments, value)) return false;
        }
    }
    return true;
}
