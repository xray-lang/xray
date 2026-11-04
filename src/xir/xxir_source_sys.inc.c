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
    for(uint32_t m=0;m<ctx->module_count;++m) {
        if(!source_request_poll(ctx,NULL))return false;
        if(!source_sys_module(ctx,m) || source_owned_checked(&ctx->graph->specs[m]))continue;
        ctx->module=m;
        for(uint32_t n=0;n<sizeof(xir_sys_native_ids)/sizeof(*xir_sys_native_ids);++n) {
            uint32_t id=xir_sys_native_ids[n];
            const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
            if(!native || !native->member)return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"sys native declaration is missing");
            uint32_t count=native->member->parameter_count;
            XrXirType *parameters=count?source_alloc(ctx,count,sizeof(*parameters)):NULL;
            if(count&&!parameters)return false;
            for(uint32_t p=0;p<count;++p) {
                XrNativeTypeTerm term=native->member->parameters[p].type;
                if(term==XR_NATIVE_TERM_I64)parameters[p]=XR_XIR_I64;
                else if(term==XR_NATIVE_TERM_BOOL)parameters[p]=XR_XIR_BOOL;
                else if(term==XR_NATIVE_TERM_STRING)parameters[p]=XR_XIR_STRING;
                else if(term==XR_NATIVE_TERM_PATH) {
                    if(!source_path_prelude_type(ctx,parameters+p))return false;
                }
                else if(term==XR_NATIVE_TERM_NULLABLE_I64||term==XR_NATIVE_TERM_NULLABLE_STRING||
                    term==XR_NATIVE_TERM_ARRAY_STRING||term==XR_NATIVE_TERM_NULLABLE_ARRAY_STRING) {
                    XrXirType element=term==XR_NATIVE_TERM_NULLABLE_I64?XR_XIR_I64:XR_XIR_STRING;
                    if(term==XR_NATIVE_TERM_ARRAY_STRING||term==XR_NATIVE_TERM_NULLABLE_ARRAY_STRING)
                        if(!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=element},&element))return false;
                    if(term!=XR_NATIVE_TERM_ARRAY_STRING)
                        if(!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=element},&element))return false;
                    parameters[p]=element;
                }
                else if(native->member->parameters[p].type==XR_NATIVE_TERM_ARRAY_U8) {
                    if(!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_U8},parameters+p))return false;
                }
                else if(native->member->parameters[p].type==XR_NATIVE_TERM_UNIT_CALLBACK) {
                    if(!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,
                        .result=XR_XIR_UNIT,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED},parameters+p))return false;
                } else return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"sys parameter requires its governed exact type");
            }
            XrXirType result=XR_XIR_UNIT;
            switch(native->member->result) {
            case XR_NATIVE_TERM_UNIT:break;
            case XR_NATIVE_TERM_BOOL:result=XR_XIR_BOOL;break;
            case XR_NATIVE_TERM_I64:result=XR_XIR_I64;break;
            case XR_NATIVE_TERM_STRING:result=XR_XIR_STRING;break;
            case XR_NATIVE_TERM_NULLABLE_PTR_U8:
                if(!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_PTR,.element=XR_XIR_U8},&result)||
                    !source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=result},&result))return false;
                break;
            case XR_NATIVE_TERM_NULLABLE_I64:
                if(!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64},&result))return false;
                break;
            case XR_NATIVE_TERM_NULLABLE_ARRAY_U8:
                if(!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_U8},&result) ||
                    !source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=result},&result))return false;
                break;
            case XR_NATIVE_TERM_NULLABLE_I64_PAIR: {
                XrXirCallableParameter *pair=source_alloc(ctx,2,sizeof(*pair));if(!pair)return false;
                pair[0]=(XrXirCallableParameter){.type=XR_XIR_I64};pair[1]=pair[0];
                if(!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=pair,.parameter_count=2},&result) ||
                    !source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=result},&result))return false;
                break;
            }
            default:return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"sys result requires its governed exact type");
            }
            if(!xir_sys_native_signature(id,&ctx->types,parameters,count,result,XR_XIR_NEVER))
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
                .promises=xir_sys_pipe_can_suspend(id)?0:XR_XIR_FUNCTION_NO_SUSPEND,.native_callable=id};
            ctx->bodies[index].module=m;ctx->bodies[index].parameters=parameters;
            if(!source_query_declare(ctx,symbol,XR_XIR_SOURCE_FUNCTION,0,(XrXirSourceRange){m,0,0,0,0}))return false;
            ((XrXirSourceDeclaration *)ctx->query.declarations)[symbol->declaration-1].signature=signature;
            ctx->bodies[index].declaration=symbol->declaration;ctx->function=index;
            if(id==XR_NATIVE_CALLABLE_SYS_DYLIB_OPEN) {
                XrXirDeclarations scope;XrXirModule module=source_module_view(ctx,&scope);uint32_t field=0;
                if(!xir_sys_dylib_path_scope(&scope,&ctx->types,index,ctx->function_count,parameters[0],&field)||
                    xr_xir_compile_nominal_access(&ctx->compile,&module,index,parameters[0],field,XR_XIR_NOMINAL_READ)!=XR_XIR_OK)
                    return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"sys loader requires a canonical public Path read");
            }
            if(!source_query_parameters(ctx,symbol->declaration))return false;
        }
    }
    ctx->module=saved_module;ctx->function=saved_function;return true;
}
