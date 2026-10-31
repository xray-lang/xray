/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_recursive_binding.inc.c - Read-only local initializer self references
 *
 * KEY CONCEPT:
 *   A fixed local callable can rebuild itself from its real captures. Reassigned
 *   or borrowed bindings retain their ordinary shared-Cell admission boundary.
 */
typedef struct SourceRecursiveBindingScan {
    SourceContext *ctx;
    AstNode *binding;
    const char *name;
    uint32_t depth;
    bool readonly, referenced;
} SourceRecursiveBindingScan;
static bool source_recursive_binding_scan(AstNode *node,void *opaque) {
    SourceRecursiveBindingScan *scan=opaque;
    if(!node || !scan->readonly)return true;
    if(scan->depth>=128){scan->readonly=false;return true;}
    const char *written=NULL;
    switch(node->type) {
    case AST_VARIABLE:
        if(source_text_same(scan->ctx,node,node->as.variable.name,scan->name))scan->referenced=true;
        break;
    case AST_ASSIGNMENT:written=node->as.assignment.name;break;
    case AST_COMPOUND_ASSIGNMENT:written=node->as.compound_assignment.name;break;
    case AST_INC:written=node->as.inc.name;break;
    case AST_DEC:written=node->as.dec.name;break;
    case AST_VAR_DECL:case AST_CONST_DECL:
        if(node!=scan->binding)written=node->as.var_decl.name;
        break;
    case AST_DESTRUCTURE_ASSIGN:case AST_DESTRUCTURE_DECL:
    case AST_MOVE_EXPR:case AST_UNSAFE_EXPR:case AST_MATCH_EXPR:
    case AST_TRY_CATCH:case AST_FOR_IN_STMT:
        scan->readonly=false;return true;
    case AST_FUNCTION_DECL:case AST_FUNCTION_EXPR: {
        FunctionDeclNode *decl=&node->as.function_expr;
        if(node->type==AST_FUNCTION_DECL)written=decl->name;
        for(int i=0;i<decl->param_count;++i)
            if(source_text_same(scan->ctx,node,decl->params[i]->name,scan->name))scan->readonly=false;
        break;
    }
    case AST_MEMBER_ACCESS:
        if(node->as.member_access.object && node->as.member_access.object->type==AST_VARIABLE)
            written=node->as.member_access.object->as.variable.name;
        break;
    case AST_CALL_EXPR:
        for(int i=0;i<node->as.call_expr.arg_count;++i)
            if(node->as.call_expr.arg_accesses && node->as.call_expr.arg_accesses[i]!=XR_CALL_ARG_PLAIN)
                scan->readonly=false;
        break;
    default:break;
    }
    if(written && source_text_same(scan->ctx,node,written,scan->name))scan->readonly=false;
    if(scan->ctx->diagnostic.status!=XR_XIR_OK)return false;
    ++scan->depth;bool known=xr_ast_for_each_child(node,source_recursive_binding_scan,scan);--scan->depth;
    if(!known)scan->readonly=false;
    return scan->ctx->diagnostic.status==XR_XIR_OK;
}
static bool source_recursive_binding_readonly(SourceContext *ctx,AstNode *binding) {
    AstNode *initial=binding->as.var_decl.initializer,*owner=ctx->bodies[ctx->function].node;
    if(!initial || initial->type!=AST_FUNCTION_EXPR || !initial->as.function_expr.return_type ||
        !owner || (owner->type!=AST_FUNCTION_EXPR && owner->type!=AST_FUNCTION_DECL) ||
        visible_name(ctx,binding->as.var_decl.name))return false;
    SourceRecursiveBindingScan scan={ctx,binding,binding->as.var_decl.name,0,true,false};
    if(!source_recursive_binding_scan(initial,&scan) || !scan.readonly || !scan.referenced)return false;
    return source_recursive_binding_scan(owner,&scan) && scan.readonly;
}
