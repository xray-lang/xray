/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_task_scope.inc.c - Lexical task ownership on real cleanup frontiers
 *
 * KEY CONCEPT:
 *   The empty helper identifies an ordinary cleanup edge; it never awaits.
 */
typedef struct SourceScopeScan {SourceContext *ctx;uint32_t count,depth;} SourceScopeScan;
static bool source_scope_member(AstNode *node,void *pointer) {
    SourceScopeScan *scan=pointer;
    if(!node||!source_request_poll(scan->ctx,node))return node==NULL;
    if(node->type==AST_FUNCTION_DECL||node->type==AST_FUNCTION_EXPR||node->type==AST_METHOD_DECL||node->type==AST_SCOPE_BLOCK)return true;
    if(node->type==AST_GO_EXPR) {
        if(scan->count==UINT32_MAX)return source_fail(scan->ctx,node,XR_XIR_OVERFLOW,"scope member count overflow");
        ++scan->count;
    }
    if(scan->depth==128)return source_fail(scan->ctx,node,XR_XIR_BUDGET,"scope syntax depth exhausted");
    ++scan->depth;bool ok=xr_ast_for_each_child(node,source_scope_member,scan);--scan->depth;return ok;
}
static bool source_scope_cleanup(SourceContext *ctx,AstNode *node) {
    uint32_t outer=ctx->function;
    if(ctx->next_closure>=ctx->closure_limit)return source_fail(ctx,node,XR_XIR_BUDGET,"scope cleanup inventory exhausted");
    uint32_t index=ctx->next_closure++;
    SourceFunction *owner=ctx->bodies+outer,*body=ctx->bodies+index;
    body->node=node;body->type_owner=owner->type_owner;body->module=ctx->module;
    body->generic_owner=owner->generic_owner;body->type_parameters=owner->type_parameters;
    body->type_parameter_count=owner->type_parameter_count;ctx->generics[index]=ctx->generics[outer];
    /* Arguments belong to each body, even when its generic binders are shared. */
    ctx->generics[index].arguments=NULL;ctx->generics[index].argument_count=0;
    char *name=source_alloc(ctx,32,sizeof(*name));if(!name)return false;
    if(source_format(ctx,name,32,"$scope_exit%u",index)!=XR_DIAG_OK)return source_fail(ctx,node,XR_XIR_BUDGET,"scope cleanup name exhausted");
    SourceName declaration={0};declaration.name=name;declaration.node=node;
    if(!source_query_declare(ctx,&declaration,XR_XIR_SOURCE_FUNCTION,owner->declaration,source_query_range(ctx,node,NULL))||
        !source_query_syntax_role(ctx,declaration.declaration,XR_XIR_SOURCE_SYNTAX_NONE,0))return false;
    body->declaration=declaration.declaration;
    ctx->functions[index]=(XrXirFunction){name,(uint32_t)source_text_size(ctx,name),NULL,0,XR_XIR_UNIT,NULL,0,NULL,0,NULL,0,XR_XIR_NEVER,0,NULL};
    ctx->identities[index]=ctx->identities[outer];ctx->identities[index].promises=XR_XIR_FUNCTION_NO_SUSPEND;
    ctx->identities[index].test_role=XR_XIR_TEST_ROLE_NONE;ctx->identities[index].test_timeout_seconds=0;
    ctx->identities[index].method_kind=ctx->identities[index].nominal_owner?XR_XIR_MEMBER_HELPER:XR_XIR_NON_MEMBER;
    ctx->identities[index].exported=0;ctx->identities[index].cleanup_owner=outer+1;
    SourceComptimeScope saved=source_comptime_scope(ctx);SourceTaskScope *scope=ctx->task_scope;
    ctx->function=index;ctx->returned=false;ctx->task_scope=NULL;
    ctx->locals=ctx->scope=NULL;ctx->facts=NULL;ctx->epochs=NULL;ctx->loop=NULL;
    ctx->active_expression=ctx->effect_argument=NULL;ctx->depth=ctx->flow_alternatives=0;ctx->type_scope=(SourceTypeScope){0};
    bool ok=source_recipe_append(ctx,(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}},NULL);
    ctx->returned=true;if(ok)ok=finish_body(ctx);source_comptime_restore(ctx,&saved);ctx->task_scope=scope;
    if(!ok)return false;
    XrXirSourceDeclaration *query=(XrXirSourceDeclaration *)&ctx->query.declarations[body->declaration-1];
    query->type=source_query_type(ctx,XR_XIR_UNIT);query->generic_parent=body->generic_owner;
    query->generic_parent_count=body->type_parameter_count;query->generic_parameter_count=body->type_parameter_count;
    query->generic_constraints=ctx->generics[index].constraints;
    if(!owner->block_count&&!begin_block(ctx))return false;
    XrXirInstruction op={XR_XIR_CLEANUP_REGISTER,XR_XIR_UNIT,{0},{owner->block_count},index,{0}};
    uint32_t count=ctx->generics[index].parameter_count;
    XrXirType *types=count?source_alloc(ctx,count,sizeof(*types)):NULL;if(count&&!types)return false;
    for(uint32_t p=0;p<count;++p){if(!source_request_poll(ctx,node))return false;types[p]=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+p);}
    if(!source_type_arguments(ctx,node,types,count,&op)||!source_recipe_group(ctx,op,NULL,0,NULL))return false;
    owner->frontier=owner->count;return begin_block(ctx);
}
static bool source_task_scope(SourceContext *ctx,AstNode *node,bool marked) {
    ScopeBlockNode *scope=&node->as.scope_block;SourceFunction *body=ctx->bodies+ctx->function;
    if(!scope->body||scope->body->type!=AST_BLOCK||ctx->identities[ctx->function].cleanup_owner||
        (scope->scope_mode!=XR_SOURCE_SCOPE_NORMAL&&scope->scope_mode!=XR_SOURCE_SCOPE_ISOLATED))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"scope requires a real lexical block and supported mode");
    SourceScopeScan scan={ctx,0,0};if(!source_scope_member(scope->body,&scan))return false;
    if(!scan.count)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"scope has no direct go member; use an ordinary block");
    uint32_t frontier=body->frontier;SourceTaskScope group={ctx->task_scope,ctx->function,{0}};
    if(!source_scope_cleanup(ctx,node)||!source_task_type(ctx,node,XR_XIR_UNIT,XR_XIR_NEVER,&group.group.type)||
        !source_recipe_append(ctx,(XrXirInstruction){XR_XIR_SCOPE_BEGIN,group.group.type,{0},{0},scope->scope_mode,{0}},&group.group))return false;
    ctx->task_scope=&group;bool ok=statement(ctx,scope->body,false);ctx->task_scope=group.parent;
    (void)marked; /* Never has no implicit business propagation to mark. */
    if(ok&&!ctx->returned) {
        uint32_t error=body->block_count;
        if(error==UINT32_MAX)return source_fail(ctx,node,XR_XIR_OVERFLOW,"scope continuation overflow");
        uint32_t origin=body->count;SourceValue caught;
        ok=source_recipe_append(ctx,(XrXirInstruction){XR_XIR_SCOPE_JOIN,XR_XIR_UNIT,{group.group.id},{error+1,error},0,{0}},NULL)&&begin_block(ctx)&&
            source_recipe_append(ctx,(XrXirInstruction){XR_XIR_INVOKE_ERROR,XR_XIR_ERROR,{0},{0},origin,{0}},&caught)&&
            source_recipe_append(ctx,(XrXirInstruction){XR_XIR_THROW,XR_XIR_UNIT,{caught.id},{0},0,{0}},NULL)&&begin_block(ctx)&&
            source_recipe_append(ctx,(XrXirInstruction){XR_XIR_CLEANUP_LEAVE,XR_XIR_UNIT,{0},{body->block_count},frontier,{0}},NULL);
        body->frontier=frontier;if(ok)ok=begin_block(ctx);
    } else body->frontier=frontier;
    return ok;
}
