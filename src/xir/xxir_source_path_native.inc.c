/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_path_native.inc.c - Current-directory declaration owned by Path
 */
#include "xxir_path_native.h"
static bool source_path_native_module(SourceContext *ctx,uint32_t module) {
    const XrModuleSpec *spec=ctx->graph->specs+module;char logical[64],canonical[160];
    return xir_path_prelude_coordinates(logical,canonical)&&spec->authority.kind==XR_MODULE_IDENTITY_STDLIB&&
        spec->authority.namespace_id&&source_text_same(ctx,NULL,spec->authority.namespace_id,xir_path_prelude_module())&&
        spec->logical_path&&source_text_same(ctx,NULL,spec->logical_path,logical);
}
static bool source_path_native_declare(SourceContext *ctx) {
    uint32_t saved_module=ctx->module,saved_function=ctx->function;
    for(uint32_t m=0;m<ctx->module_count;++m) {
        if(!source_request_poll(ctx,NULL))return false;
        if(!source_path_native_module(ctx,m)||source_owned_checked(&ctx->graph->specs[m]))continue;
        static const uint32_t ids[]={XR_NATIVE_CALLABLE_PATH_GETCWD,XR_NATIVE_CALLABLE_PATH_IS_WINDOWS,XR_NATIVE_CALLABLE_PATH_FULLPATH};
        ctx->module=m;
        for(uint32_t n=0;n<3;++n) {
            uint32_t id=ids[n];bool full=id==XR_NATIVE_CALLABLE_PATH_FULLPATH;
            XrXirType result=id==XR_NATIVE_CALLABLE_PATH_IS_WINDOWS?XR_XIR_BOOL:XR_XIR_STRING;
            XrXirType *parameters=full?source_alloc(ctx,1,sizeof(*parameters)):NULL;
            if(full&&!parameters)return false;
            if(full)parameters[0]=XR_XIR_STRING;
            const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
            if(!native||!native->member||!xir_path_native_signature(id,parameters,full?1u:0u,result,XR_XIR_NEVER))
                return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"Path query differs from its governed contract");
            if(find_name(ctx,ctx->names[m],native->member->name))continue;
            if(ctx->function_count>=ctx->function_capacity)return source_fail(ctx,NULL,XR_XIR_BUDGET,"Path query inventory exhausted");
            const char *name=source_owned_text(ctx,native->member->name),*signature=source_owned_text(ctx,native->member->signature);
            if(!name||!signature)return false;
            SourceName *symbol=add_name(ctx,&ctx->names[m],name,NULL);if(!symbol)return false;
            uint32_t index=ctx->function_count++;symbol->kind=SOURCE_FUNCTION;symbol->index=index;symbol->module=m;
            ctx->functions[index]=(XrXirFunction){.name=name,.name_length=(uint32_t)strlen(name),.parameters=parameters,
                .parameter_count=full?1u:0u,.result=result,.error_type=XR_XIR_NEVER};
            ctx->identities[index]=(XrXirFunctionIdentity){.module=m,.promises=XR_XIR_FUNCTION_NO_SUSPEND,.native_callable=id};
            ctx->bodies[index].module=m;ctx->bodies[index].parameters=parameters;ctx->function=index;
            if(!source_query_declare(ctx,symbol,XR_XIR_SOURCE_FUNCTION,0,(XrXirSourceRange){m,0,0,0,0}))return false;
            ((XrXirSourceDeclaration *)ctx->query.declarations)[symbol->declaration-1].signature=signature;
            ctx->bodies[index].declaration=symbol->declaration;
            if(!source_query_parameters(ctx,symbol->declaration))return false;
        }
    }
    ctx->module=saved_module;ctx->function=saved_function;return true;
}
