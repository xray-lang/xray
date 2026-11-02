/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_os.inc.c - Owned host-query declarations in the standard os module
 *
 * KEY CONCEPT:
 *   Source lexical declarations win; only the real standard module receives
 *   the governed host-query leaves.
 */
#include "xxir_os_native.h"
static bool source_os_module(SourceContext *ctx,uint32_t module) {
    const XrModuleSpec *spec=&ctx->graph->specs[module];
    return spec->authority.kind==XR_MODULE_IDENTITY_STDLIB && spec->authority.namespace_id &&
        source_text_same(ctx,NULL,spec->authority.namespace_id,"os") && spec->logical_path &&
        source_text_same(ctx,NULL,spec->logical_path,"os/os.xr");
}
static bool source_os_declare(SourceContext *ctx) {
    uint32_t saved_module=ctx->module,saved_function=ctx->function;
    for(uint32_t m=0;m<ctx->module_count;++m) {
        if(!source_request_poll(ctx,NULL))return false;
        if(!source_os_module(ctx,m) || source_owned_checked(&ctx->graph->specs[m]))continue;
        ctx->module=m;
        for(uint32_t id=XR_NATIVE_CALLABLE_OS_PLATFORM;id<=XR_NATIVE_CALLABLE_OS_GETPID;++id) {
            const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
            if(!native || !native->member || !native->member->name)
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"os callable declaration is missing");
            if(find_name(ctx,ctx->names[m],native->member->name))continue;
            if(ctx->function_count>=ctx->function_capacity)
                return source_fail(ctx,NULL,XR_XIR_BUDGET,"os native callable inventory exhausted");
            const char *name=source_owned_text(ctx,native->member->name);
            const char *signature=source_owned_text(ctx,native->member->signature);
            uint32_t count=id==XR_NATIVE_CALLABLE_OS_SETENV?2u:(id==XR_NATIVE_CALLABLE_OS_GETENV||id==XR_NATIVE_CALLABLE_OS_UNSETENV||id==XR_NATIVE_CALLABLE_OS_EXIT)?1u:0u;
            XrXirType *parameters=count?source_alloc(ctx,count,sizeof(*parameters)):NULL,result=XR_XIR_STRING;
            if(!name || !signature || (count && !parameters))return false;
            for(uint32_t p=0;p<count;++p)parameters[p]=id==XR_NATIVE_CALLABLE_OS_EXIT?XR_XIR_I64:XR_XIR_STRING;
            if(id==XR_NATIVE_CALLABLE_OS_SETENV||id==XR_NATIVE_CALLABLE_OS_UNSETENV)result=XR_XIR_BOOL;
            if(id==XR_NATIVE_CALLABLE_OS_EXIT)result=XR_XIR_UNIT;
            if(id==XR_NATIVE_CALLABLE_OS_GETPID)result=XR_XIR_I64;
            if(id==XR_NATIVE_CALLABLE_OS_GETENV) {
                if(!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_STRING},&result))return false;
            }
            if(id==XR_NATIVE_CALLABLE_OS_ENVIRON_BLOCK) {
                if(!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_STRING},&result))return false;
            }
            if(!xir_os_native_signature(id,&ctx->types,parameters,count,result,XR_XIR_NEVER))
                return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"os callable differs from its governed signature");
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
