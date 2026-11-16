/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Union repairs use the same finite type admission as explicit annotations. */
#include "xxir_source_collection_scope_hint.inc.c"
typedef struct SourceCollectionUnionHint {
    XrXirDataUnion members;
    XrXirType key;
    bool present;
} SourceCollectionUnionHint;
static bool source_collection_key_spelling_available(SourceContext *ctx,AstNode *site,XrXirType type,uint32_t depth) {
    if(!source_request_poll(ctx,site))return false;
    if(depth>=128)return source_fail(ctx,site,XR_XIR_BUDGET,"collection key spelling depth exhausted");
    if(source_scalar_type_name(type))return true;
    uint32_t id=(uint32_t)type;
    if(id>=XR_XIR_TYPE_PARAMETER_BASE && id<XR_XIR_TYPE_PARAMETER_LIMIT) {
        int count=0;source_type_parameters(ctx,&count);
        return id-XR_XIR_TYPE_PARAMETER_BASE<(uint32_t)count;
    }
    const XrXirTypeNode *borrow=xr_xir_type_node(&ctx->types,type);
    if(!borrow || borrow->kind!=XR_XIR_TYPE_NOMINAL)return false;
    XrXirTypeNode current=*borrow;
    if(current.nominal.declaration>=ctx->nominals.count)return false;
    XrXirLiteral literal=ctx->nominals.declarations[current.nominal.declaration].name;
    char *name=source_alloc(ctx,(size_t)literal.length+1,1);
    if(!name || !source_copy_bytes(ctx,site,name,literal.bytes,literal.length))return false;
    name[literal.length]=0;
    SourceName *resolved=source_nominal_name(ctx,name);
    if(ctx->diagnostic.status!=XR_XIR_OK)return false;
    if(!resolved || resolved->index!=current.nominal.declaration)return false;
    SourceCollectionAliasLookup lookup={.ctx=ctx,.site=site,.name=name};
    if(!source_collection_alias_at(ctx->graph->specs[ctx->module].ast,&lookup))return false;
    if(!lookup.found || lookup.shadowed)return false;
    for(uint32_t n=0;n<current.nominal.argument_count;++n)
        if(!source_collection_key_spelling_available(ctx,site,current.nominal.arguments[n],depth+1))return false;
    return true;
}
static bool source_collection_union_hint(SourceContext *ctx,AstNode *node,
    const XrHirCollectionInput *input,SourceCollectionUnionHint *out) {
    *out=(SourceCollectionUnionHint){0};
    bool constructor_visible=false;
    if(!source_collection_union_constructor_visible(ctx,node,input->kind==XR_HIR_COLLECTION_MAP,&constructor_visible))return false;
    if(!constructor_visible)return true;
    XrXirType *values=source_alloc(ctx,input->count,sizeof(*values));if(!values)return false;
    bool map=input->kind==XR_HIR_COLLECTION_MAP;XrXirType key=XR_XIR_UNIT;
    for(uint32_t n=0;n<input->count;++n) {
        if(!source_request_poll(ctx,node))return false;
        XrHirCollection entry={0};XrHirCollectionError error={0};
        XrXirStatus projected=xr_hir_collection_entry_types(&ctx->types,input->kind,&input->entries[n],&entry,&error);
        if(projected==XR_XIR_BAD_TYPE)return true;
        if(projected!=XR_XIR_OK)return source_fail(ctx,node,projected,"collection repair lost its typed entry");
        if(map && n && entry.key!=key)return true;
        key=entry.key;values[n]=entry.value;
    }
    XrXirDataUnion normalized={0};
    XrXirStatus status=xr_xir_compile_data_union(&ctx->compile,&ctx->types,values,input->count,ctx->request,&normalized);
    if(status==XR_XIR_BAD_TYPE)return true;
    if(status!=XR_XIR_OK)return source_fail(ctx,node,status,"collection union repair admission failed");
    if(normalized.count<2)return true;
    if(map) {
        XrXirDeclarations declarations;XrXirModule module=source_module_view(ctx,&declarations);
        XrXirProofContext proof={&module,{XR_XIR_CONTEXT_FUNCTION,ctx->function,0}};
        status=xir_compile_type_markers_prove_controlled(&ctx->compile,&proof,key,XR_XIR_CONSTRAINT_HASHABLE,ctx->request);
        if(status==XR_XIR_BAD_TYPE)return true;
        if(status!=XR_XIR_OK)return source_fail(ctx,node,status,"collection union repair key proof failed");
        if(!source_collection_key_spelling_available(ctx,node,key,0))return ctx->diagnostic.status==XR_XIR_OK;
    }
    for(uint32_t n=0;n<input->count;++n)if(input->entries[n].spread) {
        const XrXirTypeNode *type=xr_xir_type_node(&ctx->types,values[n]);
        bool exact=type && type->kind==XR_XIR_TYPE_DATA_UNION && type->parameter_count==normalized.count;
        for(uint32_t p=0;exact && p<normalized.count;++p)exact=type->parameters[p].type==normalized.members[p];
        if(!exact && !source_collection_spread_retargetable(ctx,node,map,n))
            return ctx->diagnostic.status==XR_XIR_OK;
    }
    SourceCollectionUnionHint result={.key=key,.present=true};
    for(uint32_t n=0;n<input->count;++n) {
        if(!source_request_poll(ctx,node))return false;
        const XrXirTypeNode *type=xr_xir_type_node(&ctx->types,values[n]);
        uint32_t count=type?type->parameter_count:1;
        for(uint32_t p=0;p<count;++p) {
            XrXirType member=type?type->parameters[p].type:values[n];uint32_t at=0;
            while(at<result.members.count&&result.members.members[at]!=member)++at;
            if(at==result.members.count) {
                if(at>=XR_XIR_DATA_UNION_MAX_MEMBERS)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"collection repair lost its admitted union");
                result.members.members[result.members.count++]=member;
            }
        }
    }
    *out=result;return true;
}
static bool source_collection_union_text(SourceContext *ctx,XrValueFormatSink sink,
    bool map,SourceCollectionUnionHint hint) {
    if(!xr_value_format_text(sink,map?"to keep both types in one Map, declare Map<":"to keep both types in one Array, declare Array<"))return false;
    if(map && (!source_collection_type_text(ctx,sink,hint.key,0)||!xr_value_format_text(sink,", ")))return false;
    for(uint32_t n=0;n<hint.members.count;++n)
        if((n&&!xr_value_format_text(sink," | "))||!source_collection_type_text(ctx,sink,hint.members.members[n],0))return false;
    return xr_value_format_text(sink,">")!=0;
}
