/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_native_calls.inc.c - Display facts from checked native operands
 */
typedef struct SourceNativeCallShape {
    XrXirType receiver;
    const XrXirType *parameters;
    uint32_t count;
} SourceNativeCallShape;
static bool source_native_call_shape(SourceContext *ctx,SourceNativeCallShape *shape,
    XrXirType receiver,const XrXirType *parameters,uint32_t count) {
    XrXirType *owned=count?source_alloc(ctx,count,sizeof(*owned)):NULL;
    if(count&&(!owned||!source_copy_bytes(ctx,NULL,owned,parameters,count*sizeof(*owned))))return false;
    *shape=(SourceNativeCallShape){receiver,owned,count};return true;
}
static bool source_array_call_signature(SourceContext *ctx,AstNode *node,
    const SourceNativeCallShape *shape,XrXirType result) {
    if(!node||node->type!=AST_CALL_EXPR)return true;
    XrXirSourceRange range=source_query_token_range(ctx,node);
    uint32_t selected=UINT32_MAX,target=0;
    for(uint32_t i=ctx->calls.count;i;i--) {
        if(!source_work(ctx,node))return false;
        XrXirSourceCall call=ctx->calls.items[i-1];
        if(call.reference>=ctx->query.reference_count||call.range.module!=range.module||call.range.line!=range.line||
            call.range.column!=range.column||call.range.end_line!=range.end_line||call.range.end_column!=range.end_column)continue;
        uint32_t id=ctx->query.references[call.reference].target;
        if(!id||id>ctx->query.declaration_count)continue;
        XrXirSourceDeclaration declaration=ctx->query.declarations[id-1];
        if(declaration.parent!=ctx->array_declaration||!declaration.native_identity)continue;
        if(call.signature.known)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"native call signature was already recorded");
        selected=i-1;target=id;break;
    }
    if(selected==UINT32_MAX)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"native call observation is missing");
    const XrNativeTypeDeclaration *array=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ARRAY);
    const XrNativeMemberDeclaration *member=NULL;
    uint32_t native_id=ctx->query.declarations[target-1].native_identity;
    for(uint32_t i=0;i<array->member_count;++i) {
        if(!source_work(ctx,node))return false;
        if(array->members[i].id==native_id){member=&array->members[i];break;}
    }
    if(!member||!member->is_method||member->operation==XR_NATIVE_OPERATION_NONE||shape->count!=member->parameter_count||
        !xr_xir_type_is_array(&ctx->types,shape->receiver))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"native call signature shape diverged");
    XrXirSourceType *parameters=shape->count?source_alloc(ctx,shape->count,sizeof(*parameters)):NULL;
    uint32_t *flags=shape->count?source_alloc(ctx,shape->count,sizeof(*flags)):NULL;
    if(shape->count&&(!parameters||!flags))return false;
    for(uint32_t p=0;p<shape->count;++p) {
        if(!source_work(ctx,node))return false;
        parameters[p]=source_query_type(ctx,shape->parameters[p]);
        flags[p]=(member->parameters[p].optional?XR_XIR_SOURCE_CALL_OPTIONAL:0)|
            (member->parameters[p].variadic?XR_XIR_SOURCE_CALL_VARIADIC:0);
    }
    XrXirSourceCallSignature signature={.parameters=parameters,.parameter_count=shape->count,
        .result=source_query_type(ctx,result),.known=true,.owner_type=source_query_type(ctx,shape->receiver),.parameter_flags=flags};
    if(!source_direct_signature_names(ctx,node,&signature))return false;
    XrXirSourceCall *call=(XrXirSourceCall *)&ctx->calls.items[selected];
    return source_copy_bytes(ctx,node,&call->signature,&signature,sizeof(signature));
}
