/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * An ordinary view owns its changed metadata and retains only a completely
 * corresponding immutable initial body from this same compiler operation.
 */
static void effect_body_ordinary_overlay_free(EffectBodyOrdinaryOverlay *overlay) {
    if (!overlay) return;
    if (overlay->functions) for (uint32_t f=0;f<overlay->count;++f)
        xr_compile_resources_free((void *)overlay->functions[f].initial_roles);
    xr_compile_resources_free(overlay->ordinary_identities);
    xr_compile_resources_free(overlay->identities);xr_compile_resources_free(overlay->generics);
    xr_compile_resources_free(overlay->functions);xr_compile_resources_free(overlay);
}

/* NULL source generics and a real non-NULL empty ordinary table are different
 * representations. The new table is owned, never canonicalized into NULL. */
static XrXirStatus effect_body_initial_generics(const XrXirCompileContext *work,
    const EffectBodyMaterial *basis,const XrXirModule *current,bool *same) {
    if (!current->generics) return XR_XIR_BAD_STRUCTURE;
    if (basis->generics) return effect_body_generics_match(work,basis->generics,
        current->generics,current->function_count,same);
    for (uint32_t f=0;f<current->function_count;++f) {
        if (!xir_compile_work(work,5)) return XR_XIR_BUDGET;
        const XrXirGeneric *generic=&current->generics[f];
        if (generic->constraints || generic->parameter_count || generic->arguments ||
            generic->argument_count || generic->parameter_kinds) *same=false;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_body_initial_compare(const XrXirCompileContext *work,
    const XrXirModule *current,const EffectBodyOrdinaryInput *ordinary,bool *same) {
    EffectBodyMaterial *basis=ordinary->initial;
    if (!effect_body_material_shape(basis) || basis->resources!=work->resources ||
        basis->backbone || basis->ordinary_identities || !ordinary->contexts || ordinary->contexts->dense)
        return XR_XIR_BAD_STRUCTURE;
    /* This is the original complete public source matcher, including actual
     * blocks, evidence shape, metadata and all ten physical instruction words. */
    XrXirStatus status=effect_body_source_match(work,basis,ordinary->source);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
    if (basis->stage!=current->stage || basis->linkage_kind!=current->linkage_kind)
        return XR_XIR_BAD_STRUCTURE;
    if (effect_body_count(basis)!=current->function_count ||
        current->function_count!=ordinary->contexts->base_count) { *same=false;return XR_XIR_OK; }
    status=effect_body_type_tables_compare(work,basis->domain.types,current->types,same);
    if (status==XR_XIR_OK) status=effect_body_type_nodes_compare(work,basis->domain.types,current->types,same);
    if (status==XR_XIR_OK) status=effect_body_declarations_match(work,basis->declarations,
        current->declarations,current->function_count,same);
    if (status==XR_XIR_OK) status=effect_body_initial_generics(work,basis,current,same);
    for (uint32_t f=0;f<current->function_count && status==XR_XIR_OK;++f) {
        if (!xir_compile_work(work,sizeof(EffectBodyFunction)+3)) return XR_XIR_BUDGET;
        if (current->functions[f].blocks || current->functions[f].block_count) return XR_XIR_BAD_STRUCTURE;
        EffectBodyFunction expected=basis->functions[f];expected.blocks=NULL;expected.block_count=0;
        status=effect_body_function_compare(work,&expected,&current->functions[f],same);
    }
    return status;
}

/* Bounds come from this current ordinary input through the original sole
 * advertisement builder. The initial TEMPLATE may have different CALL_BIND
 * advertisements, so its bound inventory is never copied as this overlay. */
static XrXirStatus effect_body_initial_bounds(const XrXirCompileContext *work,
    const XrXirModule *current,EffectInvocationDeclaredBounds **output) {
    EffectBodyAdvertisementBuilder *builder=NULL;
    XrXirStatus status=effect_body_advertisements_begin(work,current,NULL,&builder);
    for (uint32_t f=0;f<current->function_count && status==XR_XIR_OK;++f) {
        const XrXirFunction *function=&current->functions[f];
        uint64_t values=(uint64_t)function->parameter_count+function->instruction_count;
        if (values>UINT32_MAX) { status=XR_XIR_BUDGET;break; }
        status=effect_body_advertisements_function(builder,f);
        for (uint32_t v=0;v<(uint32_t)values && status==XR_XIR_OK;++v) {
            if (!xir_compile_work(work,2)) { status=XR_XIR_BUDGET;break; }
            status=effect_body_advertisements_value(builder,f,v,xr_xir_operand_type(function,v));
        }
    }
    if (status==XR_XIR_OK) status=effect_body_advertisements_finish(&builder,output);
    effect_body_advertisements_free(builder);return status;
}

static XrXirStatus effect_body_initial_overlay(const XrXirCompileContext *work,
    const EffectBodyOrdinaryInput *ordinary,EffectBodyOrdinaryOverlay **output) {
    EffectBodyMaterial *basis=ordinary->initial;uint32_t count=effect_body_count(basis);
    XrXirStatus status=XR_XIR_OK;
    EffectBodyOrdinaryOverlay *overlay=xir_compile_calloc(work,1,sizeof(*overlay),&status);
    if (!overlay) return status;
    overlay->count=count;
    /* Before any child array exists, prepay its complete partial-failure
     * teardown scan as well as the successful final overlay release. */
    if (!xir_compile_work(work,sizeof(overlay->declarations)+
        ((uint64_t)count+5)*sizeof(void *)+count+1)) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK) overlay->functions=xir_compile_calloc(work,count,sizeof(*overlay->functions),&status);
    if (status==XR_XIR_OK) overlay->identities=xir_compile_calloc(work,count,sizeof(*overlay->identities),&status);
    if (status==XR_XIR_OK) overlay->generics=xir_compile_calloc(work,count,sizeof(*overlay->generics),&status);
    if (status==XR_XIR_OK) overlay->ordinary_identities=xir_compile_calloc(work,
        count,sizeof(*overlay->ordinary_identities),&status);
    if (status==XR_XIR_OK) overlay->declarations=*basis->declarations;
    for (uint32_t f=0;f<count && status==XR_XIR_OK;++f) {
        status=effect_body_ordinary_identity(work,ordinary,f,&overlay->ordinary_identities[f]);
        if (status!=XR_XIR_OK) break;
        if (!xir_compile_work(work,sizeof(*overlay->functions)+sizeof(*overlay->identities)+
            sizeof(*overlay->generics)+3*sizeof(void *)+2)) { status=XR_XIR_BUDGET;break; }
        overlay->functions[f]=basis->functions[f];overlay->functions[f].initial_roles=NULL;
        overlay->functions[f].blocks=NULL;overlay->functions[f].block_count=0;
        overlay->identities[f]=basis->declarations->functions[f];
        if (basis->generics) overlay->generics[f]=basis->generics[f];
        EffectBodyFunction *function=&overlay->functions[f];
        uint8_t *roles=function->parameter_count?xir_compile_alloc(work,function->parameter_count,&status):NULL;
        function->initial_roles=roles;
        for (uint32_t p=0;p<function->parameter_count && status==XR_XIR_OK;++p) {
            if (!xir_compile_work(work,3)) { status=XR_XIR_BUDGET;break; }
            roles[p]=xr_xir_callable_signature(basis->domain.types,function->parameters[p])?
                XR_XIR_EFFECT_PARAMETER_VARIABLE:XR_XIR_EFFECT_PARAMETER_FIXED;
        }
    }
    if (status!=XR_XIR_OK) { effect_body_ordinary_overlay_free(overlay);return status; }
    overlay->declarations.functions=overlay->identities;*output=overlay;return XR_XIR_OK;
}

static XrXirStatus effect_body_material_initial(const XrXirCompileContext *work,
    const XrXirModule *current,const EffectBodyOrdinaryInput *ordinary,EffectBodyMaterial **output) {
    if (!output || *output || !ordinary || !ordinary->initial) return XR_XIR_BAD_STRUCTURE;
    bool same=true;
    XrXirStatus status=effect_body_initial_compare(work,current,ordinary,&same);
    if (status!=XR_XIR_OK || !same) return status;
    EffectBodyOrdinaryOverlay *overlay=NULL;EffectInvocationDeclaredBounds *declared=NULL;
    EffectBodyMaterial *holder=NULL;
    status=effect_body_initial_overlay(work,ordinary,&overlay);
    if (status==XR_XIR_OK) status=effect_body_initial_bounds(work,current,&declared);
    if (status==XR_XIR_OK) holder=xir_compile_calloc(work,1,sizeof(*holder),&status);
    if (status==XR_XIR_OK && !xir_compile_work(work,sizeof(*holder)+13*sizeof(void *)+
        4*sizeof(uint32_t)+5)) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK) status=effect_body_material_retain(work,ordinary->initial);
    if (status!=XR_XIR_OK) {
        effect_body_ordinary_overlay_free(overlay);effect_invocation_bounds_free(declared);
        xr_compile_resources_free(holder);return status;
    }
    /* Retain is the last fallible action. Every borrowed payload is attached
     * afterwards; failure cleanup cannot release any parent child pointer. */
    *holder=*ordinary->initial;holder->references=1;holder->backbone=ordinary->initial;
    holder->overlay=overlay;holder->declared=declared;holder->evidence_kind=0;
    holder->functions=overlay->functions;holder->declarations=&overlay->declarations;
    holder->generics=overlay->generics;holder->ordinary_identities=overlay->ordinary_identities;
    holder->ordinary_source_count=ordinary->source->function_count;
    holder->domain.types=&holder->terms.types;holder->domain.declarations=holder->declarations;
    holder->domain.generics=holder->generics;
    if (ordinary->initial->domain.defaults) holder->domain.defaults=&holder->defaults;
    *output=holder;return XR_XIR_OK;
}
