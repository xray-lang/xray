/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_catch.inc.c - Ordered error regions and scoped handler bindings
 */
static bool source_catch_body(SourceContext *ctx, XrCatchClause *clause, SourceValue error) {
    SourceName *locals = ctx->locals, *scope = ctx->scope;
    if (clause->var_name) {
        SourceName *binding = source_alloc(ctx,1,sizeof(*binding));
        if (!binding) return false;
        *binding = (SourceName){locals,clause->var_name,NULL,clause->body,SOURCE_LOCAL,
            error.id,ctx->module,XR_XIR_ERROR,false,false,0};
        ctx->locals = binding;
        XrXirSourceRange range = {ctx->module,clause->var_line,clause->var_column,
            clause->var_line,clause->var_column+(uint32_t)strlen(clause->var_name)};
        if (!source_query_declare(ctx,binding,XR_XIR_SOURCE_BINDING,ctx->bodies[ctx->function].declaration,range)) return false;
        source_query_binding_type(ctx,binding);
    }
    bool ok = scoped_statement(ctx,clause->body);
    ctx->locals = locals; ctx->scope = scope; return ok;
}
static bool source_catch_snapshot(SourceContext *ctx, XrXirInitializationRegion *region) {
    SourceFunction *body=&ctx->bodies[ctx->function];
    XrXirInstruction *ops=source_alloc(ctx,body->count,sizeof(*ops));
    XrXirBlock *blocks=source_alloc(ctx,body->block_count,sizeof(*blocks));
    uint32_t *operands=body->operand_count ? source_alloc(ctx,body->operand_count,sizeof(*operands)) : NULL;
    if (!ops || !blocks || (body->operand_count && !operands)) return false;
    memcpy(ops,body->ops,body->count*sizeof(*ops));
    memcpy(blocks,body->blocks,body->block_count*sizeof(*blocks));
    if (body->operand_count) memcpy(operands,body->operands,body->operand_count*sizeof(*operands));
    region->function=ctx->functions[ctx->function];
    region->function.instructions=ops; region->function.instruction_count=body->count;
    region->function.blocks=blocks; region->function.block_count=body->block_count;
    region->function.operands=operands; region->function.operand_count=body->operand_count;
    region->next=body->initialization_regions; body->initialization_regions=region;
    return true;
}
static bool source_dead_catch(SourceContext *ctx, XrCatchClause *clause, uint32_t checkpoint) {
    SourceFunction *body = &ctx->bodies[ctx->function], saved = *body;
    XrXirGeneric generic = ctx->generics[ctx->function];
    bool returned = ctx->returned;
    uint32_t count = 0;
    for (SourceLoop *loop = ctx->loop; loop; loop = loop->parent) ++count;
    SourceLoop *loops = count ? source_alloc(ctx,count,sizeof(*loops)) : NULL;
    if (count && !loops) return false;
    uint32_t i = 0;
    for (SourceLoop *loop = ctx->loop; loop; loop = loop->parent) loops[i++] = *loop;
    XrXirInitializationRegion *region=NULL;
    if (source_constructor_active(ctx)) {
        region=source_alloc(ctx,1,sizeof(*region));
        if (!region) return false;
        region->parent=body->initialization_parent; region->checkpoint=checkpoint;
        region->first_instruction=body->count; region->first_block=body->block_count;
        body->initialization_parent=region;
    }
    body->error_context = NULL;
    bool ok = begin_block(ctx) && source_catch_body(ctx,clause,(SourceValue){UINT32_MAX,XR_XIR_ERROR});
    if (ok && region) ok=source_catch_snapshot(ctx,region);
    i = 0;
    for (SourceLoop *loop = ctx->loop; loop; loop = loop->parent) *loop = loops[i++];
    saved.saw_return = body->saw_return;
    saved.initialization_regions=body->initialization_regions;
    *body = saved; ctx->generics[ctx->function] = generic; ctx->returned = returned;
    return ok;
}
static bool source_try(SourceContext *ctx, AstNode *node) {
    TryCatchNode *attempt = &node->as.try_catch;
    if (attempt->catch_count != 1 || !attempt->catch_clauses || !attempt->catch_clauses[0])
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"ordered catch filters require checked narrowing");
    XrCatchClause *clause = attempt->catch_clauses[0];
    if (clause->is_panic || clause->type || clause->pattern || !clause->body)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"typed, pattern and panic handlers require their checked contracts");
    SourceFunction *body = &ctx->bodies[ctx->function];
    uint32_t checkpoint=body->count;
    SourceErrorContext handler = {0}, *outer = body->error_context;
    body->error_context = &handler;
    bool ok = scoped_statement(ctx,attempt->try_body);
    body->error_context = outer;
    if (!ok) return false;
    if (!handler.count) return source_dead_catch(ctx,clause,checkpoint);
    uint32_t normal_jump = UINT32_MAX;
    if (!ctx->returned) {
        normal_jump = body->count;
        if (!emit(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{0},0,{0}},NULL)) return false;
    }
    uint32_t target = body->block_count;
    if (!begin_block(ctx)) return false;
    SourceValue *inputs = source_alloc(ctx,(size_t)handler.count*2,sizeof(*inputs));
    if (!inputs) return false;
    uint32_t at = handler.count;
    for (SourceErrorEdge *edge = handler.edges; edge; edge = edge->next) {
        if (!source_work(ctx,node)) return false;
        body->ops[edge->jump].targets[0] = target;
        --at; inputs[at*2] = (SourceValue){edge->block,XR_XIR_UNIT}; inputs[at*2+1] = edge->value;
    }
    SourceValue error;
    if (!emit_group(ctx,(XrXirInstruction){XR_XIR_PHI,XR_XIR_ERROR,{0},{0},0,{0}},inputs,handler.count*2,&error) ||
        !source_catch_body(ctx,clause,error)) return false;
    uint32_t handler_jump = UINT32_MAX;
    if (!ctx->returned) {
        handler_jump = body->count;
        if (!emit(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{0},0,{0}},NULL)) return false;
    }
    ctx->returned = normal_jump == UINT32_MAX && handler_jump == UINT32_MAX;
    if (ctx->returned) return true;
    target = body->block_count;
    if (!begin_block(ctx)) return false;
    if (normal_jump != UINT32_MAX) body->ops[normal_jump].targets[0] = target;
    if (handler_jump != UINT32_MAX) body->ops[handler_jump].targets[0] = target;
    return true;
}
