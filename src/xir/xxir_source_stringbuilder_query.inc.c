/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_stringbuilder_query.inc.c - Source-backed logical text signatures
 */
static bool source_stringbuilder_reference(SourceContext *ctx,AstNode *site,
    const XrNativeMemberDeclaration *member,XrXirType result) {
    SourceName *owner=source_prelude_name(ctx,"StringBuilder");
    if(!owner || owner->kind!=SOURCE_NOMINAL) return source_fail(ctx,site,XR_XIR_BAD_STRUCTURE,"StringBuilder has no canonical declaration owner");
    uint32_t declaration=0;
    for(uint32_t i=0;i<ctx->query.declaration_count;++i) {
        if(!source_work(ctx,site)) return false;
        const XrXirSourceDeclaration *row=&ctx->query.declarations[i];
        if(row->kind==XR_XIR_SOURCE_MEMBER && row->parent==owner->declaration && row->native_identity==member->id) {declaration=row->id;break;}
    }
    if(!declaration) {
        const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_STRING_BUILDER);
        XrXirSourceQueryModule *modules=source_query_append(ctx,ctx->query.modules,&ctx->query.module_count,&ctx->query_module_capacity,sizeof(*modules));
        if(!modules) return false;
        ctx->query.modules=modules;uint32_t module=ctx->query.module_count-1;
        modules[module]=(XrXirSourceQueryModule){native->identity,native->source_path,native->source_fingerprint};
        SourceName symbol={0};symbol.name=source_owned_text(ctx,member->name);
        const char *signature=source_owned_text(ctx,member->signature);
        XrXirSourceType *parameters=member->parameter_count?source_alloc(ctx,1,sizeof(*parameters)):NULL;
        if(!symbol.name || !signature || (member->parameter_count && !parameters)) return false;
        /* The declaration preserves its closed union spelling. A concrete
         * argument is normalized separately; no fictitious union Type is made. */
        if(parameters) *parameters=(XrXirSourceType){XR_XIR_UNIT,0,false};
        XrXirSourceRange range={module,(int)member->line,(int)member->column,(int)member->line,
            (int)(member->column+source_text_size(ctx,member->name))};
        if(!source_query_declare(ctx,&symbol,XR_XIR_SOURCE_MEMBER,owner->declaration,range)) return false;
        declaration=symbol.declaration;
        XrXirSourceDeclaration *row=(XrXirSourceDeclaration *)&ctx->query.declarations[declaration-1];
        row->native_identity=member->id;row->signature=signature;row->exported=true;
        row->type=(XrXirSourceType){result,0,true};row->parameters=parameters;row->parameter_count=member->parameter_count;
    }
    return source_query_target_token_reference(ctx,site,declaration,XR_XIR_SOURCE_CALL);
}
