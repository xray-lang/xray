/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_native_query.inc.c - Open native schemas belong only to queries
 */
static bool source_native_query_types(SourceContext *ctx) {
    if(!ctx->array_declaration)return true;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ARRAY);
    bool needed=false;
    for(uint32_t m=0;m<native->member_count;++m) {
        if(!source_work(ctx,NULL))return false;
        const XrNativeMemberDeclaration *member=&native->members[m];
        if(!member->operation||!ctx->array_members[member->operation])continue;
        needed|=member->result==XR_NATIVE_TERM_ARRAY_ELEMENT;
        for(uint32_t p=0;p<member->parameter_count;++p) {
            if(!source_work(ctx,NULL))return false;
            needed|=member->parameters[p].type==XR_NATIVE_TERM_ARRAY_ELEMENT;
        }
    }
    if(!needed)return true;
    XrXirType schema=XR_XIR_UNIT;
    for(uint32_t i=0;i<ctx->types.count;++i) {
        if(!source_work(ctx,NULL))return false;
        const XrXirTypeNode *node=&ctx->types.nodes[i];
        if(node->kind==XR_XIR_TYPE_ARRAY&&node->element==(XrXirType)XR_XIR_TYPE_PARAMETER_BASE&&node->parameter_span==1) {
            schema=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i);break;
        }
    }
    if(schema==XR_XIR_UNIT) {
        if(ctx->types.count>=XR_XIR_CONSTRUCTED_TYPE_LIMIT-XR_XIR_CONSTRUCTED_TYPE_BASE)
            return source_fail(ctx,NULL,XR_XIR_BUDGET,"native query type identities exhausted");
        XrXirTypes *types=source_alloc(ctx,1,sizeof(*types));
        XrXirTypeNode *nodes=source_alloc(ctx,(size_t)ctx->types.count+1,sizeof(*nodes));
        if(!types||!nodes||!source_copy_bytes(ctx,NULL,nodes,ctx->types.nodes,ctx->types.count*sizeof(*nodes)))return false;
        *types=ctx->types;types->nodes=nodes;types->count=ctx->types.count+1;
        nodes[ctx->types.count]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,.parameter_span=1};
        schema=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+ctx->types.count);ctx->query.types=types;
    }
    XrXirSourceType array={schema,ctx->array_declaration,true};
    for(uint32_t m=0;m<native->member_count;++m) {
        if(!source_work(ctx,NULL))return false;
        const XrNativeMemberDeclaration *member=&native->members[m];
        if(!member->operation||!ctx->array_members[member->operation])continue;
        uint32_t id=ctx->array_members[member->operation];
        XrXirSourceDeclaration *declaration=(XrXirSourceDeclaration *)&ctx->query.declarations[id-1];
        if(member->result==XR_NATIVE_TERM_ARRAY_ELEMENT)declaration->type=array;
        uint32_t parameter=0;
        for(uint32_t i=0;i<ctx->query.declaration_count;++i) {
            if(!source_work(ctx,NULL))return false;
            XrXirSourceDeclaration *child=(XrXirSourceDeclaration *)&ctx->query.declarations[i];
            if(child->kind!=XR_XIR_SOURCE_PARAMETER||child->parent!=id)continue;
            if(parameter>=member->parameter_count)return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"native query parameter identity diverged");
            if(member->parameters[parameter].type==XR_NATIVE_TERM_ARRAY_ELEMENT) {
                ((XrXirSourceType *)declaration->parameters)[parameter]=array;child->type=array;
            }
            ++parameter;
        }
        if(parameter!=member->parameter_count)return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"native query parameters are incomplete");
    }
    return true;
}
