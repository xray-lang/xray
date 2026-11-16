/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * A JSON repair is offered only for an available namespace and real coercions. */
static AstNode *source_collection_binding_site(SourceContext *ctx,AstNode *node) {
    AstNode *binding=ctx->binding_site,*initializer=binding?binding->as.var_decl.initializer:NULL;
    while(initializer && initializer->type==AST_GROUPING)initializer=initializer->as.grouping;
    return binding && initializer==node?binding:node;
}
static bool source_collection_json_hint(SourceContext *ctx,AstNode *node,
    const XrHirCollectionInput *input,bool *output) {
    *output=false;
    if(input->kind!=XR_HIR_COLLECTION_MAP)return true;
    for(uint32_t n=0;n<input->count;++n) {
        if(!source_request_poll(ctx,node))return false;
        XrHirCollection entry={0};XrHirCollectionError error={0};
        XrXirStatus status=xr_hir_collection_entry_types(&ctx->types,input->kind,&input->entries[n],&entry,&error);
        if(status==XR_XIR_BAD_TYPE)return true;
        if(status!=XR_XIR_OK)return source_fail(ctx,node,status,"JSON repair lost its typed entry");
        if(entry.key!=XR_XIR_STRING ||
            (entry.value!=XR_XIR_JSON_VALUE && !xr_xir_json_implicit_scalar(entry.value)))return true;
        if(input->entries[n].spread && entry.value!=XR_XIR_JSON_VALUE &&
            !source_collection_spread_retargetable(ctx,node,true,n))return ctx->diagnostic.status==XR_XIR_OK;
    }
    bool available=source_json_namespace_available(ctx);
    if(ctx->diagnostic.status!=XR_XIR_OK)return false;
    if(available) {
        SourceCollectionAliasLookup lookup={.ctx=ctx,.site=node,.name="JSON"};
        if(!source_collection_alias_at(ctx->graph->specs[ctx->module].ast,&lookup))return false;
        if(!lookup.found)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"JSON repair lost its lexical site");
        available=!lookup.shadowed;
    }
    *output=available;return true;
}
static bool source_collection_json_text(SourceContext *ctx,XrValueFormatSink sink,AstNode *node) {
    AstNode *binding=source_collection_binding_site(ctx,node);
    const char *name=binding!=node?binding->as.var_decl.name:"value";
    return xr_value_format_text(sink,"for JSON data, declare the type: var ") &&
        xr_value_format_text(sink,name) && xr_value_format_text(sink,": JSON.Object = {...}");
}
