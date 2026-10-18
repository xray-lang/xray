/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_statements.inc.c - Executable statements and their source sites
 */
#include "xxir_source_throw.inc.c"
static bool source_statement_body(SourceContext *ctx, AstNode *node, bool top) {
    if (!node || !source_work(ctx, node)) return false;
    if (ctx->returned) return source_unreachable_statement(ctx, node);
    switch (node->type) {
    case AST_DEFER_STMT: return source_defer(ctx, node);
    case AST_TRY_CATCH: {
        /* Handlers are alternative paths: no fact about a binding they write survives, and none is learned inside. */
        ++ctx->flow_alternatives;
        bool tried = source_facts_kill(ctx, node) && source_try(ctx, node);
        --ctx->flow_alternatives;
        return tried && source_facts_kill(ctx, node);
    }
    case AST_IF_STMT: return source_if(ctx, node);
    case AST_WHILE_STMT:
        if (node->as.while_stmt.label) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "labelled loops are not admitted");
        return source_loop(ctx, node, node->as.while_stmt.condition, node->as.while_stmt.body, NULL);
    case AST_FOR_STMT: return source_for(ctx, node);
    case AST_FOR_IN_STMT: {
        ++ctx->flow_alternatives;
        bool iterated = source_facts_kill(ctx, node) && source_for_in(ctx, node);
        --ctx->flow_alternatives;
        return iterated && source_facts_kill(ctx, node);
    }
    case AST_INC: case AST_DEC: return source_increment(ctx, node);
    case AST_BREAK_STMT: case AST_CONTINUE_STMT: return source_loop_exit(ctx, node);
    case AST_FUNCTION_DECL: return top ? source_repl_function_binding(ctx,node) : source_nested_function(ctx,node);
    case AST_TYPE_ALIAS: return true;
    case AST_IMPORT_STMT: case AST_STRUCT_DECL: case AST_CLASS_DECL: case AST_ENUM_DECL: case AST_INTERFACE_DECL:
        return top || source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "nested declarations are not admitted");
    case AST_VAR_DECL: case AST_CONST_DECL: return source_binding(ctx, node, top);
    case AST_DESTRUCTURE_DECL: return source_destructure_binding(ctx,node,top);
    case AST_EXPR_STMT: {
        if(top && node==ctx->repl_tail)return source_repl_result(ctx,node);
        SourceValue value; AstNode *expr=node->as.expr_stmt;
        bool appended = false;
        if (!source_string_append_statement(ctx, expr, &appended)) return false;
        if (appended) return true;
        if (expr->type!=AST_MATCH_EXPR) return expression(ctx,expr,&value);
        SourceExpressionPlan *plan=source_plan_collect(ctx,expr,(SourceExpectedType){false,XR_XIR_UNIT, false, false});
        if (!plan) return false;
        plan->statement_match=true;
        return source_plan_complete(ctx,plan,&value);
    }
    case AST_THROW_STMT: return source_throw(ctx,node);
    case AST_RETURN_STMT: {
        if (ctx->identities[ctx->function].cleanup_owner)
            return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "E0395: defer cannot return from its owning function");
        if (source_constructor_active(ctx)) return source_constructor_return(ctx, node);
        if (top || !ctx->bodies[ctx->function].node || node->as.return_stmt.value_count > 1)
            return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "invalid return placement or arity");
        SourceValue value = {0};
        SourceFunction *body = &ctx->bodies[ctx->function];
        XrXirType expected = body->infer_result ? XR_XIR_UNIT : ctx->functions[ctx->function].result;
        if (node->as.return_stmt.value_count && !source_plan_expression(ctx, node->as.return_stmt.values[0], (SourceExpectedType){!body->infer_result,expected, false, false}, &value)) return false;
        if (body->infer_result && !body->saw_return) ctx->functions[ctx->function].result = value.type;
        body->saw_return = true;
        if (value.type != ctx->functions[ctx->function].result)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "return type does not match declaration");
        ctx->returned = true;
        return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {value.type == XR_XIR_UNIT ? 0 : value.id, 0}, {0, 0}, 0, {0}}, NULL);
    }
    case AST_BLOCK: {
        if (ctx->depth >= 128) return source_fail(ctx, node, XR_XIR_BUDGET, "source block depth exhausted");
        SourceName *saved = ctx->locals, *scope = ctx->scope;
        SourceFunction *body = &ctx->bodies[ctx->function]; uint32_t entry = body->frontier;
        ctx->scope = saved; ++ctx->depth; ++body->lexical_depth;
        bool ok = source_block_functions(ctx, node);
        for (int i = 0; ok && i < node->as.block.count; ++i)
            ok = statement(ctx, node->as.block.statements[i], false);
        --ctx->depth; --body->lexical_depth; ctx->locals = saved; ctx->scope = scope;
        if (ok && !ctx->returned && body->frontier != entry) {
            ok = source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CLEANUP_LEAVE, XR_XIR_UNIT, {0}, {body->block_count}, entry, {0}}, NULL);
            body->frontier = entry;
            if (ok) ok = begin_block(ctx);
        } else body->frontier = entry;
        return ok;
    }
    default: return source_fail_syntax(ctx, node, "statement");
    }
}
static bool statement(SourceContext *ctx, AstNode *node, bool top) {
    XrXirSourceDebugPoint saved=ctx->debug_point;
    uint32_t function=ctx->debug_function;
    bool entry=ctx->debug_entry,jump_entry=ctx->debug_jump_entry;
    ctx->debug_point=(XrXirSourceDebugPoint){0};ctx->debug_function=ctx->function;
    ctx->debug_entry=false;ctx->debug_jump_entry=false;
    XrXirSourceRange range=source_query_range(ctx,node,NULL);
    if (node && node->type!=AST_BLOCK && node->type!=AST_TRY_CATCH && range.line>0 && range.column>0) {
        if (node->node_id==UINT32_MAX || !source_work_units(ctx,node,sizeof(ctx->debug_point))) {
            ctx->debug_point=saved;ctx->debug_function=function;
            ctx->debug_entry=entry;ctx->debug_jump_entry=jump_entry;
            return source_fail(ctx,node,XR_XIR_BUDGET,"source statement identity exhausted");
        }
        ctx->debug_point=(XrXirSourceDebugPoint){range,node->node_id+1,0,0};
        ctx->debug_entry=true;
        ctx->debug_jump_entry=node->type==AST_BREAK_STMT || node->type==AST_CONTINUE_STMT || node->type==AST_THROW_STMT;
    }
    bool ok=source_statement_body(ctx,node,top);
    ctx->debug_point=saved;ctx->debug_function=function;
    ctx->debug_entry=entry;ctx->debug_jump_entry=jump_entry;return ok;
}
