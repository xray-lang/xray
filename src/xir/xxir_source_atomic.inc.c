/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_atomic.inc.c - Closed declaration proof and governed type queries
 */
static bool source_native_atomic_declaration(SourceContext *ctx) {
    if (ctx->atomic_declaration) return true;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ATOMIC);
    if (!source_native_admit(ctx,native)) return false;
    XrXirSourceQueryModule *modules=source_query_append(ctx,ctx->query.modules,
        &ctx->query.module_count,&ctx->query_module_capacity,sizeof(*modules));
    if (!modules) return false;
    ctx->query.modules=modules;uint32_t module=ctx->query.module_count-1;ctx->atomic_module=module;
    modules[module]=(XrXirSourceQueryModule){native->identity,native->source_path,native->source_fingerprint};
    SourceName symbol={0};symbol.name=source_owned_text(ctx,native->name);
    if (!symbol.name || !source_query_declare(ctx,&symbol,XR_XIR_SOURCE_TYPE,0,
        (XrXirSourceRange){module,(int)native->line,(int)native->column,(int)native->line,(int)native->column+6})) return false;
    ctx->atomic_declaration=symbol.declaration;
    XrXirConstraint *constraints=source_alloc(ctx,1,sizeof(*constraints));
    if (!constraints) return false;
    *constraints=(XrXirConstraint){XR_XIR_CONSTRAINT_ATOMIC_VALUE,NULL,0};
    XrXirSourceDeclaration *record=(XrXirSourceDeclaration *)&ctx->query.declarations[symbol.declaration-1];
    record->native_identity=native->id;record->exported=true;
    record->generic_parameter_count=1;record->generic_constraints=constraints;
    SourceName parameter={0};parameter.name=source_owned_text(ctx,native->parameter_name);
    if (!parameter.name || !source_query_declare(ctx,&parameter,XR_XIR_SOURCE_TYPE_PARAMETER,
        ctx->atomic_declaration,(XrXirSourceRange){module,0,0,0,0})) return false;
    record=(XrXirSourceDeclaration *)&ctx->query.declarations[parameter.declaration-1];
    record->type=(XrXirSourceType){(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,ctx->atomic_declaration,true};
    return true;
}
static bool source_native_atomic_type(SourceContext *ctx,XrTypeRef *ref,XrXirType *type) {
    const XrNativeTypeDeclaration *native=source_native_find(ctx,ref->name);
    if (!native || native->id!=XR_NATIVE_DECLARATION_ATOMIC || ref->nchildren!=1 || !ref->children ||
        source_native_type_shadowed(ctx,ref->name))
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"type name does not bind the governed Atomic declaration");
    if (ctx->depth>=128) return source_fail(ctx,NULL,XR_XIR_BUDGET,"source type depth exhausted");
    ++ctx->depth;XrXirType element;
    bool ok=source_type(ctx,ref->children[0],&element);--ctx->depth;
    if (!ok || !source_native_atomic_declaration(ctx) || !source_intern_type(ctx,
        (XrXirTypeNode){.kind=XR_XIR_TYPE_ATOMIC,.element=element},type)) return false;
    /* The module's authentic definition environment discharges this obligation
     * after all signatures and explicit conformances have been constructed. */
    XrXirSourceRange range={ctx->module,ref->line,ref->column,ref->line,ref->column};
    return source_query_target_reference(ctx,range,ctx->atomic_declaration,XR_XIR_SOURCE_TYPE_USE);
}
