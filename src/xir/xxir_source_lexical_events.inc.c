/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_lexical_events.inc.c - Preserve actual Source execution boundaries
 */
static bool source_lexical_event(SourceContext *ctx, XirLexicalEvent event) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (ctx->diagnostic.status != XR_XIR_OK || body->region_sealed) return false;
    SourceLexicalEvent *record = source_alloc(ctx, 1, sizeof(*record));
    if (!record) return false;
    if (event.kind != XIR_LEXICAL_TEMP_BEGIN && event.kind != XIR_LEXICAL_TEMP_END) {
        event.statement = body->lexical_temporary ? body->lexical_temporary->statement : 0;
        event.parent_statement = body->lexical_temporary && body->lexical_temporary->parent ?
            body->lexical_temporary->parent->statement : 0;
    }
    event.block = body->block_count ? body->current_block.identity : 0;
    event.frontier = body->frontier;
    if (event.kind==XIR_LEXICAL_ENTER || event.kind==XIR_LEXICAL_LEAVE ||
        event.kind==XIR_LEXICAL_BIND || event.kind==XIR_LEXICAL_REPLACE || event.kind==XIR_LEXICAL_TAKE ||
        event.kind==XIR_LEXICAL_TEMP_BEGIN || event.kind==XIR_LEXICAL_TEMP_END)
        event.target_frontier=body->frontier;
    *record = (SourceLexicalEvent){body->lexical_events, event,
        body->block_count ? body->blocks[event.block].block.count : 0};
    body->lexical_events = record;
    return true;
}

static bool source_lexical_temporary(SourceContext *ctx, AstNode *node, bool begin) {
    if (!node) return true;
    if (node->node_id == UINT32_MAX)
        return source_fail(ctx, node, XR_XIR_BUDGET, "full expression identity exhausted");
    XrHirBodyScope scope = {0};
    if (!source_hir_current_scope(ctx, &scope)) return false;
    SourceFunction *body = &ctx->bodies[ctx->function];
    SourceLexicalTemporary *temporary = body->lexical_temporary;
    if (begin) {
        temporary = source_alloc(ctx, 1, sizeof(*temporary));
        if (!temporary) return false;
        *temporary = (SourceLexicalTemporary){body->lexical_temporary,node->node_id + 1};
    } else if (!temporary || temporary->statement != node->node_id + 1) {
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"full expression lost its lexical parent");
    }
    if (!source_lexical_event(ctx, (XirLexicalEvent){
        .scope=scope.index, .target=scope.index, .binding=UINT32_MAX, .value=UINT32_MAX,
        .statement=temporary->statement,
        .parent_statement=temporary->parent ? temporary->parent->statement : 0,
        .kind=begin ? XIR_LEXICAL_TEMP_BEGIN : XIR_LEXICAL_TEMP_END})) return false;
    body->lexical_temporary = begin ? temporary : temporary->parent;
    return true;
}

static bool source_lexical_binding_event(SourceContext *ctx,
    const XrHirBindingAction *action, const SourceValue *value) {
    XrHirLexicalUse use;
    if (!xr_hir_body_lexical_use(ctx->typed_body, action->use, &use))
        return source_fail(ctx, NULL, XR_XIR_BAD_STRUCTURE, "lexical binding use lost its typed owner");
    if (!use.binding.owner) return true;
    XrHirLexicalBinding binding;
    if (!xr_hir_body_lexical_binding(ctx->typed_body, use.binding.index, &binding))
        return source_fail(ctx, NULL, XR_XIR_BAD_STRUCTURE, "lexical binding lost its declaration");
    if (binding.declaration.origin.function == UINT32_MAX) return true;
    XirLexicalEventKind kind;
    uint32_t actual = UINT32_MAX;
    if (use.initializes) {
        kind = XIR_LEXICAL_BIND;
        if (value && (value->type != XR_XIR_UNIT || binding.role!=XIR_LEXICAL_LOCAL)) actual = value->id;
    } else if (use.operation == XR_XIR_CELL_WRITE || use.operation == XR_XIR_CELL_WRITE_TAKE) {
        kind = XIR_LEXICAL_REPLACE;
        actual = use.incoming;
    } else if (use.operation == XR_XIR_CELL_TAKE || use.operation == XR_XIR_VALUE_TAKE) {
        kind = XIR_LEXICAL_TAKE;
        if (value && value->type != XR_XIR_UNIT) actual = value->id;
    } else return true;
    XrHirBodyScope scope = {0};
    if (!source_hir_current_scope(ctx, &scope)) return false;
    return source_lexical_event(ctx, (XirLexicalEvent){
        .scope=scope.index, .target=binding.declaration.scope.index,
        .binding=use.binding.index, .value=actual, .statement=ctx->debug_point.statement,
        .kind=kind});
}
