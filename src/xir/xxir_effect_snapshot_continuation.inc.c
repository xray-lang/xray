/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Same-operation complete correspondence precedes shared immutable ownership.
 */
static XrXirStatus effect_body_material_retain(const XrXirCompileContext *work,
    EffectBodyMaterial *body) {
    if (!xir_compile_context_valid(work) || !effect_body_material_shape(body) || body->resources!=work->resources)
        return XR_XIR_BAD_STRUCTURE;
    if (body->references==UINT32_MAX) return XR_XIR_BUDGET;
    /* Count read, increment read/write, output publication and the future
     * release read/write are prepaid before either ownership edge changes. */
    if (!xir_compile_work(work,5*sizeof(body->references)+sizeof(body)+2)) return XR_XIR_BUDGET;
    ++body->references;return XR_XIR_OK;
}
static XrXirStatus effect_invocation_bounds_copy_physical(const XrXirCompileContext *work,
    const EffectInvocationDeclaredBounds *source,const EffectPhysicalDomain *physical,
    EffectInvocationDeclaredBounds **output);

/* Only the immutable backbone is shared. Both existing equation creation
 * and publication replace declared bounds, so every generation owns a fresh
 * complete overlay before either writer can run. No child field is writable
 * through this continuation except the holder's own bookkeeping and bounds. */
static XrXirStatus effect_body_material_holder(const XrXirCompileContext *work,
    EffectBodyMaterial *basis,EffectBodyMaterial **output) {
    if (!xir_compile_context_valid(work) || !effect_body_material_shape(basis) ||
        basis->resources!=work->resources || (basis->backbone && basis->backbone->backbone) || !basis->declared ||
        !basis->physical || !output || *output) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationDeclaredBounds *declared=NULL;
    XrXirStatus status=effect_invocation_bounds_copy_physical(work,basis->declared,basis->physical,&declared);
    EffectBodyMaterial *holder=status==XR_XIR_OK?xir_compile_calloc(work,1,sizeof(*holder),&status):NULL;
    /* Count the full holder copy, pointer rebinding, local reference write and
     * the future holder branch, independent bound release and backbone drop. */
    uint64_t writes=sizeof(*holder)+8*sizeof(void *)+4*sizeof(uint32_t)+4;
    if (status==XR_XIR_OK && !xir_compile_work(work,writes)) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK) status=effect_body_material_retain(work,basis);
    if (status!=XR_XIR_OK) {
        effect_invocation_bounds_free(declared);xr_compile_resources_free(holder);return status;
    }
    /* No fallible operation follows the ownership edge acquisition. Domain
     * pointers refer to this holder's own structures, never parent bookkeeping. */
    *holder=*basis;holder->references=1;holder->backbone=basis;holder->overlay=NULL;holder->declared=declared;
    holder->storage_kind=EFFECT_BODY_RECORDS_OWNED;
    holder->domain.types=&holder->terms.types;
    if (basis->domain.defaults) holder->domain.defaults=&holder->defaults;
    *output=holder;return XR_XIR_OK;
}

static XrXirStatus effect_body_ordinary_compare(const XrXirCompileContext *work,
    const EffectBodyMaterial *body,const XrXirModule *source,const EffectBodyOrdinaryInput *ordinary,bool *same) {
    if (!xir_compile_work(work,7)) return XR_XIR_BUDGET;
    if (!body->ordinary_identities || body->evidence_kind ||
        body->ordinary_source_count!=ordinary->source->function_count ||
        body->stage!=source->stage || body->linkage_kind!=source->linkage_kind) return XR_XIR_BAD_STRUCTURE;
    if (effect_body_count(body)!=source->function_count) { *same=false;return XR_XIR_OK; }
    XrXirStatus status=effect_body_type_tables_compare(work,body->domain.types,source->types,same);
    if (status==XR_XIR_OK) status=effect_body_type_nodes_compare(work,body->domain.types,source->types,same);
    if (status==XR_XIR_OK) status=effect_body_metadata_compare(work,&body->domain,source,same);
    for (uint32_t f=0;f<source->function_count && status==XR_XIR_OK;++f) {
        EffectBodyOrdinaryIdentity identity={0};
        status=effect_body_ordinary_identity(work,ordinary,f,&identity);
        if (status!=XR_XIR_OK) break;
        if (!xir_compile_work(work,sizeof(identity))) return XR_XIR_BUDGET;
        if (identity.declaration!=body->ordinary_identities[f].declaration ||
            identity.owner!=body->ordinary_identities[f].owner) *same=false;
        status=effect_body_initial_roles_match(work,body,f);
        if (status==XR_XIR_OK) status=effect_body_function_compare(work,&body->functions[f],&source->functions[f],same);
    }
    return status;
}
/* A positive difference is handled by the original unique generation writer.
 * Malformed storage and OOM/BUDGET are errors, never reasons to rebuild. The
 * complete current ordinary prior has already run in generation_input. */
static XrXirStatus effect_body_material_continue(const XrXirCompileContext *work,
    const XrXirModule *source,const EffectBodyOrdinaryInput *ordinary,EffectBodyMaterial **output) {
    if (!output || *output || !source || !ordinary || !ordinary->basis) return XR_XIR_BAD_STRUCTURE;
    EffectBodyMaterial *basis=NULL;
    XrXirStatus status=effect_body_ordinary_basis(work,ordinary,&basis);
    if (status!=XR_XIR_OK) return status;
    bool same=true;
    status=effect_body_ordinary_compare(work,basis,source,ordinary,&same);
    if (status!=XR_XIR_OK || !same) return status;
    return effect_body_material_holder(work,basis,output);
}
