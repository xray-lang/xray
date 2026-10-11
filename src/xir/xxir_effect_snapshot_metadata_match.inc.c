/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Full final owned metadata correspondence. No address or digest proves a field.
 */
static XrXirStatus effect_body_bytes_match(const XrXirCompileContext *work,
    const void *left,const void *right,uint64_t count,size_t size) {
    if (!size || count>SIZE_MAX/size || count*size==UINT64_MAX ||
        (count && (!left || !right))) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,count*size+1)) return XR_XIR_BUDGET;
    return count && memcmp(left,right,(size_t)count*size)?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
}

static XrXirStatus effect_body_application_match(const XrXirCompileContext *work,
    const XrXirInterfaceApplication *left,const XrXirInterfaceApplication *right) {
    if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
    if (left->declaration!=right->declaration || left->argument_count!=right->argument_count)
        return XR_XIR_BAD_STRUCTURE;
    return effect_body_bytes_match(work,left->arguments,right->arguments,
        left->argument_count,sizeof(*left->arguments));
}

static XrXirStatus effect_body_implementations_match(const XrXirCompileContext *work,
    const XrXirImplementationTable *left,const XrXirImplementationTable *right) {
    if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
    if (!!left!=!!right) return XR_XIR_BAD_STRUCTURE;
    if (!left) return XR_XIR_OK;
    if (left->count!=right->count || (left->count && (!left->records || !right->records)))
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    for (uint32_t i=0;i<left->count && status==XR_XIR_OK;++i) {
        const XrXirImplementation *a=&left->records[i],*b=&right->records[i];
        if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
        if (a->nominal_declaration!=b->nominal_declaration || a->binding_count!=b->binding_count ||
            (a->binding_count && (!a->bindings || !b->bindings))) return XR_XIR_BAD_STRUCTURE;
        status=effect_body_application_match(work,&a->interface,&b->interface);
        for (uint32_t m=0;m<a->binding_count && status==XR_XIR_OK;++m) {
            const XrXirImplementationBinding *x=&a->bindings[m],*y=&b->bindings[m];
            if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            if (x->member!=y->member || x->function!=y->function) return XR_XIR_BAD_STRUCTURE;
            status=effect_body_application_match(work,&x->requirement,&y->requirement);
        }
    }
    return status;
}

static XrXirStatus effect_body_declarations_match(const XrXirCompileContext *work,
    const XrXirDeclarations *left,const XrXirDeclarations *right,uint32_t functions) {
    if (!xir_compile_work(work,7)) return XR_XIR_BUDGET;
    if (!!left!=!!right) return XR_XIR_BAD_STRUCTURE;
    if (!left) return XR_XIR_OK;
    if (left->module_count!=right->module_count || left->slot_count!=right->slot_count ||
        left->literal_count!=right->literal_count || left->root_module!=right->root_module ||
        left->entry_function!=right->entry_function ||
        (left->module_count && (!left->modules || !right->modules)) ||
        (left->literal_count && (!left->literals || !right->literals))) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_body_bytes_match(work,left->functions,right->functions,
        functions,sizeof(*left->functions));
    if (status==XR_XIR_OK) status=effect_body_bytes_match(work,left->slots,right->slots,
        left->slot_count,sizeof(*left->slots));
    for (uint32_t m=0;m<left->module_count && status==XR_XIR_OK;++m) {
        const XrXirSourceModule *a=&left->modules[m],*b=&right->modules[m];
        if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
        if (a->name_length!=b->name_length || a->dependency_count!=b->dependency_count ||
            a->initializer!=b->initializer) return XR_XIR_BAD_STRUCTURE;
        status=effect_body_bytes_match(work,a->name,b->name,a->name_length,1);
        if (status==XR_XIR_OK) status=effect_body_bytes_match(work,a->dependencies,b->dependencies,
            a->dependency_count,sizeof(*a->dependencies));
    }
    for (uint32_t l=0;l<left->literal_count && status==XR_XIR_OK;++l) {
        const XrXirLiteral *a=&left->literals[l],*b=&right->literals[l];
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        if (a->length!=b->length) return XR_XIR_BAD_STRUCTURE;
        status=effect_body_bytes_match(work,a->bytes,b->bytes,a->length,1);
    }
    if (status==XR_XIR_OK) status=effect_body_implementations_match(work,left->implementations,right->implementations);
    return status;
}

static XrXirStatus effect_body_generics_match(const XrXirCompileContext *work,
    const XrXirGeneric *left,const XrXirGeneric *right,uint32_t functions) {
    if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
    if (!!left!=!!right) return XR_XIR_BAD_STRUCTURE;
    if (!left) return XR_XIR_OK;
    XrXirStatus status=XR_XIR_OK;
    for (uint32_t f=0;f<functions && status==XR_XIR_OK;++f) {
        const XrXirGeneric *a=&left[f],*b=&right[f];
        if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
        if (a->parameter_count!=b->parameter_count || a->argument_count!=b->argument_count ||
            (!!a->parameter_kinds!=!!b->parameter_kinds) ||
            (a->parameter_count && (!a->constraints || !b->constraints))) return XR_XIR_BAD_STRUCTURE;
        status=effect_body_bytes_match(work,a->arguments,b->arguments,a->argument_count,sizeof(*a->arguments));
        if (status==XR_XIR_OK && a->parameter_kinds) status=effect_body_bytes_match(work,
            a->parameter_kinds,b->parameter_kinds,a->parameter_count,sizeof(*a->parameter_kinds));
        for (uint32_t p=0;p<a->parameter_count && status==XR_XIR_OK;++p) {
            const XrXirConstraint *x=&a->constraints[p],*y=&b->constraints[p];
            if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            if (x->markers!=y->markers || x->interface_count!=y->interface_count ||
                (x->interface_count && (!x->interfaces || !y->interfaces))) return XR_XIR_BAD_STRUCTURE;
            for (uint32_t i=0;i<x->interface_count && status==XR_XIR_OK;++i)
                status=effect_body_application_match(work,&x->interfaces[i],&y->interfaces[i]);
        }
    }
    return status;
}

static XrXirStatus effect_body_metadata_match(const XrXirCompileContext *work,
    const XirMetadataDomain *owned,const XrXirModule *source) {
    XrXirStatus status=effect_body_declarations_match(work,owned->declarations,
        source->declarations,owned->function_count);
    if (status==XR_XIR_OK) status=effect_body_generics_match(work,owned->generics,
        source->generics,owned->function_count);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
    if (!!owned->defaults!=!!source->defaults) return XR_XIR_BAD_STRUCTURE;
    if (!owned->defaults) return XR_XIR_OK;
    if (owned->defaults->count!=source->defaults->count) return XR_XIR_BAD_STRUCTURE;
    return effect_body_bytes_match(work,owned->defaults->records,source->defaults->records,
        owned->defaults->count,sizeof(*owned->defaults->records));
}
