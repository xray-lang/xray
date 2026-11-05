/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_type_facts_publish.inc.c - Observe the source owner's actual type identities
 */
static bool source_type_fact(SourceContext *ctx,XrXirSourceTypeFact *facts,const SourceName *name,
    XrXirSourceTypeKind kind,uint32_t index) {
    if(!((ctx)->diagnostic.status == XR_XIR_OK))return false;
    if(!name||!name->declaration)return true;
    if(name->declaration>ctx->query.declaration_count||
        ctx->query.declarations[name->declaration-1].kind!=XR_XIR_SOURCE_TYPE)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source type declaration identity is invalid");
    XrXirSourceTypeFact *fact=&facts[name->declaration-1];
    if(fact->kind!=XR_XIR_SOURCE_TYPE_NONE)return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source type identity is duplicated");
    XrXirSourceTypeFact value={kind,index,{0},false};const AstNode *node=name->node;
    if(node&&(node->type==AST_CLASS_DECL||node->type==AST_STRUCT_DECL||node->type==AST_ENUM_DECL||node->type==AST_INTERFACE_DECL)){
        XrXirSourceRange range={name->module,node->line,node->column,node->end_line,node->end_column};
        const XrXirSourceDeclarationSyntax *syntax=&ctx->syntax.declarations[name->declaration-1];
        if(range.line>0&&range.column>0&&range.end_line>=range.line&&range.end_column>0&&
            (range.end_line>range.line||range.end_column>range.column)&&syntax->role==XR_XIR_SOURCE_SYNTAX_NAME){
            value.range=range;value.range_known=true;}}
    return source_copy_bytes(ctx,NULL,fact,&value,sizeof(value));
}
static XrXirStatus source_type_facts_publish(SourceContext *ctx,XrXirSourceSnapshot *snapshot) {
    uint32_t count=ctx->query.declaration_count;
    XrXirSourceTypeFact *facts=count?source_alloc(ctx,count,sizeof(*facts)):NULL;
    if(count&&!facts)return ctx->diagnostic.status;
    for(uint32_t i=0;i<ctx->nominals.count;++i){
        uint32_t kind=ctx->nominals.declarations[i].kind;
        XrXirSourceTypeKind type=kind==XR_XIR_NOMINAL_STRUCT?XR_XIR_SOURCE_TYPE_STRUCT:
            kind==XR_XIR_NOMINAL_ENUM?XR_XIR_SOURCE_TYPE_ENUM:kind==XR_XIR_NOMINAL_CLASS?XR_XIR_SOURCE_TYPE_CLASS:XR_XIR_SOURCE_TYPE_NONE;
        if(type==XR_XIR_SOURCE_TYPE_NONE)return XR_XIR_BAD_STRUCTURE;
        if(!source_type_fact(ctx,facts,ctx->nominal_sources[i],type,i))return ctx->diagnostic.status;
    }
    for(uint32_t i=0;i<ctx->interfaces.count;++i)
        if(!source_type_fact(ctx,facts,ctx->interface_sources[i],XR_XIR_SOURCE_TYPE_INTERFACE,i))return ctx->diagnostic.status;
    XrXirSourceTypeFacts observations={facts,count};
    return xr_xir_compile_source_snapshot_type_facts_copy(snapshot,&observations);
}
