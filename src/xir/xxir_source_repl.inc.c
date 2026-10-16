/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_repl.inc.c - Typed implicit declarations over an unchanged AST
 */
static bool source_repl_select_tail(SourceContext *ctx) {
    if(!ctx->interactive)return true;
    if(!ctx->graph || ctx->graph->entry_index<0 || !ctx->binding_readers || !ctx->self_unit ||
        !ctx->self_unit->interactive || ctx->linkage_kind!=XR_XIR_PROGRAM)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"interactive input requires its real Source record");
    AstNode *program=ctx->graph->specs[ctx->graph->entry_index].ast;
    if(!program || program->type!=AST_PROGRAM || program->as.program.count<0)
        return source_fail(ctx,program,XR_XIR_BAD_STRUCTURE,"interactive input has no complete Program AST");
    if(!program->as.program.count)return true;
    AstNode *tail=program->as.program.statements[program->as.program.count-1];
    if(!tail || tail->type!=AST_EXPR_STMT || !tail->as.expr_stmt)return true;
    switch(tail->as.expr_stmt->type) {
    case AST_ASSIGNMENT:case AST_COMPOUND_ASSIGNMENT:case AST_INC:case AST_DEC:return true;
    default:ctx->repl_tail=tail;return true;
    }
}
static bool source_repl_result(SourceContext *ctx,AstNode *tail) {
    SourceValue initial={0};
    if(!expression(ctx,tail->as.expr_stmt,&initial))return false;
    if(initial.type==XR_XIR_UNIT)return true;
    if(ctx->repl_result || ctx->slot_count>=ctx->slot_capacity || !ctx->self_unit->serial)
        return source_fail(ctx,tail,XR_XIR_BAD_STRUCTURE,"implicit result has no unique declaration capacity");
    char *name=source_alloc(ctx,64,1);if(!name)return false;
    if(source_format(ctx,name,64,"$result%llu",(unsigned long long)ctx->self_unit->serial)!=XR_DIAG_OK)
        return source_fail(ctx,tail,XR_XIR_BUDGET,"implicit result name exhausted");
    SourceName *symbol=add_name(ctx,&ctx->names[ctx->module],name,tail);if(!symbol)return false;
    symbol->kind=SOURCE_SLOT;symbol->index=ctx->slot_count++;symbol->module=ctx->module;symbol->inferred=true;
    ctx->slots[symbol->index]=(XrXirSlot){ctx->module,XR_XIR_UNIT,0};
    if(!source_query_declare(ctx,symbol,XR_XIR_SOURCE_BINDING,0,source_query_range(ctx,tail,name)) ||
        !source_root_join_record(ctx,&initial) || !source_binding_prepare(ctx,symbol,&initial) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_SLOT_INIT,XR_XIR_UNIT,
            {initial.id,0},{0},symbol->index,{0}},NULL))return false;
    /* The generated declaration owns the real expression result. Its origin
     * remains the caller's exact AST node; no syntax is rendered or rewritten. */
    ctx->repl_result=symbol;return true;
}
