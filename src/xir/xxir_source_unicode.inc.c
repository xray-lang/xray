/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_unicode.inc.c - Authenticated private Unicode source declarations
 *
 * KEY CONCEPT:
 *   Only the owning standard module receives generated callable names.
 *   Ordinary declarations keep lexical resolution and one shared Items binding.
 */
static const uint32_t source_unicode_callables[]={
    XR_NATIVE_CALLABLE_UNICODE_PROPERTY_ID,XR_NATIVE_CALLABLE_UNICODE_HAS_PROPERTY
};
static bool source_unicode_module(SourceContext *ctx,uint32_t module) {
    const XrModuleSpec *spec=&ctx->graph->specs[module];
    return spec->authority.kind==XR_MODULE_IDENTITY_STDLIB && spec->authority.namespace_id &&
        source_text_same(ctx,NULL,spec->authority.namespace_id,"regex") && spec->logical_path &&
        source_text_same(ctx,NULL,spec->logical_path,"regex/regex.xr");
}
static bool source_unicode_declare(SourceContext *ctx) {
    uint32_t saved_module=ctx->module,saved_function=ctx->function;
    for (uint32_t m=0;m<ctx->module_count;++m) {
        if (!source_unicode_module(ctx,m) || source_owned_checked(&ctx->graph->specs[m])) continue;
        ctx->module=m;
        for (size_t u=0;u<sizeof(source_unicode_callables)/sizeof(*source_unicode_callables);++u) {
            const XrNativeCallableDeclaration *native=xr_native_callable_by_id(source_unicode_callables[u]);
            if (!native || !native->member || native->owner || !native->namespace_id ||
                !source_text_same(ctx,NULL,native->namespace_id,"regex"))
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Unicode callable authority is inconsistent");
            /* A real declaration keeps normal lexical shadowing and its own body. */
            if (find_name(ctx,ctx->names[m],native->member->name)) continue;
            if (ctx->function_count>=ctx->function_capacity)
                return source_fail(ctx,NULL,XR_XIR_BUDGET,"native callable inventory exhausted");
            const char *name=source_owned_text(ctx,native->member->name);
            const char *signature=source_owned_text(ctx,native->member->signature);
            uint32_t count=native->member->parameter_count;
            XrXirType *parameter=source_alloc(ctx,count,sizeof(*parameter));
            if (!name || !signature || !parameter) return false;
            for (uint32_t p=0;p<count;++p) parameter[p]=native->id==XR_NATIVE_CALLABLE_UNICODE_PROPERTY_ID ? XR_XIR_STRING : XR_XIR_I64;
            SourceName *symbol=add_name(ctx,&ctx->names[m],name,NULL);
            if (!symbol) return false;
            uint32_t index=ctx->function_count++;
            symbol->kind=SOURCE_FUNCTION;symbol->index=index;symbol->module=m;
            ctx->functions[index]=(XrXirFunction){.name=name,.name_length=(uint32_t)strlen(name),
                .parameters=parameter,.parameter_count=count,.result=native->id==XR_NATIVE_CALLABLE_UNICODE_PROPERTY_ID ? XR_XIR_I64 : XR_XIR_BOOL,.error_type=XR_XIR_NEVER};
            ctx->identities[index]=(XrXirFunctionIdentity){.module=m,.promises=XR_XIR_FUNCTION_NO_SUSPEND,
                .native_callable=native->id};
            ctx->bodies[index].module=m;ctx->bodies[index].parameters=parameter;
            if (!source_query_declare(ctx,symbol,XR_XIR_SOURCE_FUNCTION,0,(XrXirSourceRange){m,0,0,0,0})) return false;
            ((XrXirSourceDeclaration *)ctx->query.declarations)[symbol->declaration-1].signature=signature;
            ctx->bodies[index].declaration=symbol->declaration;ctx->function=index;
            if (!source_query_parameters(ctx,symbol->declaration)) return false;
        }
    }
    ctx->module=saved_module;ctx->function=saved_function;return true;
}
