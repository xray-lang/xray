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
static XrXirStatus effect_refinement_copies(EffectRefinementBounds *bounds,
    const XrXirModule *source,uint32_t function,EffectRefinementFunction *output) {
    const XrXirProvenance *provenance=source->provenance;
    if (!provenance || provenance->kind!=XR_XIR_EVIDENCE_TEMPLATE) return XR_XIR_OK;
    if (!provenance->contracts || provenance->contract_count!=source->function_count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *body=&source->functions[function];
    const XrXirFunctionEffectContract *contract=&provenance->contracts[function];
    XrXirStatus status=effect_contract_values(bounds->terms.remaining,source,function,contract);
    if (status!=XR_XIR_OK) return status;
    uint32_t count=0;
    for (uint32_t v=0;v<contract->value_count;++v) {
        if (!xir_compile_work(bounds->terms.remaining,3)) return XR_XIR_BUDGET;
        const XrXirRootValueIdentity *value=&contract->values[v];
        const XrXirInstruction *op=&body->instructions[value->instruction];
        if (value->mode==XR_XIR_EFFECT_VALUE_PROPAGATE && op->op==XR_XIR_COPY &&
            op->type==value->declared_type) ++count;
    }
    EffectRefinementCopy *copies=count?effect_terms_alloc(&bounds->terms,count,sizeof(*copies)):NULL;
    if (count && !copies) return bounds->terms.status;
    uint32_t at=0;
    for (uint32_t v=0;v<contract->value_count;++v) {
        if (!xir_compile_work(bounds->terms.remaining,3)) return XR_XIR_BUDGET;
        const XrXirRootValueIdentity *value=&contract->values[v];
        const XrXirInstruction *op=&body->instructions[value->instruction];
        if (value->mode!=XR_XIR_EFFECT_VALUE_PROPAGATE || op->op!=XR_XIR_COPY ||
            op->type!=value->declared_type) continue;
        if ((uint64_t)op->args[0]>=(uint64_t)body->parameter_count+body->instruction_count ||
            xr_xir_operand_type(body,op->args[0])!=op->type || at>=count) return XR_XIR_BAD_TYPE;
        if (!xir_compile_work(bounds->terms.remaining,sizeof(*copies)+sizeof(*op))) return XR_XIR_BUDGET;
        copies[at++]=(EffectRefinementCopy){value->instruction,*op};
    }
    if (at!=count) return XR_XIR_BAD_STRUCTURE;
    output->copies=copies;output->copy_count=count;return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_effects_refinement_capture(const XrXirCompileContext *context,
    const XrXirModule *source, const XrXirEffects *effects) {
    if (!xir_effects_context_matches(context,effects,source ? source->function_count : 0) ||
        !source || !source->declarations || !source->declarations->functions ||
        effects->refinement || effects->contexts) return XR_XIR_BAD_STRUCTURE;
    XrXirModule normalized=*source;
    if (!normalized.types) normalized.types=&effect_empty_types;
    source=&normalized;
    XrXirStatus status=XR_XIR_OK;
    EffectRefinementBounds *bounds=xir_compile_calloc(context,1,sizeof(*bounds),&status);
    if (!bounds) return status;
    bounds->resources=context->resources;bounds->terms.remaining=context;
    bounds->count=source->function_count;bounds->source_type_count=source->types->count;
    status=effect_terms_seed(&bounds->terms,source->types);
    if (status==XR_XIR_OK && source->generics)
        status=xr_xir_compile_generics_clone(context,source,&bounds->generics);
    if (status==XR_XIR_OK) {
        bounds->functions=effect_terms_alloc(&bounds->terms,bounds->count,sizeof(*bounds->functions));
        if (!bounds->functions) status=bounds->terms.status;
    }
    for (uint32_t f=0;f<bounds->count && status==XR_XIR_OK;++f) {
        const XrXirFunction *body=&source->functions[f];
        XrXirType *parameters=body->parameter_count ?
            effect_terms_alloc(&bounds->terms,body->parameter_count,sizeof(*parameters)) : NULL;
        XrXirType *instructions=body->instruction_count ?
            effect_terms_alloc(&bounds->terms,body->instruction_count,sizeof(*instructions)) : NULL;
        char *name=body->name_length ? effect_terms_alloc(&bounds->terms,body->name_length,1) : NULL;
        if ((body->parameter_count && !parameters) || (body->instruction_count && !instructions) ||
            (body->name_length && !name)) { status=bounds->terms.status;break; }
        if (!xir_compile_work(context,sizeof(EffectRefinementFunction)+body->name_length+
            (uint64_t)(body->parameter_count+body->instruction_count)*sizeof(XrXirType))) {
            status=XR_XIR_BUDGET;break;
        }
        for (uint32_t p=0;p<body->parameter_count;++p) parameters[p]=body->parameters[p];
        for (uint32_t i=0;i<body->instruction_count;++i) instructions[i]=body->instructions[i].type;
        if (body->name_length) memcpy(name,body->name,body->name_length);
        bounds->functions[f]=(EffectRefinementFunction){.parameters=parameters,.instructions=instructions,
            .parameter_count=body->parameter_count,.instruction_count=body->instruction_count,
            .name_length=body->name_length,.name=name,.identity=source->declarations->functions[f]};
        status=effect_refinement_copies(bounds,source,f,&bounds->functions[f]);
    }
    if (status!=XR_XIR_OK) { effect_refinement_free(bounds);return status; }
    bounds->terms.remaining=NULL;((XrXirEffects *)effects)->refinement=bounds;
    return XR_XIR_OK;
}

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
        if (owned->parameter_span!=current->parameter_span || !effect_term_same(&bounds->terms,owned,current))
            status=bounds->terms.status==XR_XIR_OK ? XR_XIR_BAD_STRUCTURE : bounds->terms.status;
    }
    for (uint32_t f=0;f<bounds->count && status==XR_XIR_OK;++f) {
        const EffectRefinementFunction *original=&bounds->functions[f];
        const XrXirFunction *actual=&source->functions[f];
        if (!xir_compile_work(context,sizeof(original->identity)+4+original->name_length)) {
            status=XR_XIR_BUDGET;break;
        }
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
                if (!xir_compile_work(context,1)) { status=XR_XIR_BUDGET;break; }
                if (xr_xir_binder_kind(left,p)!=xr_xir_binder_kind(right,p)) status=XR_XIR_BAD_STRUCTURE;
            }
        }
    }
    bounds->terms.remaining=NULL;return status;
}

static XrXirStatus effect_refinement_apply(EffectOrdinaryContexts *ordinary,
    const XrXirModule *source, EffectRefinementBounds *bounds) {
    const XrXirCompileContext *context=ordinary->terms.remaining;
    XrXirStatus status=effect_refinement_match(context,source,bounds);
    const XrXirType *cache=NULL;
    if (status==XR_XIR_OK) status=effect_terms_import(&ordinary->terms,&bounds->terms.types,&cache);
    for (uint32_t f=0;f<ordinary->count && status==XR_XIR_OK;++f) {
        EffectOrdinaryNode node=ordinary->nodes[f];
        const EffectRefinementFunction *original=&bounds->functions[node.declaration];
        XrXirType *parameters=original->parameter_count ?
            effect_terms_alloc(&ordinary->terms,original->parameter_count,sizeof(*parameters)) : NULL;
        if (original->parameter_count && !parameters) { status=ordinary->terms.status;break; }
        XrXirGeneric environment={.arguments=node.arguments,.argument_count=node.argument_count};
        for (uint32_t p=0;p<original->parameter_count && status==XR_XIR_OK;++p) {
            XrXirType declared=XR_XIR_UNIT;
            if (!xr_xir_callable_signature(&bounds->terms.types,original->parameters[p])) {
                parameters[p]=node.source_parameters[p];continue;
            }
            status=effect_terms_import_child(&ordinary->terms,cache,bounds->terms.types.count,
                original->parameters[p],&declared);
            if (status==XR_XIR_OK && f>=ordinary->base_count)
                status=effect_terms_substitute(&ordinary->terms,declared,&environment,&declared);
            parameters[p]=declared;
        }
        ordinary->functions[f].parameters=parameters;
    }
    return status;
}
