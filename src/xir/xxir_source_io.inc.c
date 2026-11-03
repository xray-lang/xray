/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_io.inc.c - Exact source-owned filesystem declarations
 *
 * KEY CONCEPT:
 *   Path remains an ordinary canonical class. Its type visibility cannot
 *   substitute for the declaring function's normal public field-read access.
 */
#include "xxir_io_native.h"
static bool source_io_module(SourceContext *ctx,uint32_t module) {
    const XrModuleSpec *spec=&ctx->graph->specs[module];
    return spec->authority.kind==XR_MODULE_IDENTITY_STDLIB && spec->authority.namespace_id &&
        source_text_same(ctx,NULL,spec->authority.namespace_id,"io") && spec->logical_path &&
        source_text_same(ctx,NULL,spec->logical_path,"io/io.xr");
}
static bool source_io_declare(SourceContext *ctx) {
    uint32_t saved_module=ctx->module,saved_function=ctx->function;
    static const uint32_t ids[]={XR_NATIVE_CALLABLE_IO_CHMOD,XR_NATIVE_CALLABLE_IO_CHDIR};
    for(uint32_t m=0;m<ctx->module_count;++m) {
        if(!source_request_poll(ctx,NULL))return false;
        if(!source_io_module(ctx,m) || source_owned_checked(&ctx->graph->specs[m]))continue;
        ctx->module=m;
        for(uint32_t n=0;n<sizeof(ids)/sizeof(*ids);++n) {
            uint32_t id=ids[n],count=id==XR_NATIVE_CALLABLE_IO_CHMOD?2u:1u;
            const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
            if(!native || !native->member || !native->member->name)
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"io callable declaration is missing");
            if(find_name(ctx,ctx->names[m],native->member->name))continue;
            if(ctx->function_count>=ctx->function_capacity)
                return source_fail(ctx,NULL,XR_XIR_BUDGET,"io native callable inventory exhausted");
            XrXirType path=XR_XIR_UNIT;
            if(!source_path_prelude_type(ctx,&path))return false;
            XrXirType *parameters=source_alloc(ctx,count,sizeof(*parameters));if(!parameters)return false;
            parameters[0]=path;if(count==2)parameters[1]=XR_XIR_I64;
            if(!xir_io_native_signature(id,&ctx->types,parameters,count,XR_XIR_BOOL,XR_XIR_NEVER))
                return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"io callable differs from its governed signature");
            const char *name=source_owned_text(ctx,native->member->name);
            const char *signature=source_owned_text(ctx,native->member->signature);if(!name || !signature)return false;
            SourceName *symbol=add_name(ctx,&ctx->names[m],name,NULL);if(!symbol)return false;
            uint32_t index=ctx->function_count++;
            symbol->kind=SOURCE_FUNCTION;symbol->index=index;symbol->module=m;
            ctx->functions[index]=(XrXirFunction){.name=name,.name_length=(uint32_t)strlen(name),
                .parameters=parameters,.parameter_count=count,.result=XR_XIR_BOOL,.error_type=XR_XIR_NEVER};
            ctx->identities[index]=(XrXirFunctionIdentity){.module=m,
                .promises=XR_XIR_FUNCTION_NO_SUSPEND,.native_callable=id};
            ctx->bodies[index].module=m;ctx->bodies[index].parameters=parameters;
            XrXirDeclarations declarations;XrXirModule module=source_module_view(ctx,&declarations);uint32_t field=0;
            if(!xir_io_native_scope(&declarations,&ctx->types,index,ctx->function_count,path,&field) ||
                xr_xir_compile_nominal_access(&ctx->compile,&module,index,path,field,XR_XIR_NOMINAL_READ)!=XR_XIR_OK)
                return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"io callable requires the real Path field read permission");
            if(!source_query_declare(ctx,symbol,XR_XIR_SOURCE_FUNCTION,0,(XrXirSourceRange){m,0,0,0,0}))return false;
            ((XrXirSourceDeclaration *)ctx->query.declarations)[symbol->declaration-1].signature=signature;
            ctx->bodies[index].declaration=symbol->declaration;ctx->function=index;
            if(!source_query_parameters(ctx,symbol->declaration))return false;
        }
    }
    ctx->module=saved_module;ctx->function=saved_function;return true;
}
