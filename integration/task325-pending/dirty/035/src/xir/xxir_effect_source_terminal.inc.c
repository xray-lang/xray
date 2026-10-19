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
 *   Every temporary graph, type substitution and error summary dies here.
 */
/* Drop every borrowed constructor pointer before freeing the backbone's
 * real scratch. The owning immutable descriptors remain at the same addresses. */
static XrXirStatus effect_final_retire_body(const XrXirCompileContext *work,EffectBodyMaterial *body) {
    uint32_t depth=0;
    for (;body;body=body->backbone) {
        if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
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
    }
    return status;
}

static XrXirStatus effect_source_body_retired(const XrXirCompileContext *work,const EffectBodyMaterial *body) {
    uint32_t depth=0;
    for (;body;body=body->backbone) {
        if (!xir_compile_work(work,11)) return XR_XIR_BUDGET;
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
        !result->functions || !result->facts || !result->witnesses ||
        certificate->declarations!=certificate->body->declarations ||
        certificate->generics!=certificate->body->generics ||
        certificate->defaults.count!=certificate->body->defaults.count ||
        certificate->defaults.records!=certificate->body->defaults.records ||
        certificate->terms.types.nodes!=certificate->body->terms.types.nodes ||
        certificate->terms.types.count!=certificate->body->terms.types.count ||
        certificate->terms.types.nominals!=certificate->body->terms.types.nominals ||
        certificate->terms.types.interfaces!=certificate->body->terms.types.interfaces ||
        owner->local_roots!=certificate->body->local_roots ||
        owner->local_witnesses!=certificate->body->local_witnesses) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<count;++f) {
        if (!xir_compile_work(work,6)) return XR_XIR_BUDGET;
        const XrXirOrigin *origin=&certificate->origins[f];
        uint32_t parameters=certificate->body->functions[f].parameter_count;
        if (owner->roots[f]>=owner->count || owner->root_spaces[f]>=count ||
            !!certificate->parameter_kinds[f]!=!!parameters ||
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
static XrXirStatus effect_source_result_views(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,const XrXirEffects *summary) {
    const EffectFinalFunctions *result=(&certificate->final->summary);
    if (summary->result_borrow!=result || summary->formula_terminals!=result->witnesses ||
        summary->constant_witnesses!=result->witnesses+(size_t)result->count*2)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<result->count;++f) {
        if (!xir_compile_work(work,5)) return XR_XIR_BUDGET;
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
            if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
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
static XrXirStatus effect_source_certificate(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,const XrXirEffects *summary) {
    if (!certificate) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,7)) return XR_XIR_BUDGET;
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
        !summary || summary->count!=effect_body_count(certificate->body) ||
        !effect_contracts_read(summary) || !certificate->origins || !certificate->owners || !certificate->parameter_kinds)
        return XR_XIR_BAD_STRUCTURE;
    for (const EffectBodyMaterial *body=certificate->body;body;body=body->backbone) {
        if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
        if (!effect_body_material_shape(body) || body->resources!=work->resources ||
            body->terms.remaining) return XR_XIR_BAD_STRUCTURE;
    }
    if (!xir_compile_work(work,sizeof(EffectInvocationRead)+sizeof(EffectInvocationCertificate)+3*sizeof(void *)))
        return XR_XIR_BUDGET;
    EffectInvocationRead owner={0};
    XrXirStatus opening=effect_invocation_read_open(work,summary,certificate->declared,certificate->final,&owner);
    if (opening!=XR_XIR_OK) return opening;
    EffectInvocationCertificate local=*certificate;local.terms.remaining=work;
    XrXirStatus status=effect_source_equation_shape(work,certificate);
    if (status==XR_XIR_OK) status=effect_source_result_views(work,certificate,summary);
    for (uint32_t f=0;f<summary->count && status==XR_XIR_OK;++f) {
        const XrXirFunctionEffectContract *contract=&effect_contracts_read(summary)[f];
        uint32_t parameters=certificate->body->functions[f].parameter_count;
        if (!xir_compile_work(work,2)) { status=XR_XIR_BUDGET;break; }
        if (contract->parameter_count!=parameters || !!contract->parameters!=!!parameters) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        for (uint32_t p=0;p<parameters && status==XR_XIR_OK;++p) {
            if (!xir_compile_work(work,2)) { status=XR_XIR_BUDGET;break; }
            if (certificate->parameter_kinds[f][p]!=contract->parameters[p].kind)
                status=XR_XIR_BAD_STRUCTURE;
        }
    }
    if (status==XR_XIR_OK) status=effect_body_physical_match(work,owner.final->physical,owner.final->material);
    if (status==XR_XIR_OK) status=effect_invocation_owned_proofs_read(&owner,&owner.body);
    if (status==XR_XIR_OK) status=effect_invocation_closed_read(work,certificate->final);
    if (status==XR_XIR_OK) status=effect_invocation_deferred_verify(&owner);
    if (status==XR_XIR_OK) status=effect_invocation_bind_bounds(&local,certificate->declared,owner.final->physical);
    if (status==XR_XIR_OK) status=effect_source_atoms(work,&local);
    if (status==XR_XIR_OK) status=effect_source_projections(work,&local);
    effect_invocation_read_close(&owner);return status;
}

static XrXirStatus effect_source_contexts(const XrXirCompileContext *work,
    const XrXirModule *source,const XrXirEffects *effects) {
    if (!source->declarations || !source->declarations->functions)
        return effect_contexts_read(effects)?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
    /* Context construction uses this same canonical empty type domain.
     * Normalize only the request view; the complete source input was matched
     * independently before this terminal, and every context is still replayed. */
    XrXirModule normalized;
    if (!source->types) {
        if (!xir_compile_work(work,sizeof(normalized)+2*sizeof(void *))) return XR_XIR_BUDGET;
        normalized=*source;normalized.types=&effect_empty_types;source=&normalized;
    }
    const EffectContextOwner *owner=effect_contexts_read(effects);
    if (!owner || !owner->ordinary || !owner->dense || !owner->forest || owner->refinement ||
        owner->ordinary->terms.remaining || owner->dense->terms.remaining || owner->forest->terms.remaining)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,21)) return XR_XIR_BUDGET;
    if (!effect_terms_scratch_retired(&owner->ordinary->terms) ||
        !effect_terms_scratch_retired(&owner->dense->terms) || !effect_terms_scratch_retired(&owner->forest->terms))
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,2*sizeof(EffectTerms))) return XR_XIR_BUDGET;
    EffectTerms ordinary={.types=owner->ordinary->terms.types,.remaining=work};
    EffectTerms dense={.types=owner->dense->terms.types,.remaining=work};
    const EffectInvocationCertificate *certificate=owner->forest->invocations;
    XrXirStatus status=effect_context_dense_stable_read(work,owner,source,certificate,&ordinary,&dense);
    if (status==XR_XIR_OK) status=effect_source_certificate(work,certificate,&owner->dense->uses);
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
        if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
        if (origin->function!=f || origin->argument_count || origin->arguments ||
            certificate->owners[f]!=parent) return XR_XIR_BAD_STRUCTURE;
        const EffectFinalFunctions *result=(&certificate->final->summary);
        const XrXirRootFormula *a=&public->contracts[f].formula,*b=&result->functions[f].formula;
        if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
        if (a->constant_mask!=b->constant_mask || a->term_count!=b->term_count ||
            !!a->terms!=!!a->term_count) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t t=0;t<a->term_count;++t) {
            if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
            if (a->terms[t].kind!=b->terms[t].kind || a->terms[t].index!=b->terms[t].index)
                return XR_XIR_BAD_STRUCTURE;
        }
        for (uint32_t d=0;d<4;++d) {
            const XrXirRootEffectWitness *expected=&result->witnesses[(size_t)d*effects->count+f];
            if (!xir_compile_work(work,10)) return XR_XIR_BUDGET;
            if (!effect_invocation_witness_equal(expected,&effects->formula_terminals[(size_t)d*effects->count+f]) ||
                (d>=2 && !effect_invocation_witness_equal(expected,
                    &effects->constant_witnesses[(size_t)(d-2)*effects->count+f]))) return XR_XIR_BAD_STRUCTURE;
        }

    }
    return effect_public_root_match(work,effects);
}

/* Control and Error permissions retain their independent original algorithms.
 * These request-owned outputs borrow the already fully matched body and ROOT
 * certificate, while their real allocations, scans and fixed points are paid. */
static void effect_source_outcomes_free(XrXirEffects *fresh) {
    effect_outcomes_clear(&fresh->outcomes_work,fresh->count);
}

static XrXirStatus effect_source_outcomes_prepare(const XrXirCompileContext *work,
    const XrXirEffects *effects,XrXirEffects *fresh) {
    const EffectFinalOutcomes *actual=effect_final_outcomes(effects);
    if (!actual) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,sizeof(*fresh))) return XR_XIR_BUDGET;
    *fresh=(XrXirEffects){.resources=work->resources,.count=effects->count,.root_input=actual,
        .invocations=effects->invocations,
        .result_borrow=effects->result_borrow,.generation=effects->invocations->body,
        .cells=effects->cells};
    fresh->outcomes_work.source_type_count=actual->source_type_count;
    XrXirStatus status=XR_XIR_OK;
    fresh->outcomes_work.functions=xir_compile_calloc(work,fresh->count,sizeof(*fresh->outcomes_work.functions),&status);
    if (status==XR_XIR_OK) fresh->outcomes_work.task_errors=xir_compile_calloc(work,fresh->count,sizeof(*fresh->outcomes_work.task_errors),&status);
    return status;
}

static XrXirStatus effect_source_outcomes_match(const XrXirCompileContext *work,
    const XrXirEffects *effects,const XrXirEffects *fresh) {
    const EffectFinalOutcomes *actual=effect_final_outcomes(effects);
    if (!actual || !actual->functions || !actual->task_errors) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<effects->count;++f) {
        if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
        if (actual->functions[f].throws!=fresh->outcomes_work.functions[f].throws ||
            actual->task_errors[f]!=fresh->outcomes_work.task_errors[f]) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_source_errors_match(const XrXirCompileContext *work,
    const XrXirEffects *effects,const XrXirEffects *fresh) {
    const EffectFinalOutcomes *actual=effect_final_outcomes(effects);
    if (!actual) return XR_XIR_BAD_STRUCTURE;
    if (actual->error_rows!=fresh->outcomes_work.error_rows ||
        actual->atom_count!=fresh->outcomes_work.atom_count || actual->words!=fresh->outcomes_work.words ||
        actual->atom_count>actual->atom_capacity || !!actual->atoms!=!!actual->atom_capacity ||
        !actual->errors || !fresh->outcomes_work.errors) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t a=0;a<actual->atom_count;++a) {
        if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
        if (actual->atoms[a].type!=fresh->outcomes_work.atoms[a].type || actual->atoms[a].variant!=fresh->outcomes_work.atoms[a].variant)
            return XR_XIR_BAD_STRUCTURE;
    }
    uint64_t words=(uint64_t)actual->error_rows*actual->words;
    for (uint64_t w=0;w<words;++w) {
        if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
        if (actual->errors[w]!=fresh->outcomes_work.errors[w]) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_source_outcomes(const XrXirCompileContext *work,const XrXirEffects *effects) {
    XrXirCompileContext current=*work;XrXirEffects fresh={0};EffectControlCheck input={0};
    EffectGraph graph={.material=effects->invocations->body,.physical=effects->invocations->body->physical};
    if (!xir_compile_work(work,sizeof(current)+sizeof(fresh)+sizeof(graph)+sizeof(input))) return XR_XIR_BUDGET;
    XrXirStatus status=effect_final_control_prepare(work,effects,&input);
    if (status==XR_XIR_OK) status=effect_control_dependencies_move(work,&input,&graph);
    if (status==XR_XIR_OK) status=effect_source_outcomes_prepare(work,effects,&fresh);
    if (status==XR_XIR_OK) status=effect_errors_analyze(&fresh,&graph,&current);
    if (status==XR_XIR_OK) status=effect_source_outcomes_match(work,effects,&fresh);
    if (status==XR_XIR_OK) status=effect_source_errors_match(work,effects,&fresh);
    effect_graph_free(&graph);effect_control_check_free(&input);effect_source_outcomes_free(&fresh);return status;
}

XR_FUNC XrXirStatus xir_effects_source_terminal(const XrXirCompileContext *work,
    const XrXirModule *source,const XrXirEffects *effects) {
    if (!xir_effects_context_matches(work,effects,source?source->function_count:0) || !source ||
        source->stage!=XR_XIR_CHECKED || !source->provenance ||
        source->provenance->kind!=XR_XIR_EVIDENCE_TEMPLATE || !effects->invocations ||
        effects->generation || effects->refinement) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_source_certificate(work,effects->invocations,effects);
    if (status==XR_XIR_OK) status=effect_source_contexts(work,source,effects);
    if (status==XR_XIR_OK) status=effect_source_publication_match(work,source,effects);
    if (status==XR_XIR_OK) status=effect_source_outcomes(work,effects);
    if (status==XR_XIR_OK) status=effect_invocation_current_input_match(work,source,effects->invocations->body);
    return status;
}
