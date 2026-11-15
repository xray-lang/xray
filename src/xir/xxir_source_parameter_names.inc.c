/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Published namespaces are copied before their producer can retire. */
static bool source_parameter_names_copy(SourceContext *ctx,const XrXirParameterNames *row,
    uint32_t physical,XrXirLiteral **output) {
    if(row->parameter_offset>physical || row->count!=physical-row->parameter_offset)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library parameter namespace has the wrong arity");
    XrXirLiteral *names=physical?source_alloc(ctx,physical,sizeof(*names)):NULL;
    if(physical && !names)return false;
    for(uint32_t p=0;p<physical;++p)names[p]=(XrXirLiteral){0};
    for(uint32_t p=0;p<row->count;++p) {
        XrXirLiteral name=row->names[p];
        char *bytes=name.length?source_alloc(ctx,name.length,1):NULL;
        if((name.length && !bytes) || !source_copy_bytes(ctx,NULL,bytes,name.bytes,name.length))return false;
        names[p+row->parameter_offset]=(XrXirLiteral){bytes,name.length};
    }
    *output=names;return true;
}
static bool source_parameter_names_import_function(SourceContext *ctx,const XrXirModule *library,
    uint32_t original,uint32_t target) {
    const XrXirParameterNames *row=xr_xir_parameter_names_find(library->declarations,
        XR_XIR_PARAMETER_FUNCTION,original,0);
    if(!row)return true;
    XrXirLiteral *names=NULL;
    if(!source_parameter_names_copy(ctx,row,ctx->functions[target].parameter_count,&names))return false;
    ctx->bodies[target].call_parameter_names=names;
    ctx->bodies[target].call_parameter_namespace=true;return true;
}
static bool source_parameter_names_import_requirement(SourceContext *ctx,const XrXirModule *library,
    uint32_t original,uint32_t target,uint32_t member) {
    const XrXirParameterNames *row=xr_xir_parameter_names_find(library->declarations,
        XR_XIR_PARAMETER_REQUIREMENT,original,member);
    if(!row)return true;
    XrXirLiteral *names=NULL;
    if(!source_parameter_names_copy(ctx,row,row->count,&names))return false;
    if(target>=ctx->interfaces.count || member>=ctx->interfaces.declarations[target].method_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library parameter namespace owner is not mapped");
    if(ctx->imported_parameter_owner_count<ctx->interfaces.count) {
        SourceImportedParameterNames **owners=source_alloc(ctx,ctx->interfaces.count,sizeof(*owners));
        if(!owners || !source_copy_bytes(ctx,NULL,owners,ctx->imported_parameter_names,
            (size_t)ctx->imported_parameter_owner_count*sizeof(*owners)))return false;
        ctx->imported_parameter_names=owners;ctx->imported_parameter_owner_count=ctx->interfaces.count;
    }
    SourceImportedParameterNames *entries=ctx->imported_parameter_names[target];
    if(!entries) {
        entries=source_alloc(ctx,ctx->interfaces.declarations[target].method_count,sizeof(*entries));
        if(!entries)return false;
        ctx->imported_parameter_names[target]=entries;
    }
    SourceImportedParameterNames *entry=&entries[member];
    if(entry->present)return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library parameter namespace is duplicated");
    entry->row=*row;entry->row.owner=target;entry->row.names=names;
    entry->present=true;return true;
}
static bool source_parameter_names_finish(SourceContext *ctx) {
    uint64_t capacity=ctx->function_count;
    for(uint32_t i=0;i<ctx->interfaces.count;++i)capacity+=ctx->interfaces.declarations[i].method_count;
    if(capacity>ctx->compile.limits.instructions || capacity>UINT32_MAX)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"declaration parameter namespace inventory exhausted");
    XrXirParameterNames *rows=capacity?source_alloc(ctx,(uint32_t)capacity,sizeof(*rows)):NULL;
    if(capacity && !rows)return false;
    uint32_t count=0;
    for(uint32_t f=0;f<ctx->function_count;++f) {
        if(!source_request_poll(ctx,NULL))return false;
        SourceFunction body=ctx->bodies[f];
        XrXirFunctionIdentity identity=ctx->identities[f];
        AstNode *node=body.node;
        bool declared=node && (node->type==AST_FUNCTION_DECL || node->type==AST_METHOD_DECL);
        if(!body.call_parameter_namespace && (!declared || body.default_expression ||
            identity.method_kind==XR_XIR_MEMBER_HELPER || identity.method_kind==XR_XIR_MATERIALIZER ||
            identity.method_kind==XR_XIR_CONSTRUCTOR_FIELDS || identity.cleanup_owner))continue;
        uint32_t offset=identity.capture_count+(identity.method_kind==XR_XIR_READ_METHOD?1u:0u);
        uint32_t physical=ctx->functions[f].parameter_count;
        if(offset>physical)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"declaration namespace prefix exceeds physical arity");
        XrHirSignatureInput input={.family=XR_HIR_CALL_FUNCTION,.function=f,.count=physical,.parameter_offset=offset};
        XrHirCallDeclarationNames names={0};
        XrXirStatus status=source_hir_call_names(ctx,&input,&names);
        if(status!=XR_XIR_OK)return source_fail(ctx,node,status,"declaration parameter namespace cannot be published");
        rows[count++]=(XrXirParameterNames){XR_XIR_PARAMETER_FUNCTION,f,0,offset,
            physical>offset?names.parameters+offset:NULL,physical-offset};
    }
    for(uint32_t i=0;i<ctx->interfaces.count;++i) {
        const XrXirInterfaceDeclaration *owner=&ctx->interfaces.declarations[i];
        for(uint32_t m=0;m<owner->method_count;++m) {
            if(!source_request_poll(ctx,NULL))return false;
            const XrXirTypeNode *type=xr_xir_callable_signature(&ctx->types,owner->methods[m].signature);
            if(!type)return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"interface namespace has no callable signature");
            bool imported=ctx->interface_sources[i] && ctx->interface_sources[i]->checked_library;
            if(imported) {
                SourceImportedParameterNames *entries=i<ctx->imported_parameter_owner_count?
                    ctx->imported_parameter_names[i]:NULL;
                if(!entries || !entries[m].present)continue;
            }
            XrHirSignatureInput input={.family=XR_HIR_CALL_REQUIREMENT,.interface_declaration=i,
                .interface_member=m,.count=type->parameter_count};
            XrHirCallDeclarationNames names={0};
            XrXirStatus status=source_hir_call_names(ctx,&input,&names);
            if(status!=XR_XIR_OK)return source_fail(ctx,NULL,status,"interface parameter namespace cannot be published");
            rows[count++]=(XrXirParameterNames){XR_XIR_PARAMETER_REQUIREMENT,i,m,0,
                names.count?names.parameters:NULL,names.count};
        }
    }
    ctx->parameter_names=count?rows:NULL;ctx->parameter_name_count=count;return true;
}
