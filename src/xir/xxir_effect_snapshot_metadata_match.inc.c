/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Full final owned metadata correspondence. No address or digest proves a field.
 */
/* NULL preserves the public matcher's original first-difference rejection.
 * A private result distinguishes data changes from errors without swallowing
 * an allocation, malformed-storage or finite-ledger failure. */
static XrXirStatus effect_body_difference(bool *same) {
    if (!same) return XR_XIR_BAD_STRUCTURE;
    *same=false;return XR_XIR_OK;
}

static XrXirStatus effect_body_bytes_match(const XrXirCompileContext *work,
    const void *left,const void *right,uint64_t count,size_t size,bool *same) {
    if (!size || count>SIZE_MAX/size || count*size==UINT64_MAX ||
        (count && (!left || !right))) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,count*size+1)) return XR_XIR_BUDGET;
    if (count && memcmp(left,right,(size_t)count*size)) {
        XrXirStatus difference=effect_body_difference(same);
        if (difference!=XR_XIR_OK) return difference;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_body_application_match(const XrXirCompileContext *work,
    const XrXirInterfaceApplication *left,const XrXirInterfaceApplication *right,bool *same) {
    if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
    if ((left->argument_count && !left->arguments) || (right->argument_count && !right->arguments))
        return XR_XIR_BAD_STRUCTURE;
    if (left->declaration!=right->declaration || left->argument_count!=right->argument_count) {
        return effect_body_difference(same);
    }
    return effect_body_bytes_match(work,left->arguments,right->arguments,
        left->argument_count,sizeof(*left->arguments),same);
}

#include "xxir_effect_snapshot_derivation_match.inc.c"

static XrXirStatus effect_body_implementations_match(const XrXirCompileContext *work,
    const XrXirImplementationTable *left,const XrXirImplementationTable *right,bool *same) {
    if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
    if ((left && left->count && !left->records) || (right && right->count && !right->records))
        return XR_XIR_BAD_STRUCTURE;
    if (!!left!=!!right || (left && left->count!=right->count)) return effect_body_difference(same);
    if (!left) return XR_XIR_OK;
    XrXirStatus status=XR_XIR_OK;
    for (uint32_t i=0;i<left->count && status==XR_XIR_OK;++i) {
        const XrXirImplementation *a=&left->records[i],*b=&right->records[i];
        if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
        if ((a->binding_count && !a->bindings) || (b->binding_count && !b->bindings)) return XR_XIR_BAD_STRUCTURE;
        if (a->nominal_declaration!=b->nominal_declaration || a->binding_count!=b->binding_count ||
            a->derivation_policy!=b->derivation_policy) {
            return effect_body_difference(same);
        }
        status=effect_body_application_match(work,&a->interface,&b->interface,same);
        for (uint32_t m=0;m<a->binding_count && status==XR_XIR_OK;++m) {
            const XrXirImplementationBinding *x=&a->bindings[m],*y=&b->bindings[m];
            if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            if (x->member!=y->member || x->function!=y->function) {
                XrXirStatus difference=effect_body_difference(same);
                if (difference!=XR_XIR_OK) return difference;
            }
            status=effect_body_application_match(work,&x->requirement,&y->requirement,same);
        }
    }
    return status;
}

static XrXirStatus effect_body_declarations_match(const XrXirCompileContext *work,
    const XrXirDeclarations *left,const XrXirDeclarations *right,uint32_t functions,bool *same) {
    if (!xir_compile_work(work,7)) return XR_XIR_BUDGET;
    if (!!left!=!!right) return effect_body_difference(same);
    if (!left) return XR_XIR_OK;
    if ((left->module_count && !left->modules) || (right->module_count && !right->modules) ||
        (left->literal_count && !left->literals) || (right->literal_count && !right->literals) ||
        (left->slot_count && !left->slots) || (right->slot_count && !right->slots)) return XR_XIR_BAD_STRUCTURE;
    if (left->module_count!=right->module_count || left->slot_count!=right->slot_count ||
        left->literal_count!=right->literal_count || left->root_module!=right->root_module ||
        left->entry_function!=right->entry_function) return effect_body_difference(same);
    XrXirStatus status=effect_body_bytes_match(work,left->functions,right->functions,
        functions,sizeof(*left->functions),same);
    if (status==XR_XIR_OK) status=effect_body_bytes_match(work,left->slots,right->slots,
        left->slot_count,sizeof(*left->slots),same);
    for (uint32_t m=0;m<left->module_count && status==XR_XIR_OK;++m) {
        const XrXirSourceModule *a=&left->modules[m],*b=&right->modules[m];
        if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
        if ((a->name_length && !a->name) || (b->name_length && !b->name) ||
            (a->dependency_count && !a->dependencies) || (b->dependency_count && !b->dependencies))
            return XR_XIR_BAD_STRUCTURE;
        if (a->name_length!=b->name_length || a->dependency_count!=b->dependency_count ||
            a->initializer!=b->initializer || a->lexical_scope!=b->lexical_scope) return effect_body_difference(same);
        status=effect_body_bytes_match(work,a->name,b->name,a->name_length,1,same);
        if (status==XR_XIR_OK) status=effect_body_bytes_match(work,a->dependencies,b->dependencies,
            a->dependency_count,sizeof(*a->dependencies),same);
    }
    for (uint32_t l=0;l<left->literal_count && status==XR_XIR_OK;++l) {
        const XrXirLiteral *a=&left->literals[l],*b=&right->literals[l];
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        if ((a->length && !a->bytes) || (b->length && !b->bytes)) return XR_XIR_BAD_STRUCTURE;
        if (a->length!=b->length) return effect_body_difference(same);
        status=effect_body_bytes_match(work,a->bytes,b->bytes,a->length,1,same);
    }
    if (status==XR_XIR_OK) status=effect_body_implementations_match(work,left->implementations,right->implementations,same);
    if (status==XR_XIR_OK) status=effect_body_derivations_match(work,left->derivations,right->derivations,same);
    return status;
}

static XrXirStatus effect_body_structural_match(const XrXirCompileContext *work,
    const XrXirConstraint *left,const XrXirConstraint *right,bool *same) {
    if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
    if ((!!left->structural_requirements!=!!left->structural_count) ||
        (!!right->structural_requirements!=!!right->structural_count)) return XR_XIR_BAD_STRUCTURE;
    if (left->structural_count!=right->structural_count) return effect_body_difference(same);
    for (uint32_t r=0;r<left->structural_count;++r) {
        if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
        const XrXirStructuralRequirement *a=&left->structural_requirements[r],*b=&right->structural_requirements[r];
        if (a->shape!=b->shape || a->policy!=b->policy) {
            XrXirStatus status=effect_body_difference(same);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_body_generics_match(const XrXirCompileContext *work,
    const XrXirGeneric *left,const XrXirGeneric *right,uint32_t functions,bool *same) {
    if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
    if (!!left!=!!right) return effect_body_difference(same);
    if (!left) return XR_XIR_OK;
    XrXirStatus status=XR_XIR_OK;
    for (uint32_t f=0;f<functions && status==XR_XIR_OK;++f) {
        const XrXirGeneric *a=&left[f],*b=&right[f];
        if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
        if ((a->parameter_count && !a->constraints) || (b->parameter_count && !b->constraints) ||
            (a->argument_count && !a->arguments) || (b->argument_count && !b->arguments)) return XR_XIR_BAD_STRUCTURE;
        if (a->parameter_count!=b->parameter_count || a->argument_count!=b->argument_count ||
            (!!a->parameter_kinds!=!!b->parameter_kinds)) return effect_body_difference(same);
        status=effect_body_bytes_match(work,a->arguments,b->arguments,a->argument_count,sizeof(*a->arguments),same);
        if (status==XR_XIR_OK && a->parameter_kinds) status=effect_body_bytes_match(work,
            a->parameter_kinds,b->parameter_kinds,a->parameter_count,sizeof(*a->parameter_kinds),same);
        for (uint32_t p=0;p<a->parameter_count && status==XR_XIR_OK;++p) {
            const XrXirConstraint *x=&a->constraints[p],*y=&b->constraints[p];
            if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            if ((x->interface_count && !x->interfaces) || (y->interface_count && !y->interfaces))
                return XR_XIR_BAD_STRUCTURE;
            if (x->markers!=y->markers || x->interface_count!=y->interface_count) return effect_body_difference(same);
            status=effect_body_structural_match(work,x,y,same);
            for (uint32_t i=0;i<x->interface_count && status==XR_XIR_OK;++i)
                status=effect_body_application_match(work,&x->interfaces[i],&y->interfaces[i],same);
        }
    }
    return status;
}

static XrXirStatus effect_body_metadata_compare(const XrXirCompileContext *work,
    const XirMetadataDomain *owned,const XrXirModule *source,bool *same) {
    XrXirStatus status=effect_body_declarations_match(work,owned->declarations,
        source->declarations,owned->function_count,same);
    if (status==XR_XIR_OK) status=effect_body_generics_match(work,owned->generics,
        source->generics,owned->function_count,same);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
    if (!!owned->defaults!=!!source->defaults) return effect_body_difference(same);
    if (!owned->defaults) return XR_XIR_OK;
    if ((owned->defaults->count && !owned->defaults->records) ||
        (source->defaults->count && !source->defaults->records)) return XR_XIR_BAD_STRUCTURE;
    if (owned->defaults->count!=source->defaults->count) return effect_body_difference(same);
    return effect_body_bytes_match(work,owned->defaults->records,source->defaults->records,
        owned->defaults->count,sizeof(*owned->defaults->records),same);
}

static XrXirStatus effect_body_metadata_match(const XrXirCompileContext *work,
    const XirMetadataDomain *owned,const XrXirModule *source) {
    return effect_body_metadata_compare(work,owned,source,NULL);
}
