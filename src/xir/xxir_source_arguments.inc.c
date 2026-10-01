/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_arguments.inc.c - Declaration-owned default argument functions
 *
 * KEY CONCEPT:
 *   Omission selects an ordinary checked call, not a runtime missing value.
 */
static bool source_argument_arity(SourceContext *ctx, AstNode *node, uint32_t index, uint32_t supplied) {
    uint32_t count = ctx->functions[index].parameter_count;
    if (supplied > count) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "too many call arguments");
    for (uint32_t p = supplied; p < count; ++p) {
        if (!source_work(ctx, node)) return false;
        if (!ctx->bodies[index].argument_defaults || !ctx->bodies[index].argument_defaults[p])
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "missing required call argument");
    }
    return true;
}
static bool source_argument_default(SourceContext *ctx, AstNode *node, uint32_t index, uint32_t parameter,
    SourceSubstitution *substitution, SourceValue *value) {
    uint32_t helper = ctx->bodies[index].argument_defaults[parameter];
    XrXirInstruction op = {XR_XIR_CALL, XR_XIR_UNIT, {0}, {0}, helper, {0}};
    return source_work(ctx, node) &&
        source_substitute(ctx, substitution, ctx->functions[helper].result, 0, &op.type) &&
        source_type_arguments(ctx, node, substitution->types, substitution->count, &op) && source_recipe_record(ctx, op, value);
}
static bool source_argument_functions(SourceContext *ctx, uint32_t *next) {
    uint32_t end = *next;
    for (uint32_t f = (uint32_t)ctx->graph->spec_count; f < end; ++f) {
        SourceFunction *body = &ctx->bodies[f];
        if (body->checked_library) continue;
        AstNode *node = body->node;
        XrParamNode **parameters = NULL;
        uint32_t count = 0, offset = 0;
        if (node->type == AST_FUNCTION_DECL) {
            parameters = node->as.function_decl.params; count = (uint32_t)node->as.function_decl.param_count;
        } else if (node->type == AST_METHOD_DECL) {
            parameters = node->as.method_decl.params; count = (uint32_t)node->as.method_decl.param_count;
            offset = node->as.method_decl.is_constructor || node->as.method_decl.is_static ? 0 : 1;
        } else continue;
        bool seen = false;
        for (uint32_t p = 0; p < count; ++p) {
            if (!source_work(ctx, node)) return false;
            AstNode *expression = parameters[p]->default_value;
            if (!expression) {
                if (seen) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "default parameters must be a trailing suffix");
                continue;
            }
            seen = true;
            if (!body->argument_defaults) {
                body->argument_defaults = source_alloc(ctx, count + offset, sizeof(*body->argument_defaults));
                if (!body->argument_defaults) return false;
            }
            if (*next >= ctx->first_closure)
                return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "default argument function inventory mismatch");
            uint32_t helper = (*next)++;
            body->argument_defaults[p + offset] = helper;
            ctx->functions[helper] = (XrXirFunction) {"$argument_default", 17, NULL, 0,
                ctx->functions[f].parameters[p + offset], NULL, 0, NULL, 0, NULL, 0};
            SourceFunction *target = &ctx->bodies[helper];
            target->node = node; target->type_owner = body->type_owner; target->module = body->module;
            target->declaration = body->declaration; target->generic_owner = body->generic_owner;
            target->type_parameters = body->type_parameters; target->type_parameter_count = body->type_parameter_count;
            target->default_expression = expression;
            ctx->generics[helper] = ctx->generics[f];
            ctx->identities[helper] = ctx->identities[f];
    ctx->identities[helper].method_kind = ctx->identities[helper].nominal_owner ? XR_XIR_MEMBER_HELPER : XR_XIR_NON_MEMBER;
        }
    }
    return true;
}
