/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_arguments.inc.c - Declaration-owned default argument functions
 *
 * KEY CONCEPT:
 *   Omission selects a checked declaration purpose, not a runtime missing value.
 */
/* The source arena owns the single sorted declaration relation. Import remapping
 * may reorder owners, so insertion never assumes producer traversal order. */
static bool source_default_binding_add(SourceContext *ctx, AstNode *node,
    uint32_t owner, uint32_t ordinal, uint32_t helper) {
    uint32_t count = ctx->defaults.count, position = 0;
    const XrXirDefaultBinding *old = ctx->defaults.records;
    for (; position < count; ++position) {
        if (!source_work(ctx, node)) return false;
        if (old[position].owner > owner ||
            (old[position].owner == owner && old[position].ordinal >= ordinal)) break;
    }
    if (position < count && old[position].owner == owner && old[position].ordinal == ordinal)
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "duplicate parameter default binding");
    if (count == UINT32_MAX)
        return source_fail(ctx, node, XR_XIR_BUDGET, "parameter default inventory exhausted");
    uint64_t moved = count - position;

    XrXirDefaultBinding *records = (XrXirDefaultBinding *)ctx->defaults.records;
    if (count == ctx->default_capacity) {
        uint32_t capacity = count > UINT32_MAX / 2 ? UINT32_MAX : count ? count * 2 : 8;

        records = source_alloc(ctx, capacity, sizeof(*records));
        if (!records) return false;
        if (!source_work_units(ctx, node, (uint64_t)count * sizeof(*records))) return false;
        if (count) memcpy(records, old, (size_t)count * sizeof(*records));
        ctx->default_capacity = capacity;
    }
    if (!source_work_units(ctx, node, moved * sizeof(*records))) return false;
    if (moved) memmove(records + position + 1, records + position, (size_t)moved * sizeof(*records));
    records[position] = (XrXirDefaultBinding){XR_XIR_DEFAULT_PARAMETER, owner, ordinal, helper};
    ctx->defaults = (XrXirDefaultTable){records, count + 1};
    return true;
}
static bool source_default_binding_get(SourceContext *ctx, AstNode *node,
    uint32_t owner, uint32_t ordinal, const XrXirDefaultBinding **binding) {
    XrXirDeclarations declarations;
    XrXirModule module = source_module_view(ctx, &declarations);
    XrXirStatus status = xr_xir_compile_default_lookup(&ctx->compile, &module, owner, ordinal, binding);
    return status == XR_XIR_OK || source_fail(ctx, node, status, "parameter default lookup failed");
}
static bool source_argument_arity(SourceContext *ctx, AstNode *node, uint32_t index, uint32_t supplied) {
    uint32_t count = ctx->functions[index].parameter_count;
    if (supplied > count) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "too many call arguments");
    for (uint32_t p = supplied; p < count; ++p) {
        const XrXirDefaultBinding *binding = NULL;
        if (!source_work(ctx, node) || !source_default_binding_get(ctx, node, index, p, &binding)) return false;
        if (!binding) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "missing required call argument");
    }
    return true;
}
static bool source_argument_default(SourceContext *ctx, AstNode *node, uint32_t index, uint32_t parameter,
    SourceSubstitution *substitution, SourceValue *value) {
    const XrXirDefaultBinding *binding = NULL;
    if (!source_default_binding_get(ctx, node, index, parameter, &binding)) return false;
    if (!binding) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "parameter default binding is missing");
    XrXirInstruction op = {XR_XIR_CALL_DEFAULT, XR_XIR_UNIT, {0}, {index, parameter}, 0, {0}};
    return source_work(ctx, node) &&
        source_substitute(ctx, substitution, ctx->functions[index].parameters[parameter], 0, &op.type) &&
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
            if (*next >= ctx->first_closure)
                return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "default argument function inventory mismatch");
            uint32_t helper = (*next)++;
            if (!source_default_binding_add(ctx, node, f, p + offset, helper)) return false;
            ctx->functions[helper] = (XrXirFunction) {"$argument_default", 17, NULL, 0,
                ctx->functions[f].parameters[p + offset], NULL, 0, NULL, 0, NULL, 0};
            SourceFunction *target = &ctx->bodies[helper];
            target->node = node; target->type_owner = body->type_owner; target->module = body->module;
            target->declaration = body->declaration; target->generic_owner = body->generic_owner;
            target->type_parameters = body->type_parameters; target->type_parameter_count = body->type_parameter_count;
            target->default_expression = expression;
            ctx->generics[helper] = ctx->generics[f];
            ctx->identities[helper] = ctx->identities[f];
            ctx->identities[helper].test_role = XR_XIR_TEST_ROLE_NONE;
            ctx->identities[helper].test_timeout_seconds = 0;
            ctx->identities[helper].exported = false;
            ctx->identities[helper].promises = 0;
            ctx->identities[helper].method_kind = ctx->identities[helper].nominal_owner ? XR_XIR_MEMBER_HELPER : XR_XIR_NON_MEMBER;
        }
    }
    return true;
}
