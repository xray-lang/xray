/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_sys.inc.c - Owned private syscall declaration in the standard sys module
 *
 * KEY CONCEPT:
 *   Source lexical declarations win; only the real standard module receives
 *   the governed OS-thread operations.
 */
#include "xxir_sys_native.h"
static bool source_sys_module(SourceContext *ctx,uint32_t module) {
    const XrModuleSpec *spec=&ctx->graph->specs[module];
    return spec->authority.kind==XR_MODULE_IDENTITY_STDLIB && spec->authority.namespace_id &&
        source_text_same(ctx,NULL,spec->authority.namespace_id,"sys") && spec->logical_path &&
        source_text_same(ctx,NULL,spec->logical_path,"sys/sys.xr");
}
static bool source_sys_declare(SourceContext *ctx) {
    uint32_t saved_module=ctx->module,saved_function=ctx->function;
    static const uint32_t ids[]={XR_NATIVE_CALLABLE_SYS_THREAD_YIELD,XR_NATIVE_CALLABLE_SYS_PIN_TO_CPU};
    for(uint32_t m=0;m<ctx->module_count;++m) {
        if(!source_request_poll(ctx,NULL))return false;
        if(!source_sys_module(ctx,m) || source_owned_checked(&ctx->graph->specs[m]))continue;
        ctx->module=m;
        for(uint32_t n=0;n<sizeof(ids)/sizeof(*ids);++n) {
            uint32_t id=ids[n],count=id==XR_NATIVE_CALLABLE_SYS_PIN_TO_CPU?1u:0u;
            XrXirType *parameters=count?source_alloc(ctx,count,sizeof(*parameters)):NULL;
            if(count&&!parameters)return false;
            if(count)parameters[0]=XR_XIR_I64;
            XrXirType result=count?XR_XIR_BOOL:XR_XIR_UNIT;
            const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
            if(!xir_sys_native_signature(id,parameters,count,result,XR_XIR_NEVER))
                return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"sys callable differs from its governed signature");
            if(find_name(ctx,ctx->names[m],native->member->name))continue;
            if(ctx->function_count>=ctx->function_capacity)
                return source_fail(ctx,NULL,XR_XIR_BUDGET,"sys native callable inventory exhausted");
            const char *name=source_owned_text(ctx,native->member->name);
            const char *signature=source_owned_text(ctx,native->member->signature);
            if(!name || !signature)return false;
            SourceName *symbol=add_name(ctx,&ctx->names[m],name,NULL);if(!symbol)return false;
            uint32_t index=ctx->function_count++;
            symbol->kind=SOURCE_FUNCTION;symbol->index=index;symbol->module=m;
            ctx->functions[index]=(XrXirFunction){.name=name,.name_length=(uint32_t)strlen(name),
                .parameters=parameters,.parameter_count=count,.result=result,.error_type=XR_XIR_NEVER};
            ctx->identities[index]=(XrXirFunctionIdentity){.module=m,
                .promises=XR_XIR_FUNCTION_NO_SUSPEND,.native_callable=id};
            ctx->bodies[index].module=m;ctx->bodies[index].parameters=parameters;
            if(!source_query_declare(ctx,symbol,XR_XIR_SOURCE_FUNCTION,0,(XrXirSourceRange){m,0,0,0,0}))return false;
            ((XrXirSourceDeclaration *)ctx->query.declarations)[symbol->declaration-1].signature=signature;
            ctx->bodies[index].declaration=symbol->declaration;ctx->function=index;
            if(!source_query_parameters(ctx,symbol->declaration))return false;
        }
    }
    ctx->module=saved_module;ctx->function=saved_function;return true;
}
