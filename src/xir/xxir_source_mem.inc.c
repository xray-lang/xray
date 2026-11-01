/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_mem.inc.c - Owned memory declarations in their actual Source scope
 *
 * KEY CONCEPT:
 *   The source graph selects the private declarations. Full physical checking
 *   separately validates each callable and every receiver-root return body.
 */
static const uint32_t source_mem_callables[]={
    XR_NATIVE_CALLABLE_BUFFER_ALLOCATE,XR_NATIVE_CALLABLE_BUFFER_ALLOCATE_ZEROED,
    XR_NATIVE_CALLABLE_BUFFER_ALLOCATE_ALIGNED,XR_NATIVE_CALLABLE_BUFFER_LENGTH,
    XR_NATIVE_CALLABLE_BUFFER_AS_BYTES,XR_NATIVE_CALLABLE_BUFFER_BORROW_PTR,
    XR_NATIVE_CALLABLE_BUFFER_RESIZE,XR_NATIVE_CALLABLE_MEM_FENCE,XR_NATIVE_CALLABLE_MEM_PREFETCH,
    XR_NATIVE_CALLABLE_MEM_CACHE_FLUSH,XR_NATIVE_CALLABLE_MEM_NONTEMPORAL_STORE,
    XR_NATIVE_CALLABLE_MEM_CACHE_LINE_SIZE,XR_NATIVE_CALLABLE_MEM_PAGE_ALLOCATE,
    XR_NATIVE_CALLABLE_MEM_PAGE_PROTECT,XR_NATIVE_CALLABLE_MEM_PAGE_FREE,XR_NATIVE_CALLABLE_MEM_MOVE,
    XR_NATIVE_CALLABLE_MEM_VOLATILE_LOAD,XR_NATIVE_CALLABLE_MEM_VOLATILE_STORE
};
static bool source_mem_module(SourceContext *ctx,uint32_t module) {
    if (module>=(uint32_t)ctx->graph->spec_count) return false;
    const XrModuleSpec *spec=&ctx->graph->specs[module];
    return spec->authority.kind==XR_MODULE_IDENTITY_STDLIB && spec->authority.namespace_id &&
        source_text_same(ctx,NULL,spec->authority.namespace_id,"mem") && spec->logical_path &&
        source_text_same(ctx,NULL,spec->logical_path,"mem/mem.xr");
}
static bool source_mem_buffer(SourceContext *ctx,uint32_t ordinal) {
    if (ordinal>=ctx->nominals.count || !ctx->nominal_sources[ordinal]) return false;
    SourceName *owner=ctx->nominal_sources[ordinal];
    const XrXirNominalDeclaration *decl=&ctx->nominals.declarations[ordinal];
    return source_mem_module(ctx,owner->module) && decl->kind==XR_XIR_NOMINAL_CLASS &&
        decl->flags==XR_XIR_NOMINAL_FINAL && decl->exported && !decl->native.native_id &&
        !decl->parameter_count && decl->field_count==1 && decl->fields &&
        decl->fields[0].type==XR_XIR_BUFFER_STORAGE && (decl->fields[0].flags&XR_XIR_FIELD_PRIVATE) &&
        source_text_same(ctx,NULL,owner->name,"Buffer");
}
static bool source_mem_method_contract(SourceContext *ctx,SourceName *owner,const MethodDeclNode *method) {
    if (!source_mem_buffer(ctx,owner->index) || method->is_static || method->param_count ||
        method->receiver_mode!=XR_PARAM_READ) return false;
    if (method->is_operator)
        return !method->borrow_origin_syntax && !method->borrow_origin_count &&
            method->op_type==OPTYPE_LEN && source_text_same(ctx,NULL,method->name,"__operator_len");
    return method->borrow_origin_syntax==XR_BORROW_ORIGIN_EXPLICIT_SET && method->borrow_origin_count==1 &&
        method->borrow_origins && method->borrow_origins[0].kind==AST_BORROW_ORIGIN_RECEIVER &&
        source_text_same(ctx,NULL,method->name,"asBytes");
}
static bool source_mem_term(SourceContext *ctx,XrNativeTypeTerm term,XrXirType *output) {
    switch (term) {
    case XR_NATIVE_TERM_UNIT:*output=XR_XIR_UNIT;return true;
    case XR_NATIVE_TERM_I64:*output=XR_XIR_I64;return true;
    case XR_NATIVE_TERM_BOOL:*output=XR_XIR_BOOL;return true;
    case XR_NATIVE_TERM_BUFFER_STORAGE:*output=XR_XIR_BUFFER_STORAGE;return true;
    case XR_NATIVE_TERM_SLICE_U8:case XR_NATIVE_TERM_PTR_U8:case XR_NATIVE_TERM_MUT_PTR_U8:
        return source_intern_type(ctx,(XrXirTypeNode){.kind=term==XR_NATIVE_TERM_SLICE_U8?XR_XIR_TYPE_SLICE:
            term==XR_NATIVE_TERM_PTR_U8?XR_XIR_TYPE_PTR:XR_XIR_TYPE_MUT_PTR,.element=XR_XIR_U8},output);
    default:return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"memory callable has an unsupported physical term");
    }
}
static bool source_mem_declare(SourceContext *ctx) {
    uint32_t saved_module=ctx->module,saved_function=ctx->function;
    for (uint32_t m=0;m<ctx->module_count;++m) {
        if (!(m%64u) && !source_request_poll(ctx,NULL)) return false;
        if (!source_mem_module(ctx,m) || source_owned_checked(&ctx->graph->specs[m])) continue;
        ctx->module=m;
        for (uint32_t u=0;u<sizeof(source_mem_callables)/sizeof(*source_mem_callables);++u) {
            const XrNativeCallableDeclaration *native=xr_native_callable_by_id(source_mem_callables[u]);
            if (!native || !native->member || native->owner || !native->namespace_id ||
                !source_text_same(ctx,NULL,native->namespace_id,"mem"))
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"memory callable declaration is inconsistent");
            if (find_name(ctx,ctx->names[m],native->member->name)) continue;
            if (ctx->function_count>=ctx->function_capacity)
                return source_fail(ctx,NULL,XR_XIR_BUDGET,"memory callable inventory exhausted");
            const char *name=source_owned_text(ctx,native->member->name);
            const char *signature=source_owned_text(ctx,native->member->signature);
            uint32_t count=native->member->parameter_count;
            XrXirType *parameters=count?source_alloc(ctx,count,sizeof(*parameters)):NULL,result=XR_XIR_UNIT;
            if (!name || !signature || (count && !parameters) || !source_mem_term(ctx,native->member->result,&result)) return false;
            for (uint32_t p=0;p<count;++p)
                if (!source_mem_term(ctx,native->member->parameters[p].type,&parameters[p])) return false;
            SourceName *symbol=add_name(ctx,&ctx->names[m],name,NULL);if (!symbol) return false;
            uint32_t index=ctx->function_count++;symbol->kind=SOURCE_FUNCTION;symbol->index=index;symbol->module=m;
            ctx->functions[index]=(XrXirFunction){.name=name,.name_length=(uint32_t)source_text_size(ctx,name),
                .parameters=parameters,.parameter_count=count,.result=result,.error_type=XR_XIR_NEVER};
            ctx->identities[index]=(XrXirFunctionIdentity){.module=m,.promises=XR_XIR_FUNCTION_NO_SUSPEND,.native_callable=native->id};
            ctx->bodies[index].module=m;ctx->bodies[index].parameters=parameters;ctx->function=index;
            if (!source_query_declare(ctx,symbol,XR_XIR_SOURCE_FUNCTION,0,(XrXirSourceRange){m,0,0,0,0})) return false;
            ((XrXirSourceDeclaration *)ctx->query.declarations)[symbol->declaration-1].signature=signature;
            ctx->bodies[index].declaration=symbol->declaration;
            if (!source_query_parameters(ctx,symbol->declaration)) return false;
        }
    }
    ctx->module=saved_module;ctx->function=saved_function;return true;
}
