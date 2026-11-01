/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_optional_chain.inc.c - Short-circuiting nullable field projections
 *
 * KEY CONCEPT:
 *   One evaluated receiver feeds ordinary typed field reads. Every absent
 *   receiver exits the entire suffix through one nullable result block.
 */
static bool source_optional_receiver_check(SourceContext *ctx,AstNode *site,
    XrXirType receiver,bool *unwrap) {
    const OptionalChainNode *link=&site->as.optional_chain;
    if(link->chain_type!=0 || !link->name || link->index)
        return source_fail(ctx,site,XR_XIR_BAD_TYPE,"optional index and call chains are not implemented");
    *unwrap=xr_xir_type_is_nullable(&ctx->types,receiver);
    if(*unwrap && link->implicit_link)
        return source_fail(ctx,site,XR_XIR_BAD_TYPE,"nullable field requires explicit optional access");
    return true;
}
static bool source_optional_chain(SourceContext *ctx,AstNode *node,SourceValue *value) {
    uint32_t count=0;AstNode *root=node;
    while(root && root->type==AST_OPTIONAL_CHAIN) {
        if(!(count%64) && !source_request_poll(ctx,root))return false;
        if(++count>=128)return source_fail(ctx,node,XR_XIR_BUDGET,"optional chain depth exhausted");
        if(root->as.optional_chain.chain_type!=0 || !root->as.optional_chain.name || root->as.optional_chain.index)
            return source_fail(ctx,root,XR_XIR_BAD_TYPE,"optional index and call chains are not implemented");
        root=root->as.optional_chain.object;
    }
    if(!root)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"optional chain has no receiver");
    AstNode **links=source_alloc(ctx,count,sizeof(*links));
    uint32_t *branches=source_alloc(ctx,count,sizeof(*branches));
    if(!links || !branches)return false;
    AstNode *link=node;
    for(uint32_t i=count;i;--i) {
        if(!(i%64) && !source_request_poll(ctx,link))return false;
        links[i-1]=link;link=link->as.optional_chain.object;
    }
    SourceValue current;
    if(!expression(ctx,root,&current))return false;
    SourceFunction *body=&ctx->bodies[ctx->function];uint32_t branch_count=0;
    for(uint32_t i=0;i<count;++i) {
        AstNode *site=links[i];bool unwrap=false;
        if(!(i%64) && !source_request_poll(ctx,site))return false;
        if(!source_optional_receiver_check(ctx,site,current.type,&unwrap))return false;
        if(unwrap) {
            SourceValue present;
            if(!source_nullable_value(ctx,site,(XrHirNullableInput){.role=XR_HIR_NULLABLE_UNEQUAL_NULL,
                .source=current.type},current.id,&present))return false;
            uint32_t branch=body->count,yes=body->block_count;
            if(!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_BRANCH,XR_XIR_UNIT,{present.id},{0},0,{0}},NULL) ||
                !begin_block(ctx))return false;
            body->recipes[branch].instruction.targets[0]=yes;branches[branch_count++]=branch;
            if(!source_unwrap_value(ctx,site,current,&current))return false;
        }
        uint32_t field=0;XrXirType type;
        if(!source_struct_field(ctx,site,current.type,site->as.optional_chain.name,false,&field,&type) ||
            !source_hir_nominal_get(ctx,site,current,field,0,&current))return false;
    }
    XrXirType result=current.type;
    if(!xr_xir_type_is_nullable(&ctx->types,result) && !source_nullable_type(ctx,result,&result))return false;
    XrHirConversion conversion;
    if(!source_conversion_plan(ctx,node,current.type,(SourceExpectedType){true,result,false,false},&conversion) ||
        !source_conversion_emit(ctx,&conversion,&current))return false;
    if(!branch_count){*value=current;return true;}
    uint32_t yes_end=body->current_block.identity,yes_jump=body->count;
    if(!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{0},0,{0}},NULL))return false;
    uint32_t absent=body->block_count;
    if(!begin_block(ctx))return false;
    SourceValue missing;
    if(!source_nullable_value(ctx,node,(XrHirNullableInput){.role=XR_HIR_NULLABLE_NULL,
        .has_expected=true,.expected=result},0,&missing))return false;
    uint32_t no_end=body->current_block.identity,no_jump=body->count;
    if(!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{0},0,{0}},NULL))return false;
    uint32_t join=body->block_count;
    if(!begin_block(ctx))return false;
    for(uint32_t i=0;i<branch_count;++i) {
        if(!(i%64) && !source_request_poll(ctx,node))return false;
        body->recipes[branches[i]].instruction.targets[1]=absent;
    }
    body->recipes[yes_jump].instruction.targets[0]=join;
    body->recipes[no_jump].instruction.targets[0]=join;
    SourceValue inputs[]={{.id=yes_end,.type=XR_XIR_UNIT},current,{.id=no_end,.type=XR_XIR_UNIT},missing};
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_PHI,result,{0},{0},0,{0}},inputs,4,value);
}
