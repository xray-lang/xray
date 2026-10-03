/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_cleanup.inc.c - Lexical cleanup bodies without closure objects
 */
static bool source_defer(SourceContext *ctx, AstNode *node) {
    AstNode *block = node->as.defer_stmt.body;
    uint32_t outer = ctx->function;
    SourceFunction *owner = &ctx->bodies[outer];
    if (!block || block->type != AST_BLOCK || !owner->lexical_depth)
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "defer requires an enclosing real block and a block body");
    SourceCaptureScan scan = {ctx, NULL, NULL, 0, 0};
    if (!capture_scan(block, &scan)) return false;
    for (SourceCapture *p = scan.captures; p; p = p->next) if (p->source->construction) {
        uint32_t fields = xr_xir_type_node(&ctx->types, p->source->type)->nominal.field_count;
        if (fields > 65536 || scan.count - 1 > 65536 - fields)
            return source_fail(ctx, node, XR_XIR_BUDGET, "constructor cleanup capture budget exhausted");
        for (SourceCapture *q = scan.captures; q; q = q->next)
            if (q != p && q->source->kind != SOURCE_UNIT_LOCAL && q->index > p->index) q->index = q->index - 1 + fields;
        scan.count = scan.count - 1 + fields;
        break;
    }
    if (scan.count > 65536 || ctx->next_closure >= ctx->closure_limit)
        return source_fail(ctx, node, XR_XIR_BUDGET, "cleanup parameter or declaration budget exhausted");
    uint32_t index = ctx->next_closure++;
    SourceFunction *body = &ctx->bodies[index];
    body->node = node; body->type_owner = owner->type_owner; body->module = ctx->module;
    body->generic_owner = owner->generic_owner;
    body->type_parameters = owner->type_parameters; body->type_parameter_count = owner->type_parameter_count;
    ctx->generics[index].parameter_count = ctx->generics[outer].parameter_count;
    ctx->generics[index].constraints = ctx->generics[outer].constraints;
    char *name = source_alloc(ctx, 32, sizeof(*name));
    body->parameters = scan.count ? source_alloc(ctx,scan.count,sizeof(*body->parameters)) : NULL;
    SourceValue *captures = source_alloc(ctx, scan.count, sizeof(*captures));
    if (!name || (scan.count && !body->parameters) || !captures) return false;
    if (source_format(ctx, name, 32, "$cleanup%u", index) != XR_DIAG_OK)
        return source_fail(ctx, node, XR_XIR_BUDGET, "cleanup name formatting exhausted");
    SourceName declaration = {0}; declaration.name = name; declaration.node = node;
    if (!source_query_declare(ctx, &declaration, XR_XIR_SOURCE_FUNCTION, owner->declaration,
        source_query_range(ctx, node, NULL))) return false;
    body->declaration = declaration.declaration;
    ctx->functions[index] = (XrXirFunction){name, (uint32_t)source_text_size(ctx, name), body->parameters,
        scan.count, XR_XIR_UNIT, NULL, 0, NULL, 0, NULL, 0};
    ctx->identities[index] = ctx->identities[outer];
    ctx->identities[index].test_role = XR_XIR_TEST_ROLE_NONE;
    ctx->identities[index].test_timeout_seconds = 0;
    ctx->identities[index].method_kind = ctx->identities[index].nominal_owner ? XR_XIR_MEMBER_HELPER : XR_XIR_NON_MEMBER;
    ctx->identities[index].exported = 0; ctx->identities[index].cleanup_owner = outer + 1;
    SourceName *locals = ctx->locals, *scope = ctx->scope;
    SourceLoop *loop = ctx->loop; bool returned = ctx->returned;
    ctx->function = index; ctx->locals = ctx->scope = NULL; ctx->loop = NULL; ctx->returned = false;
    bool ok = source_capture_parameters(ctx, node, &scan) && statement(ctx, block, false) && finish_body(ctx);
    if (ok) {
        XrXirSourceDeclaration *query = (XrXirSourceDeclaration *)&ctx->query.declarations[body->declaration - 1];
        query->type = source_query_type(ctx, XR_XIR_UNIT); query->generic_parent = body->generic_owner;
        query->generic_parent_count = body->type_parameter_count; query->generic_parameter_count = body->type_parameter_count;
        query->generic_constraints = ctx->generics[index].constraints;
    }
    ctx->function = outer; ctx->locals = locals; ctx->scope = scope; ctx->loop = loop; ctx->returned = returned;
    if (!ok) return false;
    for (SourceCapture *p = scan.captures; p; p = p->next) {
        if (p->source->kind == SOURCE_UNIT_LOCAL) continue;
        XrXirType type = p->source->type;
        if (p->source->mutable && !source_cell_type(ctx, type, &type)) return false;
        if (p->source->construction) {
            uint32_t fields = xr_xir_type_node(&ctx->types, p->source->type)->nominal.field_count;
            for (uint32_t f = 0; f < fields; ++f)
                if (!source_constructor_storage(ctx, node, f, &captures[p->index + f])) return false;
        } else captures[p->index] = (SourceValue){p->source->index, type};
    }
    if (!owner->block_count && !begin_block(ctx)) return false;
    XrXirInstruction op = {XR_XIR_CLEANUP_REGISTER, XR_XIR_UNIT, {0}, {owner->block_count}, index, {0}};
    uint32_t count = ctx->generics[index].parameter_count;
    XrXirType *types = count ? source_alloc(ctx, count, sizeof(*types)) : NULL;
    if (count && !types) return false;
    for (uint32_t i = 0; i < count; ++i) types[i] = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE + i);
    if (!source_type_arguments(ctx, node, types, count, &op) || !source_recipe_group(ctx, op, captures, scan.count, NULL)) return false;
    owner->frontier = owner->count;
    return begin_block(ctx);
}
