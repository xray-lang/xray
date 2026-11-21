/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Checked collection operands receive an owning HIR use before emission. */
static bool source_collection_construct(SourceContext *ctx,AstNode *node,const XrHirCollectionInput *literal,
    XrXirType type,const SourceValue *values,uint32_t count,uint32_t ordinal,SourceValue *out) {
    XrHirCollectionValue *operands=count?source_alloc(ctx,count,sizeof(*operands)):NULL;
    if(count && !operands)return false;
    for(uint32_t n=0;n<count;++n) {
        if(!(n%64) && !source_request_poll(ctx,node))return false;
        operands[n]=(XrHirCollectionValue){values[n].type,values[n].id};
    }
    XrHirBodyScope scope={0};if(!source_hir_current_scope(ctx,&scope))return false;
    bool map=literal->kind==XR_HIR_COLLECTION_MAP;
    if(map && ordinal>=ctx->map_literal_count)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Map construction has no actual provenance");
    XrHirCollectionConstructInput input={.compile=&ctx->compile,.types=&ctx->types,.scope=scope,
        .origin=source_hir_origin(ctx,node,0),.literal=*literal,.values=operands,.count=count,.ordinal=ordinal,
        .result=type,.provenance=map?ctx->map_literals+ordinal:NULL};
    XrHirBodyUse use={0};XrHirCollectionError error={0};
    XrXirStatus status=xr_hir_body_collection_construct(ctx->typed_body,&input,ctx->request,&use,&error);
    if(status!=XR_XIR_OK)return source_fail(ctx,node,status,"collection construction differs from its checked operands");
    uint32_t before=ctx->bodies[ctx->function].count;
    XrXirOp operation=map?XR_XIR_MAP_LITERAL:XR_XIR_ARRAY_NEW;
    if(!map && xr_xir_type_is_fixed_array(&ctx->types,type))operation=XR_XIR_FIXED_ARRAY_NEW;
    if(!source_recipe_group(ctx,(XrXirInstruction){operation,type,{0},{0},ordinal,{0}},values,count,out))return false;
    return source_hir_binding_emitted(ctx,&(XrHirBindingAction){.use=use},out,before);
}
