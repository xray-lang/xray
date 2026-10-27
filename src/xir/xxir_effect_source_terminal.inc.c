/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_source_terminal.inc.c - Fresh read-only Source terminal replay
 *
 * KEY CONCEPT:
 *   An immutable owner supplies storage, never a permission exemption. Each
 *   use checks physical inputs, closed equations and independent outcomes.
 *   Temporary type and invocation scratch dies before the final pure checks.
 */
/* Drop every borrowed constructor pointer before freeing the backbone's
 * real scratch. The owning immutable descriptors remain at the same addresses. */
static XrXirStatus effect_final_retire_body(const XrXirCompileContext *work,EffectBodyMaterial *body) {
    uint32_t depth=0;
    for (;body;body=body->backbone) {

        if (depth++==3 || !effect_body_material_shape(body) || body->resources!=work->resources)
            return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=effect_terms_retire(work,&body->terms,body->backbone==NULL);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* Only the completed private constructor calls this. All real scratch frees
 * precede the last current-Input comparison and the sole public ownership move. */
static XrXirStatus effect_final_retire_construction(const XrXirCompileContext *work,XrXirEffects *effects) {
    if (!effects || !effects->outcomes_owner || effects->root_input || !effects->invocations ||
        !effects->invocations->body || effects->generation || effects->refinement) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationCertificate *certificate=effects->invocations;
    XrXirStatus status=effect_terms_retire(work,&certificate->terms,true);
    if (status==XR_XIR_OK) status=effect_final_retire_body(work,certificate->body);
    EffectContextOwner *owner=effects->outcomes_work.contexts;
    if (status==XR_XIR_OK && owner) {
        if (!owner->ordinary || !owner->dense || !owner->forest || owner->refinement ||
            !owner->forest->invocations || !owner->forest->invocations->body ||
            !owner->ordinary->uses.generation || owner->dense->uses.generation) return XR_XIR_BAD_STRUCTURE;
        certificate=owner->forest->invocations;
        status=effect_terms_retire(work,&owner->ordinary->terms,true);
        if (status==XR_XIR_OK) status=effect_terms_retire(work,&owner->dense->terms,true);
        if (status==XR_XIR_OK) status=effect_terms_retire(work,&owner->forest->terms,true);
        if (status==XR_XIR_OK) status=effect_terms_retire(work,&certificate->terms,true);
        if (status==XR_XIR_OK) status=effect_final_retire_body(work,certificate->body);
        if (status==XR_XIR_OK) status=effect_final_retire_body(work,owner->ordinary->uses.generation);
        if (status==XR_XIR_OK && (owner->ordinary->resources!=work->resources ||
            owner->ordinary->uses.resources!=work->resources ||
            owner->dense->resources!=work->resources || owner->dense->uses.resources!=work->resources))
            status=XR_XIR_BAD_STRUCTURE;

        if (status==XR_XIR_OK) {
            effect_ordinary_results_free(owner->ordinary);
            owner->ordinary->uses=(XrXirEffects){0};
            effect_ordinary_results_free(owner->dense);owner->dense->uses=(XrXirEffects){0};
        }
    }
    return status;
}

static XrXirStatus effect_source_body_retired(const XrXirCompileContext *work,const EffectBodyMaterial *body) {
    uint32_t depth=0;
    for (;body;body=body->backbone) {

        if (depth++==3 || !effect_body_material_shape(body) || body->resources!=work->resources ||
            !effect_terms_scratch_retired(&body->terms)) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_source_equation_shape(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate) {
    const EffectInvocationFinal *owner=certificate->final;
    const EffectFinalFunctions *result=(&certificate->final->summary);
    uint32_t count=effect_body_count(certificate->body);
    if (!owner->count || owner->count>owner->capacity || (uint64_t)owner->capacity>(uint64_t)work->limits.functions*work->limits.instructions ||
        owner->edge_count>owner->edge_capacity || !!owner->edges!=!!owner->edge_capacity ||
        !owner->nodes ||
        !owner->roots || !owner->root_spaces || !!owner->sites!=!!owner->site_count ||
        owner->deferred_count>owner->deferred_capacity ||
        !!owner->deferred_calls!=!!owner->deferred_capacity ||
        result->resources!=work->resources || result->count!=count || result->capacity!=count ||
        !result->functions || !result->witnesses ||
        certificate->declarations!=certificate->body->declarations ||
        certificate->generics!=certificate->body->generics ||
        certificate->defaults.count!=certificate->body->defaults.count ||
        certificate->defaults.records!=certificate->body->defaults.records ||
        certificate->terms.types.nodes!=certificate->body->terms.types.nodes ||
        certificate->terms.types.count!=certificate->body->terms.types.count ||
        certificate->terms.types.nominals!=certificate->body->terms.types.nominals ||
        certificate->terms.types.interfaces!=certificate->body->terms.types.interfaces) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<count;++f) {

        const XrXirOrigin *origin=&certificate->origins[f];
        if (owner->roots[f]>=owner->count || owner->root_spaces[f]>=count ||
            !!origin->arguments!=!!origin->argument_count ||
            origin->effect_arguments || origin->effect_argument_count ||
            (certificate->owners[f]!=UINT32_MAX && certificate->owners[f]>=count))
            return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

/* Public formulas and terminal views borrow the sole Result. Exact alias
 * checks establish destruction provenance only; its stored records are
 * independently enumerated from the closed current equations below. */
static XrXirStatus effect_source_result_views(
    const EffectInvocationCertificate *certificate,const XrXirEffects *summary) {
    const EffectFinalFunctions *result=(&certificate->final->summary);
    if (summary->result_borrow!=result || summary->formula_terminals!=result->witnesses ||
        summary->constant_witnesses!=result->witnesses+(size_t)result->count*2)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<result->count;++f) {

        const XrXirRootFormula *actual=&effect_contracts_read(summary)[f].formula;
        const EffectFinalFunction *record=&result->functions[f];
        if (actual->constant_mask!=record->formula.constant_mask ||
            actual->term_count!=record->formula.term_count || actual->terms!=record->storage ||
            record->formula.terms!=record->storage || record->capacity!=actual->term_count)
            return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_source_atoms(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate) {
    const EffectInvocationFinal *owner=certificate->final;
    for (uint32_t n=0;n<owner->count;++n) {
        const EffectInvocationNode *node=&owner->nodes[n];
        if (node->root>=effect_body_count(certificate->body) ||
            node->body>=effect_body_count(certificate->body)) return XR_XIR_BAD_STRUCTURE;
        uint32_t parameters=certificate->body->functions[node->root].parameter_count;
        if (!effect_invocation_node_shape(node,parameters,
            certificate->body->functions[node->body].instruction_count)) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t a=0;a<node->cause_count;++a) {

            if (effect_invocation_atom(node,parameters,a,false)) {
                XrXirStatus status=effect_invocation_certificate_cause(work,certificate,n,a);
                if (status!=XR_XIR_OK) return status;
            } else {
                const EffectInvocationCause *cause=&node->causes[a];
                XrXirRootEffectWitness zero={0};
                if (cause->next!=UINT32_MAX || cause->edge!=UINT32_MAX || cause->distance!=UINT32_MAX ||
                    !effect_invocation_witness_equal(&cause->witness,&zero)) return XR_XIR_BAD_STRUCTURE;
            }
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_source_projections(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate) {
    const EffectFinalFunctions *result=(&certificate->final->summary);
    /* This same independent receiving core checks all canonical terms,
     * selected atoms, current causes and final transformed witnesses. */
    XrXirStatus status=effect_invocation_publication_match(work,certificate,result);
    if (status!=XR_XIR_OK) return status;
    return effect_invocation_public_gate(work,certificate);
}

/* Verification borrows the actual Final and its sole immutable Input.
 * Request scratch contains only the fresh incoming index; no construction
 * owner, queue or copied semantic inventory participates in this proof. */
/* Constructor relations grant no final admission. The terminal inventory
 * pass checks the sole finite source relation after all cleanup callbacks. */
static XrXirStatus effect_source_relations(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,const XrXirEffects *summary) {
    if (!certificate) return XR_XIR_BAD_STRUCTURE;

    if (!effect_terms_scratch_retired(&certificate->terms)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus retired=effect_source_body_retired(work,certificate->body);
    if (retired!=XR_XIR_OK) return retired;

    if (!certificate || !effect_body_material_shape(certificate->body) ||
        certificate->resources!=work->resources || certificate->body->resources!=work->resources ||
        !certificate->final || !certificate->final || !certificate->declared ||
        certificate->terms.remaining || certificate->body->terms.remaining ||
        certificate->final->material!=certificate->body ||
        certificate->final->physical!=certificate->body->physical ||
        certificate->final->cells!=certificate->body->cells ||
        (summary && (summary->count!=effect_body_count(certificate->body) ||
            !effect_contracts_read(summary))) || !certificate->origins || !certificate->owners)
        return XR_XIR_BAD_STRUCTURE;
    for (const EffectBodyMaterial *body=certificate->body;body;body=body->backbone) {

        if (!effect_body_material_shape(body) || body->resources!=work->resources ||
            body->terms.remaining) return XR_XIR_BAD_STRUCTURE;
    }

    EffectInvocationRead owner={0};
    XrXirStatus opening=effect_invocation_read_open(work,certificate,certificate->final,&owner);
    if (opening!=XR_XIR_OK) return opening;
    EffectInvocationCertificate local=*certificate;local.terms.remaining=work;
    XrXirStatus status=effect_source_equation_shape(work,certificate);
    if (status==XR_XIR_OK && summary) status=effect_source_result_views(certificate,summary);
    for (uint32_t f=0;summary && f<summary->count && status==XR_XIR_OK;++f) {
        const XrXirFunctionEffectContract *contract=&effect_contracts_read(summary)[f];
        uint32_t parameters=certificate->body->functions[f].parameter_count;

        if (contract->parameter_count!=parameters || !!contract->parameters!=!!parameters) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        for (uint32_t p=0;p<parameters && status==XR_XIR_OK;++p) {

            uint32_t parameter_kind=UINT32_MAX;
            status=effect_role_parameter_read(work,certificate->body,&certificate->final->role_proof,f,p,&parameter_kind);
            if (status==XR_XIR_OK && parameter_kind!=contract->parameters[p].kind) status=XR_XIR_BAD_STRUCTURE;
        }
    }
    if (status==XR_XIR_OK) status=effect_body_physical_match(work,owner.final->physical,owner.final->material);
    if (status==XR_XIR_OK) status=effect_role_proof_match(work,certificate->body,&certificate->final->role_proof);
    if (status==XR_XIR_OK) status=effect_invocation_cell_terminals_read(&owner,&owner.body);
    if (status==XR_XIR_OK) status=effect_invocation_iterators_read(&owner,&owner.body,NULL);
    if (status==XR_XIR_OK) status=effect_invocation_closed_read(work,certificate->final);
    if (status==XR_XIR_OK) status=effect_invocation_deferred_verify(&owner);
    if (status==XR_XIR_OK) status=effect_invocation_bind_bounds(&local,certificate->declared,owner.final->physical);
    if (status==XR_XIR_OK) status=effect_source_atoms(work,&local);
    if (status==XR_XIR_OK) status=effect_source_projections(work,&local);
    effect_invocation_read_close(&owner);return status;
}

static XrXirStatus effect_source_contexts(const XrXirCompileContext *work,
    const XrXirModule *source,const XrXirEffects *effects) {
    if(!source->provenance || source->provenance->kind!=XR_XIR_EVIDENCE_TEMPLATE) {

        return effect_contexts_read(effects)?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
    }
    if (!source->declarations || !source->declarations->functions)
        return effect_contexts_read(effects)?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
    /* Context construction uses this same canonical empty type domain.
     * Normalize only the request view; the complete source input was matched
     * independently before this terminal, and every context is still replayed. */
    XrXirModule normalized;
    if (!source->types) {

        normalized=*source;normalized.types=&effect_empty_types;source=&normalized;
    }
    const EffectContextOwner *owner=effect_contexts_read(effects);
    if (!effect_context_owner_published(work->resources,owner) ||
        !owner || !owner->ordinary || !owner->dense || !owner->forest || owner->refinement ||
        owner->ordinary->terms.remaining || owner->dense->terms.remaining || owner->forest->terms.remaining)
        return XR_XIR_BAD_STRUCTURE;

    if (!effect_terms_scratch_retired(&owner->ordinary->terms) ||
        !effect_terms_scratch_retired(&owner->dense->terms) || !effect_terms_scratch_retired(&owner->forest->terms))
        return XR_XIR_BAD_STRUCTURE;

    EffectTerms ordinary={.types=owner->ordinary->terms.types,.remaining=work};
    EffectTerms dense={.types=owner->dense->terms.types,.remaining=work};
    const EffectInvocationCertificate *certificate=owner->forest->invocations;
    XrXirStatus status=effect_context_dense_stable_read(work,owner,source,certificate,&ordinary,&dense);
    if (status==XR_XIR_OK) status=effect_source_relations(work,certificate,NULL);
    if (status==XR_XIR_OK) status=effect_invocation_forest_match(work,owner->forest);
    effect_terms_free(&dense);effect_terms_free(&ordinary);return status;
}

static XrXirStatus effect_source_publication_match(const XrXirCompileContext *work,
    const XrXirModule *source,const XrXirEffects *effects) {
    const EffectInvocationCertificate *certificate=effects->invocations;
    const EffectFinalOutcomes *public=effect_final_outcomes(effects);
    if (!public) return XR_XIR_BAD_STRUCTURE;
    if (!public->root || !public->root_witnesses || !public->unresolved_witnesses ||
        !public->contracts || !effects->formula_terminals || !effects->constant_witnesses)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<effects->count;++f) {
        const XrXirOrigin *origin=&certificate->origins[f];
        uint32_t parent=source->declarations && source->declarations->functions[f].cleanup_owner?
            source->declarations->functions[f].cleanup_owner-1:UINT32_MAX;

        if (origin->function!=f || origin->argument_count || origin->arguments ||
            certificate->owners[f]!=parent) return XR_XIR_BAD_STRUCTURE;
        const EffectFinalFunctions *result=(&certificate->final->summary);
        const XrXirRootFormula *a=&public->contracts[f].formula,*b=&result->functions[f].formula;

        if (a->constant_mask!=b->constant_mask || a->term_count!=b->term_count ||
            !!a->terms!=!!a->term_count) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t t=0;t<a->term_count;++t) {

            if (a->terms[t].kind!=b->terms[t].kind || a->terms[t].index!=b->terms[t].index)
                return XR_XIR_BAD_STRUCTURE;
        }
        for (uint32_t d=0;d<4;++d) {
            const XrXirRootEffectWitness *expected=&result->witnesses[(size_t)d*effects->count+f];

            if (!effect_invocation_witness_equal(expected,&effects->formula_terminals[(size_t)d*effects->count+f]) ||
                (d>=2 && !effect_invocation_witness_equal(expected,
                    &effects->constant_witnesses[(size_t)(d-2)*effects->count+f]))) return XR_XIR_BAD_STRUCTURE;
        }

    }
    return effect_public_root_match(work,effects);
}

/* Control and Error consume independent Final evidence after all earlier
 * receiving cleanup callbacks. Neither domain reconstructs an Error Graph. */
static XrXirStatus effect_source_outcomes(const XrXirCompileContext *work,const XrXirEffects *effects) {
    return effect_final_control_verify(work,effects);
}

/* This terminal role pass follows every receiving free callback. It has
 * no mutable graph or scratch allocation and cannot retain a passed receipt. */
static XrXirStatus effect_source_role_certificate(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate) {
    XrXirStatus status=effect_role_range(work,certificate,1,sizeof(*certificate));
    if (status!=XR_XIR_OK) return status;
    if (certificate->resources!=work->resources) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,certificate->final,1,sizeof(*certificate->final));
    if (status!=XR_XIR_OK) return status;
    if (certificate->final->material!=certificate->body) return XR_XIR_BAD_STRUCTURE;
    return effect_final_inventory_match(work,certificate->body,certificate->final);
}
static XrXirStatus effect_source_terminal_roles(const XrXirCompileContext *work,const XrXirEffects *effects) {
    XrXirStatus status=effect_source_role_certificate(work,effects->invocations);
    if (status!=XR_XIR_OK) return status;
    const EffectInvocationCertificate *certificate=effects->invocations;
    const EffectFinalOutcomes *outcomes=certificate->final->outcomes;
    status=effect_role_range(work,outcomes,1,sizeof(*outcomes));if (status!=XR_XIR_OK) return status;
    if (outcomes->resources!=work->resources || outcomes->count!=effects->count) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_contracts_match(work,certificate->body,&certificate->final->role_proof,
        outcomes->contracts,outcomes->count);
    if (status!=XR_XIR_OK || !outcomes->contexts) return status;
    status=effect_role_range(work,outcomes->contexts,1,sizeof(*outcomes->contexts));
    if (status!=XR_XIR_OK) return status;
    const EffectContextForest *forest=outcomes->contexts->forest;
    status=effect_role_range(work,forest,1,sizeof(*forest));if (status!=XR_XIR_OK) return status;
    if (forest->resources!=work->resources) return XR_XIR_BAD_STRUCTURE;
    status=effect_source_role_certificate(work,forest->invocations);if (status!=XR_XIR_OK) return status;
    const EffectInvocationCertificate *nested=forest->invocations;
    if (forest->count!=effect_body_count(nested->body)) return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}

static XrXirStatus effect_source_terminal_current(const XrXirCompileContext *work,
    const XrXirModule *source,const XrXirEffects *effects,bool checked_copy) {
    if (!xir_effects_context_matches(work,effects,source?source->function_count:0) ||
        !effect_final_outcomes(effects) || !source || source->stage!=XR_XIR_CHECKED || !effects->invocations ||
        effects->generation || effects->refinement) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_source_relations(work,effects->invocations,effects);
    if (status==XR_XIR_OK) status=effect_source_contexts(work,source,effects);
    if (status==XR_XIR_OK) status=effect_source_publication_match(work,source,effects);
    if (status==XR_XIR_OK) status=effect_source_outcomes(work,effects);
    if (status==XR_XIR_OK) status=effect_source_terminal_roles(work,effects);
    if (status==XR_XIR_OK) status=error_finite_outcomes_match(work,effects->invocations->body,effect_final_outcomes(effects));
    if (status==XR_XIR_OK) status=checked_copy?
        effect_body_checked_copy_match(work,effects->invocations->body,source):
        effect_invocation_current_input_match(work,source,effects->invocations->body);
    return status;
}

XR_FUNC XrXirStatus xir_effects_source_terminal(const XrXirCompileContext *work,
    const XrXirModule *source,const XrXirEffects *effects) {
    if(!source || !source->provenance || source->provenance->kind!=XR_XIR_EVIDENCE_TEMPLATE) return XR_XIR_BAD_STRUCTURE;
    return effect_source_terminal_current(work,source,effects,false);
}
XR_FUNC XrXirStatus xir_effects_raw_source_terminal(const XrXirCompileContext *work,
    const XrXirModule *source,const XrXirEffects *effects) {
    if(!source || source->provenance) return XR_XIR_BAD_STRUCTURE;
    return effect_source_terminal_current(work,source,effects,false);
}

XR_FUNC XrXirStatus xir_effects_receiving_terminal(const XrXirCompileContext *work,
    const XrXirModule *module,const XrXirEffects *effects) {
    if(!module || !module->provenance || !xir_effect_kind_is_closed(module->provenance->kind))
        return XR_XIR_BAD_STRUCTURE;
    return effect_source_terminal_current(work,module,effects,false);
}

XR_FUNC XrXirStatus xir_effects_built_copy_match(const XrXirCompileContext *work,
    const XrXirModule *module,const XrXirEffects *effects) {
    if(!xir_effects_context_matches(work,effects,module?module->function_count:0) ||
        !effects->invocations) return XR_XIR_BAD_STRUCTURE;
    return effect_body_checked_copy_match(work,effects->invocations->body,module);
}
XR_FUNC XrXirStatus xir_effects_built_copy_terminal(const XrXirCompileContext *work,
    const XrXirModule *module,const XrXirEffects *effects) {
    if(!module || module->provenance) return XR_XIR_BAD_STRUCTURE;
    return effect_source_terminal_current(work,module,effects,true);
}
