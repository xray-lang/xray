/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_path_native.inc.c - Private host facts owned by Path
 *
 * KEY CONCEPT:
 *   The host flavour, the textual absolute form and the current directory are
 *   declared only in the real standard Path module. Their exported values keep
 *   their Xray bodies; this file only supplies the governed entry points those
 *   bodies call.
 */
#include "xxir_path_native.h"
/* The resolver reports the standard module's authority namespace as `path`
 * and its logical path as `path/path.xr`; the published coordinates include
 * the module once more, so only the path part of them is compared here. */
static bool source_path_native_module(SourceContext *ctx,uint32_t module) {
    const XrModuleSpec *spec=ctx->graph->specs+module;char logical[64],canonical[160];
    if(!xir_path_prelude_coordinates(logical,canonical))return false;
    const char *slash=strchr(logical,'/');
    const char *module_path=slash?slash+1:logical;
    /* The bare `path` import and the quoted `std/path/path` import reach the
     * same file through two resolver routes; the logical path is the one fact
     * both carry, so the module is identified by it alone. */
    bool kind_ok=spec->authority.kind==XR_MODULE_IDENTITY_STDLIB||spec->authority.kind==XR_MODULE_IDENTITY_PACKAGE;
    bool same=spec->logical_path&&source_text_same(ctx,NULL,spec->logical_path,module_path);
    return kind_ok&&same;
}
static bool source_path_native_declare(SourceContext *ctx) {
    uint32_t saved_module=ctx->module,saved_function=ctx->function;
    for(uint32_t m=0;m<ctx->module_count;++m) {
        if(!source_request_poll(ctx,NULL))return false;
        if(!source_path_native_module(ctx,m)||source_owned_checked(&ctx->graph->specs[m]))continue;
        ctx->module=m;
        for(uint32_t item=0;item<sizeof(xir_path_native_ids)/sizeof(*xir_path_native_ids);++item) {
            uint32_t id=xir_path_native_ids[item];
            const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
            if(!native||!native->member||!native->member->name)
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Path host fact declaration is missing");
            const XrNativeMemberDeclaration *member=native->member;
            uint32_t count=member->parameter_count;
            XrXirType *parameters=count?source_alloc(ctx,count,sizeof(*parameters)):NULL;
            XrXirType result=member->result==XR_NATIVE_TERM_BOOL?XR_XIR_BOOL:XR_XIR_STRING;
            if(count&&!parameters)return false;
            for(uint32_t p=0;p<count;++p) {
                if(member->parameters[p].type==XR_NATIVE_TERM_STRING)parameters[p]=XR_XIR_STRING;
                else return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"Path host fact requires its governed operand type");
            }
            if(!xir_path_native_signature(id,parameters,count,result,XR_XIR_NEVER))
                return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"Path host fact differs from its governed contract");
            if(find_name(ctx,ctx->names[m],member->name))continue;
            if(ctx->function_count>=ctx->function_capacity)return source_fail(ctx,NULL,XR_XIR_BUDGET,"Path host fact inventory exhausted");
            const char *name=source_owned_text(ctx,member->name),*signature=source_owned_text(ctx,member->signature);
            if(!name||!signature)return false;
            SourceName *symbol=add_name(ctx,&ctx->names[m],name,NULL);if(!symbol)return false;
            uint32_t index=ctx->function_count++;symbol->kind=SOURCE_FUNCTION;symbol->index=index;symbol->module=m;
            ctx->functions[index]=(XrXirFunction){.name=name,.name_length=(uint32_t)strlen(name),
                .parameters=parameters,.parameter_count=count,.result=result,.error_type=XR_XIR_NEVER};
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
