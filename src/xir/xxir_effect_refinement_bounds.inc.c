/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_refinement_bounds.inc.c - Owned pre-bottom Source advertisements
 *
 * KEY CONCEPT:
 *   Temporary mathematical bottom is distinct from a real declaration bound.
 *   This owner exists only while Source refines; final checking discards it.
 */
typedef struct EffectRefinementCopy {
    uint32_t instruction;
    XrXirInstruction operation;
} EffectRefinementCopy;

typedef struct EffectRefinementFunction {
    const XrXirType *parameters, *instructions;
    uint32_t parameter_count, instruction_count, name_length;
    const char *name;
    XrXirFunctionIdentity identity;
    const EffectRefinementCopy *copies;
    uint32_t copy_count;
} EffectRefinementFunction;

struct EffectRefinementBounds {
    EffectTerms terms;
    XrCompileResources *resources;
    uint32_t count, source_type_count;
    EffectRefinementFunction *functions;
    XrXirGeneric *generics;
};

static void effect_refinement_free(EffectRefinementBounds *bounds) {
    if (!bounds) return;
    xr_xir_compile_generics_free(bounds->generics,bounds->count);
    effect_terms_free(&bounds->terms);xr_compile_resources_free(bounds);
}

/* Retain actual typed propagation copies, excluding real fixed barriers and
 * consuming CALL_BIND declarations. A later normalization must keep this exact
 * producer edge; its current source operand still proves every weakening. */

static XrXirStatus effect_refinement_match(const XrXirCompileContext *context,
    const XrXirModule *source, EffectRefinementBounds *bounds) {
    XrXirModule normalized=*source;
    if (!normalized.types) normalized.types=&effect_empty_types;
    source=&normalized;
    if (bounds->resources!=context->resources || source->function_count!=bounds->count ||
        source->types->count<bounds->source_type_count || !source->declarations ||
        !source->declarations->functions || !!bounds->generics!=!!source->generics)
        return XR_XIR_BAD_STRUCTURE;
    bounds->terms.remaining=context;
    XrXirStatus status=effect_terms_domain_match(&bounds->terms,source->types);
    for (uint32_t t=0;t<bounds->source_type_count && status==XR_XIR_OK;++t) {
        const XrXirTypeNode *owned=&bounds->terms.types.nodes[t],*current=&source->types->nodes[t];
        if (owned->parameter_span!=current->parameter_span || !effect_term_same(owned,current))
            status=bounds->terms.status==XR_XIR_OK ? XR_XIR_BAD_STRUCTURE : bounds->terms.status;
    }
    for (uint32_t f=0;f<bounds->count && status==XR_XIR_OK;++f) {
        const EffectRefinementFunction *original=&bounds->functions[f];
        const XrXirFunction *actual=&source->functions[f];

        if (original->parameter_count!=actual->parameter_count || original->instruction_count!=actual->instruction_count ||
            original->name_length!=actual->name_length ||
            memcmp(&original->identity,&source->declarations->functions[f],sizeof(original->identity)) ||
            (original->name_length && (!actual->name || memcmp(original->name,actual->name,original->name_length)))) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        if (bounds->generics) {
            const XrXirGeneric *left=&bounds->generics[f],*right=&source->generics[f];
            if (left->parameter_count!=right->parameter_count) { status=XR_XIR_BAD_STRUCTURE;break; }
            status=effect_terms_domain_constraints(&bounds->terms,source->types,left->constraints,
                right->constraints,left->parameter_count);
            for (uint32_t p=0;p<left->parameter_count && status==XR_XIR_OK;++p) {

                if (xr_xir_binder_kind(left,p)!=xr_xir_binder_kind(right,p)) status=XR_XIR_BAD_STRUCTURE;
            }
        }
    }
    bounds->terms.remaining=NULL;return status;
}
