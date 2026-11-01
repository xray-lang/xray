/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_unsafe.inc.c - Lexically scoped raw-memory expressions
 *
 * KEY CONCEPT:
 *   Unsafe permits explicit raw access inside the current function only.
 *   Ordinary types, readonly access, cleanup and origin checks remain active.
 */
static bool source_unsafe_expression(SourceContext *ctx,AstNode *node,
    SourceExpectedType expected,SourceValue *value) {
    AstNode *block=node->as.unsafe_expr.operand;
    if (!block || block->type!=AST_BLOCK)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"unsafe requires a lexical block");
    if (ctx->depth>=128 || ctx->bodies[ctx->function].unsafe_depth==UINT32_MAX)
        return source_fail(ctx,node,XR_XIR_BUDGET,"unsafe expression depth exhausted");
    uint32_t function=ctx->function,frontier=ctx->bodies[function].frontier;
    SourceName *locals=ctx->locals,*scope=ctx->scope;XrHirBodyScope typed={0};
    if (!source_hir_scope_enter(ctx,block,&typed)) return false;
    ctx->scope=locals;++ctx->depth;++ctx->bodies[function].lexical_depth;
    ++ctx->bodies[function].unsafe_depth;
    bool ok=source_block_functions(ctx,block);*value=(SourceValue){.type=XR_XIR_UNIT};
    for (int i=0;ok && i<block->as.block.count;++i) {
        AstNode *part=block->as.block.statements[i];
        bool tail=i+1==block->as.block.count && part->type==AST_EXPR_STMT;
        ok=tail?source_plan_expression(ctx,part->as.expr_stmt,expected,value):statement(ctx,part,false);
    }
    --ctx->bodies[function].unsafe_depth;--ctx->bodies[function].lexical_depth;--ctx->depth;
    ctx->locals=locals;ctx->scope=scope;source_hir_scope_leave(ctx,typed);
    if (ok && !ctx->returned && ctx->bodies[function].frontier!=frontier) {
        ok=source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_CLEANUP_LEAVE,
            .targets={ctx->bodies[function].block_count,0},.immediate=frontier},NULL);
        ctx->bodies[function].frontier=frontier;if (ok) ok=begin_block(ctx);
    } else ctx->bodies[function].frontier=frontier;
    return ok;
}
