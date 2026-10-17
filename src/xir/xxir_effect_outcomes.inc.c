/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_outcomes.inc.c - Sole Final ownership of public effect outcomes
 *
 * KEY CONCEPT:
 *   Construction moves the actual allocations once. Published reads cannot
 *   recover provisional fields or borrow a constructor's mutable inventory.
 */
static bool effect_outcomes_empty(const EffectFinalOutcomes *outcomes) {
    return !outcomes->resources && !outcomes->count && !outcomes->atom_count &&
        !outcomes->atom_capacity && !outcomes->words && !outcomes->source_type_count &&
        !outcomes->error_rows && !outcomes->functions && !outcomes->witnesses &&
        !outcomes->atoms && !outcomes->errors && !outcomes->error_proof && !outcomes->control_proof && !outcomes->task_errors &&
        !outcomes->go_safe && !outcomes->task_creation && !outcomes->root &&
        !outcomes->root_witnesses && !outcomes->unresolved_witnesses &&
        !outcomes->contracts && !outcomes->entry_roots && !outcomes->contexts;
}

/* Shape rejects invalid requests. Fresh receiving verification separately
 * compares all current Input fields and every reconstructed result row. */
static const EffectFinalOutcomes *effect_final_outcomes(const XrXirEffects *effects) {
    if (!effects || !effects->invocations || !effects->invocations->final ||
        effects->invocations->resources!=effects->resources ||
        effects->root_input || effects->outcomes_owner || !effect_outcomes_empty(&effects->outcomes_work)) return NULL;
    const EffectFinalOutcomes *final=effects->invocations->final->outcomes;
    if (!final || final->resources!=effects->resources || final->count!=effects->count ||
        !final->count || !final->functions || !final->witnesses || !final->errors ||
        !final->task_errors || !final->go_safe || !final->task_creation ||
        !final->root || !final->root_witnesses || !final->unresolved_witnesses ||
        !final->contracts || !final->entry_roots || !final->control_proof ||
        final->atom_count>final->atom_capacity || !!final->atoms!=!!final->atom_capacity ||
        final->words!=((uint64_t)final->atom_count+65)/64 || final->error_rows<final->count ||
        (uint64_t)final->error_rows>(uint64_t)final->count+effects->invocations->final->count)
        return NULL;
    if (final->contexts && !effect_context_owner_published(effects->resources,final->contexts)) return NULL;
    return final;
}

/* All fallible work precedes the ownership move. No allocation or callback
 * separates the last current-input comparison from the only public owner. */
static XrXirStatus effect_outcomes_publish(const XrXirCompileContext *work,
    const XrXirModule *source,XrXirEffects *effects) {
    if (!xir_effects_context_matches(work,effects,source?source->function_count:0) ||
        !effects->invocations || !effects->invocations->final ||
        effects->invocations->final->outcomes || !effects->outcomes_owner || effects->root_input) return XR_XIR_BAD_STRUCTURE;
    EffectFinalOutcomes *records=&effects->outcomes_work;
    if (records->resources || records->count || !records->functions || !records->witnesses ||
        !records->errors || !records->task_errors || !records->go_safe || !records->task_creation ||
        !records->root || !records->root_witnesses || !records->unresolved_witnesses ||
        !records->contracts || !records->entry_roots ||
        records->error_rows<effects->count || records->atom_count>records->atom_capacity ||
        !!records->atoms!=!!records->atom_capacity || records->words!=((uint64_t)records->atom_count+65)/64)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=error_finite_construct(work,effects->invocations,records,&records->error_proof);
    if(status==XR_XIR_OK) status=effect_control_proof_build(work,effects->invocations,records,&records->control_proof);
    if(status==XR_XIR_OK) status=effect_final_retire_construction(work,effects);
    if(status==XR_XIR_OK) status=effect_final_inventories_match(work,effects->invocations,records);
    if(status==XR_XIR_OK) status=error_finite_outcomes_match(work,effects->invocations->body,records);
    if(status==XR_XIR_OK) status=effect_control_proof_check(work,effects->invocations,records);
    if (status==XR_XIR_OK) status=effect_invocation_current_input_match(work,source,effects->invocations->body);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(work,2*sizeof(*records)+sizeof(records->resources)+sizeof(records->count)))
        return XR_XIR_BUDGET;
    EffectFinalOutcomes *final=effects->outcomes_owner;
    *final=*records;final->resources=effects->resources;final->count=effects->count;
    effects->invocations->final->outcomes=final;effects->outcomes_owner=NULL;
    *records=(EffectFinalOutcomes){0};
    return XR_XIR_OK;
}

/* Presentation uses the actual dense context only for closed Source bodies.
 * An open generic keeps its original symbolic function namespace. */
static bool effect_public_root_context(const XrXirEffects *effects,uint32_t function) {
    const EffectBodyMaterial *body=effects->invocations->body;
    return effect_contexts_read(effects) && (!body->generics || !body->generics[function].parameter_count);
}

/* These public records are a semantic projection of the same immutable ROOT
 * proof. Every output is compared before a receiving trace allocates storage;
 * shape alone never blesses an overwritten permission or cause. */
static XrXirStatus effect_public_root_match(const XrXirCompileContext *work,const XrXirEffects *effects) {
    const EffectFinalOutcomes *actual=effect_final_outcomes(effects);
    if (!actual || !xir_effects_context_matches(work,effects,actual->count) ||
        !effect_body_material_shape(effects->invocations->body) ||
        effect_body_count(effects->invocations->body)!=actual->count) return XR_XIR_BAD_STRUCTURE;
    const EffectFinalFunctions *summary=&effects->invocations->final->summary;
    if (summary->resources!=work->resources || summary->count!=actual->count ||
        summary->capacity!=summary->count || !summary->witnesses)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<actual->count;++f) {
        XrXirRootEffects expected;
        XrXirStatus status=effect_invocation_certificate_facts(work,effects->invocations,f,&expected);
        if(status!=XR_XIR_OK) return status;
        const XrXirRootEffects *facts=&expected;
        const XrXirRootEffectWitness *root=&summary->witnesses[f];
        const XrXirRootEffectWitness *unknown=&summary->witnesses[summary->count+f];
        EffectContextRootValue value;
        if (effect_public_root_context(effects,f)) {
            const EffectContextForest *forest=effect_contexts_read(effects)->forest;
            status=effect_invocation_forest_values(work,forest,f,&value);
            if(status!=XR_XIR_OK) return status;
            facts=&value.facts;root=&value.root;unknown=&value.unresolved;
        }
        if (!xir_compile_work(work,24)) return XR_XIR_BUDGET;
        if (actual->root[f].requires_root!=facts->requires_root || actual->root[f].unresolved!=facts->unresolved ||
            !effect_invocation_witness_equal(&actual->root_witnesses[f],root) ||
            !effect_invocation_witness_equal(&actual->unresolved_witnesses[f],unknown)) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
