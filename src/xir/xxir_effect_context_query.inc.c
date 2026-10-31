/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_context_query.inc.c - Structural selection from authentic contexts
 */
#include "xxir_effect_reference_internal.h"

static XrXirStatus effect_context_owner_project(const XrXirCompileContext *context,
    const XrXirModule *source, XrXirEffects *effects, const EffectContextOwner *fresh) {

    for (uint32_t f=0;f<effects->count;++f) {
        if (source->generics && source->generics[f].parameter_count) continue;
        EffectContextRootValue value;
        XrXirStatus status=effect_invocation_forest_values(context,fresh->forest,f,&value);
        if(status!=XR_XIR_OK) return status;
        effects->outcomes_work.root[f]=value.facts;
        effects->outcomes_work.root_witnesses[f]=value.root;
        effects->outcomes_work.unresolved_witnesses[f]=value.unresolved;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_context_owner_refresh(const XrXirCompileContext *context,
    const XrXirModule *source, XrXirEffects *effects,EffectBodyMaterial *initial) {
    if (!source->provenance || source->provenance->kind!=XR_XIR_EVIDENCE_TEMPLATE ||
        !source->declarations || !source->declarations->functions) return XR_XIR_OK;
    EffectContextOwner *fresh=NULL;
    XrXirStatus status=effect_context_owner_build(context,source,effects->refinement,initial,effects->input_facts,&fresh);
    if (status!=XR_XIR_OK) return status;
    status=effect_context_owner_project(context,source,effects,fresh);
    if (status!=XR_XIR_OK) { effect_context_owner_free(fresh);return status; }
    EffectContextOwner *old=effects->outcomes_work.contexts;effects->outcomes_work.contexts=fresh;
    effect_context_owner_free(old);return XR_XIR_OK;
}

XR_FUNC bool xir_effects_context_available(const XrXirCompileContext *context,
    const XrXirEffects *effects) {
    return xir_compile_context_valid(context) && effects && effects->resources==context->resources &&
        effect_contexts_read(effects)!=NULL;
}

static XrXirStatus effect_context_refresh_current(const XrXirCompileContext *context,
    const XrXirModule *source, const XrXirEffects *effects) {
    if (!xir_effects_context_matches(context,effects,source ? source->function_count : 0) ||
        !source || !effects->outcomes_owner || effects->root_input) return XR_XIR_BAD_STRUCTURE;
    XrXirModule normalized=*source;
    if (!normalized.types) normalized.types=&effect_empty_types;
    source=&normalized;
    if (!effect_contexts_read(effects)) return XR_XIR_OK;
    if (effects->refinement) {
        XrXirStatus bound_status=effect_refinement_match(context,source,effects->refinement);
        if (bound_status!=XR_XIR_OK) return bound_status;
    }
    const EffectOrdinaryContexts *ordinary=effect_contexts_read(effects)->ordinary;
    EffectTerms terms={.types=ordinary->terms.types,.remaining=context};

    XrXirStatus status=effect_terms_snapshot_match(&terms,source->types,ordinary->source_type_count);
    if (status==XR_XIR_OK) status=effect_ordinary_bodies_match(ordinary,source,&terms);
    effect_terms_free(&terms);
    if (status==XR_XIR_BAD_TYPE || status==XR_XIR_BAD_STRUCTURE)
        /* Source may have advanced inside the refiner before the next outer
         * invocation publication. Its changed view needs the original writer. */
        return effect_context_owner_refresh(context,source,(XrXirEffects *)effects,NULL);
    return status;
}

static XrXirStatus effect_context_origin_match(const XrXirCompileContext *context,
    const EffectOrdinaryContexts *dense, const XirEffectContextInput *input,
    uint32_t index, bool *same) {
    const EffectOrdinaryNode *node=&dense->nodes[index];
    const XrXirOrigin *origin=input->origin;
    if (!origin || !!origin->arguments!=!!origin->argument_count ||
        !!origin->effect_arguments!=!!origin->effect_argument_count) return XR_XIR_BAD_STRUCTURE;
    *same=node->declaration==origin->function && node->argument_count==origin->argument_count;
    if (!*same) return XR_XIR_OK;
    uint32_t count=dense->functions[index].parameter_count;
    if (!origin->effect_argument_count && count) {
        *same=index<dense->base_count && !origin->argument_count;return XR_XIR_OK;
    }
    /* Zero physical parameters form a complete empty vector. Its ordinary
     * generic identity must still match exactly; it is not the base identity.
     * A missing nonempty physical vector retains the base-only rule above. */
    if (origin->effect_argument_count!=count || (count && !origin->effect_arguments)) {
        *same=false;return XR_XIR_OK;
    }
    XrXirTypeMatchScratch scratch={context->resources,NULL};XrXirStatus status=XR_XIR_OK;
    for (uint32_t a=0;a<node->argument_count && *same && status==XR_XIR_OK;++a) {
        status=xr_xir_compile_type_substitution_matches_between_scratch(context,
            &dense->terms.types,input->types,NULL,0,node->arguments[a],origin->arguments[a],&scratch);
        if (status==XR_XIR_BAD_TYPE) { *same=false;status=XR_XIR_OK; }
    }
    for (uint32_t p=0;p<count && *same && status==XR_XIR_OK;++p) {

        if (origin->effect_arguments[p].parameter!=p) { *same=false;break; }
        status=xr_xir_compile_type_substitution_matches_between_scratch(context,
            &dense->terms.types,input->types,NULL,0,node->physical_types[p],origin->effect_arguments[p].type,&scratch);
        if (status==XR_XIR_BAD_TYPE) { *same=false;status=XR_XIR_OK; }
    }
    xr_xir_type_match_scratch_free(&scratch);return status;
}

/* Check each lexical vector before any matching identity can read it.
 * The complete typed owner-chain comparison still runs for every candidate. */
static XrXirStatus effect_context_owner_shapes(
    const XirEffectContextInput *input) {
    for (uint32_t a=0;a<input->owner_count;++a) {

        const XrXirOrigin *owner=&input->owners[a];
        if (!!owner->arguments!=!!owner->argument_count ||
            !!owner->effect_arguments!=!!owner->effect_argument_count) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_context_owners_match(const XrXirCompileContext *context,
    const EffectOrdinaryContexts *dense, const XirEffectContextInput *input,
    uint32_t selected, bool *same) {
    uint32_t owner=dense->nodes[selected].owner;
    for (uint32_t a=0;a<input->owner_count && *same;++a) {

        if (owner>=dense->count) { *same=false;break; }
        XirEffectContextInput parent=*input;parent.origin=&input->owners[a];
        XrXirStatus status=effect_context_origin_match(context,dense,&parent,owner,same);
        if (status!=XR_XIR_OK) return status;
        owner=dense->nodes[owner].owner;
    }
    /* Every selected context consumes its complete lexical owner chain,
     * including zero-arity generic contexts with an empty physical vector. */
    if (*same && owner!=UINT32_MAX) *same=false;
    return XR_XIR_OK;
}

typedef enum EffectContextAdmissionState {
    EFFECT_CONTEXT_ADMISSION_PENDING, EFFECT_CONTEXT_ADMISSION_COMPLETE,
    EFFECT_CONTEXT_ADMISSION_REJECTED
} EffectContextAdmissionState;
struct XirEffectContextScope {
    const XrXirCompileContext *context;
    const XrXirEffects *effects;
    const XrXirModule *source;
    const XrXirTypes *types;
    const XrXirConstruction *construction;
    XrCompileRequestControl *control;
    XrXirStatus status;
    EffectContextAdmissionState state;
};

static XrXirStatus effect_context_admission(const XrXirCompileContext *context,
    const XrXirEffects *effects,const XrXirModule *source,const XrXirTypes *types) {
    XrXirModule normalized=*source;
    if (!normalized.types) normalized.types=&effect_empty_types;
    if (!types) types=&effect_empty_types;
    const EffectOrdinaryContexts *dense=effect_contexts_read(effects)->dense;
    const EffectOrdinaryContexts *ordinary=effect_contexts_read(effects)->ordinary;
    EffectTerms terms={.types=ordinary->terms.types,.remaining=context};

    XrXirStatus status=effect_terms_snapshot_match(&terms,normalized.types,dense->source_type_count);
    if (status==XR_XIR_OK) status=effect_ordinary_bodies_match(ordinary,&normalized,&terms);
    if (status==XR_XIR_OK) status=types->interfaces ?
        effect_terms_domain_match(&terms,types) :
        effect_terms_projected_domain(&terms,types);
    effect_terms_free(&terms);
    return status;
}

static XrXirStatus effect_context_select_actual(const XrXirCompileContext *context,
    const XrXirEffects *effects,const XirEffectContextInput *input,XirEffectContextView *output) {
    const EffectOrdinaryContexts *dense=effect_contexts_read(effects)->dense;
    XrXirStatus status=XR_XIR_OK;
    uint32_t caller=UINT32_MAX;
    for (uint32_t f=0;f<dense->count;++f) {

        bool same=false;status=effect_context_origin_match(context,dense,input,f,&same);
        if (status==XR_XIR_OK && same) status=effect_context_owners_match(context,dense,input,f,&same);
        if (status!=XR_XIR_OK) return status;
        if (same) {
            if (caller!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            caller=f;
        }
    }
    if (caller==UINT32_MAX) return XR_XIR_BAD_TYPE;
    uint32_t target=caller;
    if (input->instruction!=UINT32_MAX) {
        const XrXirFunction *original=&input->source->functions[input->origin->function];
        if (input->instruction>=original->instruction_count ||
            !effect_context_family(original->instructions[input->instruction].op)) return XR_XIR_BAD_STRUCTURE;
        const XrXirInstruction *op=&dense->functions[caller].instructions[input->instruction];
        if (op->immediate<0 || (uint64_t)op->immediate>=dense->count) return XR_XIR_BAD_STRUCTURE;
        target=(uint32_t)op->immediate;
        if (caller<dense->base_count && input->source->generics &&
            input->source->generics[input->origin->function].parameter_count) return XR_XIR_BAD_TYPE;
    }

    EffectContextRootValue value;
    status=effect_invocation_forest_values(context,effect_contexts_read(effects)->forest,target,&value);
    if(status!=XR_XIR_OK) return status;
    const XrXirRootEffects *facts=&value.facts;
    uint32_t edge_mask=(facts->requires_root?XR_XIR_CALLABLE_ROOT_REQUIRED:0)|
        (facts->unresolved?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0);
    if (input->instruction!=UINT32_MAX) {
        status=effect_invocation_context_edge_mask(context,effect_contexts_read(effects)->forest,
            caller,input->instruction,target,&edge_mask);
        if (status!=XR_XIR_OK) return status;
    }
    *output=(XirEffectContextView){&dense->terms.types,&dense->functions[target],
        dense->nodes[target].physical_types,dense->nodes[target].arguments,dense->nodes[target].declaration,
        dense->functions[target].parameter_count,dense->nodes[target].argument_count,
        !!(edge_mask&XR_XIR_CALLABLE_ROOT_REQUIRED),!!(edge_mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED)};
    return XR_XIR_OK;
}


static XrXirStatus effect_context_read_select(const XrXirCompileContext *context,
    const XrXirEffects *effects,const XirEffectContextInput *input,XirEffectContextView *output) {
    if (!xir_compile_context_valid(context) || !effects || effects->resources!=context->resources ||
        !effect_contexts_read(effects) || !input || !input->source || !input->origin || !output ||
        input->origin->function>=input->source->function_count ||
        !!input->origin->arguments!=!!input->origin->argument_count ||
        !!input->owners!=!!input->owner_count || input->owner_count>effects->count) return XR_XIR_BAD_STRUCTURE;
    XrXirModule normalized=*input->source;XirEffectContextInput request=*input;
    if (!normalized.types) normalized.types=&effect_empty_types;
    if (!request.types) request.types=&effect_empty_types;
    request.source=&normalized;
    XrXirStatus status=effect_context_owner_shapes(&request);
    if (status==XR_XIR_OK) status=effect_context_admission(context,effects,request.source,request.types);
    if (status!=XR_XIR_OK) return status;
    return effect_context_select_actual(context,effects,&request,output);
}

static XrXirStatus effect_context_scope_failure(XirEffectContextScope *scope,XrXirStatus status) {
    if (scope->status==XR_XIR_OK && status!=XR_XIR_OK) scope->status=status;
    if (scope->status!=XR_XIR_OK) scope->state=EFFECT_CONTEXT_ADMISSION_REJECTED;
    return scope->status;
}

/* This operation never borrows a dynamic-array element or memoizes a query.
 * Source and actual pools stay immutable for this lexical call; only private
 * derived storage may grow. A new call must repeat the complete admission. */
static XrXirStatus effect_context_read_operation(const XrXirCompileContext *context,
    const XrXirEffects *effects,XirEffectInput input,const XrXirTypes *types,
    XirEffectContextOperation operation,void *data) {
    const XrXirModule *source=input.module;
    if (!xir_compile_context_valid(context) || !effects || effects->resources!=context->resources ||
        !effect_contexts_read(effects) || !source || !source->functions || !source->function_count ||
        !operation || effects->count!=source->function_count) return XR_XIR_BAD_STRUCTURE;
    XirEffectContextScope selection_scope={context,effects,source,types,input.construction,input.control,XR_XIR_OK,
        EFFECT_CONTEXT_ADMISSION_PENDING};

    XrXirStatus status=operation(data,&selection_scope);
    return selection_scope.status!=XR_XIR_OK ? selection_scope.status : status;
}

static XrXirStatus effect_context_read_scoped(XirEffectContextScope *scope,
    const XirEffectContextInput *input,XirEffectContextView *output) {
    if (!scope) return XR_XIR_BAD_STRUCTURE;
    if (scope->status!=XR_XIR_OK) return scope->status;
    if (!xir_compile_context_valid(scope->context) || !scope->effects ||
        scope->effects->resources!=scope->context->resources || !effect_contexts_read(scope->effects) ||
        !input || input->source!=scope->source || input->types!=scope->types ||
        !input->construction || input->construction!=scope->construction || !input->origin || !output ||
        input->origin->function>=scope->source->function_count ||
        !!input->origin->arguments!=!!input->origin->argument_count ||
        !!input->owners!=!!input->owner_count || input->owner_count>scope->effects->count)
        return effect_context_scope_failure(scope,XR_XIR_BAD_STRUCTURE);

    XrXirStatus shape=effect_context_owner_shapes(input);
    if (shape!=XR_XIR_OK) return effect_context_scope_failure(scope,shape);
    if (scope->state==EFFECT_CONTEXT_ADMISSION_PENDING) {
        scope->status=effect_context_admission(scope->context,scope->effects,scope->source,scope->types);
        scope->state=scope->status==XR_XIR_OK ? EFFECT_CONTEXT_ADMISSION_COMPLETE : EFFECT_CONTEXT_ADMISSION_REJECTED;
    }
    if (scope->status!=XR_XIR_OK) return scope->status;
    if (scope->state!=EFFECT_CONTEXT_ADMISSION_COMPLETE)
        return effect_context_scope_failure(scope,XR_XIR_BAD_STRUCTURE);
    XrXirModule normalized=*input->source;XirEffectContextInput request=*input;
    if (!normalized.types) normalized.types=&effect_empty_types;
    if (!request.types) request.types=&effect_empty_types;
    request.source=&normalized;
    XrXirStatus status=effect_context_select_actual(scope->context,scope->effects,&request,output);
    return effect_context_scope_failure(scope,status);
}

/* The base reference API has no instantiated owner vector. It reconstructs
 * only complete nongeneric lexical identities from the authentic declaration
 * chain; generic ancestors require the explicit context request instead. */
static XrXirStatus effect_context_base_owner_origins(const XrXirCompileContext *context,
    const XrXirModule *module,uint32_t function,XrXirOrigin **output,uint32_t *output_count) {
    if (!module || function>=module->function_count || !output || *output ||
        !output_count || *output_count) return XR_XIR_BAD_STRUCTURE;

    /* An absent declaration table has no lexical chain. The complete source
     * snapshot and strict selector still reject omission of an actual owner. */
    if (!module->declarations) return XR_XIR_OK;
    if (!module->declarations->functions) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunctionIdentity *identities=module->declarations->functions;
    uint32_t current=function,count=0;
    for (;;) {

        uint32_t parent=identities[current].cleanup_owner;
        if (!parent) break;
        if (parent>module->function_count || count>=module->function_count ||
            parent-1==function) return XR_XIR_BAD_STRUCTURE;
        current=parent-1;
        if (module->generics && module->generics[current].parameter_count) return XR_XIR_BAD_TYPE;
        ++count;
    }
    if ((uint64_t)count>SIZE_MAX/sizeof(XrXirOrigin)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    XrXirOrigin *owners=count ? xir_compile_alloc(context,(size_t)count*sizeof(*owners),&status) : NULL;
    if (count && !owners) return status;
    current=function;
    for (uint32_t a=0;a<count;++a) {

        uint32_t parent=identities[current].cleanup_owner;
        if (!parent || parent>module->function_count || parent-1==function) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        current=parent-1;
        if (module->generics && module->generics[current].parameter_count) {
            status=XR_XIR_BAD_TYPE;break;
        }
        owners[a]=(XrXirOrigin){.function=current};
    }
    if (status==XR_XIR_OK && identities[current].cleanup_owner) status=XR_XIR_BAD_STRUCTURE;
    if (status!=XR_XIR_OK) { xr_compile_resources_free(owners);return status; }
    *output=owners;*output_count=count;return XR_XIR_OK;
}

/* Both entry points read the same real edge. A lexical scope borrows
 * immutable inputs only for its current operation and retains no result. */
typedef struct EffectReferenceLocation { uint32_t function,instruction; } EffectReferenceLocation;

static XrXirStatus effect_context_reference_root(const XrXirCompileContext *context,
    const XrXirEffects *effects,const XrXirModule *module,EffectReferenceLocation location,
    XirEffectContextScope *scope,uint32_t *output) {
    uint32_t function=location.function,instruction=location.instruction;
    if (!output || !module || !module->functions ||
        !xir_effects_context_matches(context,effects,module->function_count) || function>=module->function_count ||
        instruction>=module->functions[function].instruction_count || !module->functions[function].instructions)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=&module->functions[function].instructions[instruction];
    if (op->op!=XR_XIR_FUNCTION_REF || op->immediate<0 || (uint64_t)op->immediate>=effects->count)
        return XR_XIR_BAD_STRUCTURE;

    uint32_t mask=0;
    if (effect_contexts_read(effects) && (!module->generics || !module->generics[function].parameter_count)) {
        XrXirOrigin origin={.function=function};
        XrXirOrigin *owners=NULL;uint32_t owner_count=0;
        XrXirStatus status=effect_context_base_owner_origins(context,module,function,&owners,&owner_count);
        XirEffectContextInput input={module,module->types,&origin,instruction,owners,owner_count,
            scope?scope->construction:effects->input_facts->construction,scope?scope->control:NULL};
        XirEffectContextView selected={0};
        if (status==XR_XIR_OK) status=scope ? effect_context_read_scoped(scope,&input,&selected) :
            effect_context_read_select(context,effects,&input,&selected);
        xr_compile_resources_free(owners);
        if (status!=XR_XIR_OK) return status;
        mask=(selected.requires_root?XR_XIR_CALLABLE_ROOT_REQUIRED:0)|
            (selected.unresolved?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0);
    } else if (effect_contracts_read(effects) && !effect_contexts_read(effects)) {
        XrXirStatus status=effect_invocation_effect_edge_mask(context,effects,
            function,instruction,(uint32_t)op->immediate,&mask);
        if (status!=XR_XIR_OK) return status;
    } else {
        const XrXirRootEffects *facts=&effect_roots_read(effects)->root[op->immediate];
        mask=(facts->requires_root?XR_XIR_CALLABLE_ROOT_REQUIRED:0)|
            (facts->unresolved?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0);
    }

    *output=mask;return XR_XIR_OK;
}

static XrXirStatus effect_context_reference_current(const XrXirCompileContext *context,
    const XrXirEffects *effects,const XrXirModule *module,
    uint32_t function,uint32_t instruction,uint32_t *output) {
    return effect_context_reference_root(context,effects,module,
        (EffectReferenceLocation){function,instruction},NULL,output);
}
XR_FUNC XrXirStatus xir_effects_reference_root_scoped(XirEffectContextScope *scope,
    uint32_t function,uint32_t instruction,uint32_t *output) {
    if (!scope) return XR_XIR_BAD_STRUCTURE;
    if (scope->status!=XR_XIR_OK) return scope->status;
    if (scope->state!=EFFECT_CONTEXT_ADMISSION_PENDING && scope->state!=EFFECT_CONTEXT_ADMISSION_COMPLETE)
        return effect_context_scope_failure(scope,XR_XIR_BAD_STRUCTURE);
    XrXirStatus status=effect_context_reference_root(scope->context,scope->effects,scope->source,
        (EffectReferenceLocation){function,instruction},scope,output);
    return effect_context_scope_failure(scope,status);
}

static XrXirStatus effect_context_owner_trace(const XrXirCompileContext *context,
    const XrXirEffects *effects, uint32_t function, XrXirRootCauseTrace **output) {
    if (!xir_effects_context_matches(context,effects,effects ? effects->count : 0) ||
        function>=effects->count || !effect_contexts_read(effects) || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    return effect_context_forest_trace(context,effect_contexts_read(effects)->forest,function,output);
}
