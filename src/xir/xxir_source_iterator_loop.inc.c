/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_iterator_loop.inc.c - Lexical Iterator loops and authenticated cleanup
 */
static bool source_iterator_cleanup(SourceContext *ctx,AstNode *node,SourceValue iterator) {
    if(ctx->function_count>=ctx->function_capacity || ctx->function_count>=ctx->compile.limits.functions)
        return source_fail(ctx,node,XR_XIR_BUDGET,"Iterator cleanup helper capacity exhausted");
    uint32_t outer=ctx->function,index=ctx->function_count;
    SourceFunction *owner=&ctx->bodies[outer],*body=&ctx->bodies[index];
    XrXirType *parameter=source_alloc(ctx,1,sizeof(*parameter));char *name=source_alloc(ctx,40,1);
    if(!parameter || !name) return false;
    *parameter=iterator.type;
    if(source_format(ctx,name,40,"$iterator_cleanup%u",index)!=XR_DIAG_OK)
        return source_fail(ctx,node,XR_XIR_BUDGET,"Iterator cleanup name exhausted");
    SourceName declaration={0};declaration.name=name;declaration.node=node;
    if(!source_query_declare(ctx,&declaration,XR_XIR_SOURCE_FUNCTION,owner->declaration,
        source_query_range(ctx,node,NULL))) return false;
    body->node=node;body->module=owner->module;body->parameters=parameter;body->type_owner=owner->type_owner;
    body->generic_owner=owner->generic_owner;body->type_parameters=owner->type_parameters;
    body->type_parameter_count=owner->type_parameter_count;body->declaration=declaration.declaration;
    ctx->functions[index]=(XrXirFunction){name,(uint32_t)source_text_size(ctx,name),parameter,1,XR_XIR_UNIT,NULL,0,NULL,0,NULL,0, XR_XIR_NEVER};
    ctx->generics[index].parameter_count=ctx->generics[outer].parameter_count;
    ctx->generics[index].constraints=ctx->generics[outer].constraints;
    ctx->identities[index]=ctx->identities[outer];
    ctx->identities[index].promises=0;ctx->identities[index].cleanup_owner=outer+1;
    ctx->identities[index].exported=0;ctx->identities[index].test_role=XR_XIR_TEST_ROLE_NONE;
    ctx->identities[index].test_timeout_seconds=0;
    ctx->identities[index].method_kind=ctx->identities[index].nominal_owner ? XR_XIR_MEMBER_HELPER : XR_XIR_NON_MEMBER;
    ++ctx->function_count;
    bool returned=ctx->returned;ctx->function=index;ctx->returned=false;SourceValue ignored;
    bool ok=source_iterator_consume(ctx,node,(SourceValue){0,iterator.type},(SourceValue){0},XR_XIR_ITERATOR_CLOSE,&ignored) &&
        source_recipe_append(ctx,(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}},NULL);
    if(ok) {ctx->returned=true;ok=finish_body(ctx);}
    if(ok) {
        XrXirSourceDeclaration *record=(XrXirSourceDeclaration *)&ctx->query.declarations[body->declaration-1];
        record->type=source_query_type(ctx,XR_XIR_UNIT);record->generic_parent=body->generic_owner;
        record->generic_parent_count=body->type_parameter_count;
        record->generic_parameter_count=ctx->generics[index].parameter_count;
        record->generic_constraints=ctx->generics[index].constraints;
    }
    ctx->function=outer;ctx->returned=returned;if(!ok) return false;
    uint32_t count=ctx->generics[index].parameter_count;
    XrXirType *types=count ? source_alloc(ctx,count,sizeof(*types)) : NULL;
    if(count && !types) return false;
    for(uint32_t p=0;p<count;++p) types[p]=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+p);
    if(!owner->block_count && !begin_block(ctx)) return false;
    XrXirInstruction op={XR_XIR_CLEANUP_REGISTER,XR_XIR_UNIT,{0},{owner->block_count},index,{0}};
    if(!source_type_arguments(ctx,node,types,count,&op) || !source_recipe_group(ctx,op,&iterator,1,NULL)) return false;
    owner->frontier=owner->count;return begin_block(ctx);
}
static bool source_iterator_loop(SourceContext *ctx,AstNode *node,SourceValue iterator) {
    if(node->as.for_in_stmt.is_keyvalue)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Iterator for-in binds its single produced value");
    SourceFunction *body=&ctx->bodies[ctx->function];uint32_t frontier=body->frontier;
    if(!source_iterator_cleanup(ctx,node,iterator)) return false;
    uint32_t head=body->block_count;
    if(!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{head},0,{0}},NULL) || !begin_block(ctx)) return false;
    SourceValue more;
    if(!source_iterator_consume(ctx,node,iterator,(SourceValue){0},XR_XIR_ITERATOR_HAS_NEXT,&more)) return false;
    uint32_t branch=body->count,entry=body->block_count;
    if(!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_BRANCH,XR_XIR_UNIT,{more.id},{entry},0,{0}},NULL) || !begin_block(ctx)) return false;
    SourceValue element;
    if(!source_iterator_consume(ctx,node,iterator,(SourceValue){0},XR_XIR_ITERATOR_NEXT,&element)) return false;
    SourceLoop loop={ctx->loop,NULL,NULL,body->frontier};ctx->loop=&loop;
    bool ok=source_for_in_body(ctx,node,(SourceValue){0},element);ctx->loop=loop.parent;
    if(!ok || (!ctx->returned && !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{head},0,{0}},NULL)) ||
        !patch_exits(ctx,loop.continues,head)) return false;
    uint32_t exit=body->block_count;if(!begin_block(ctx)) return false;
    body->recipes[branch].instruction.targets[1]=exit;
    if(!patch_exits(ctx,loop.breaks,exit) || !source_recipe_record(ctx,
        (XrXirInstruction){XR_XIR_CLEANUP_LEAVE,XR_XIR_UNIT,{0},{body->block_count},frontier,{0}},NULL)) return false;
    body->frontier=frontier;return begin_block(ctx);
}
