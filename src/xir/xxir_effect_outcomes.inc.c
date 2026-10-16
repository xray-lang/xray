/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_outcomes.inc.c - Sole Final ownership of control and error results
 *
 * KEY CONCEPT:
 *   Construction moves the actual allocations once. Published reads cannot
 *   recover provisional fields or borrow a constructor's mutable inventory.
 */
static bool effect_outcomes_empty(const EffectFinalOutcomes *outcomes) {
    return !outcomes->resources && !outcomes->count && !outcomes->atom_count &&
        !outcomes->atom_capacity && !outcomes->words && !outcomes->source_type_count &&
        !outcomes->error_rows && !outcomes->functions && !outcomes->witnesses &&
        !outcomes->atoms && !outcomes->errors && !outcomes->task_errors &&
        !outcomes->go_safe && !outcomes->task_creation;
}

/* Shape rejects invalid requests. Fresh receiving verification separately
 * compares all current Input fields and every reconstructed result row. */
static const EffectFinalOutcomes *effect_final_outcomes(const XrXirEffects *effects) {
    if (!effects || !effects->invocations || !effects->invocations->final ||
        effects->invocations->resources!=effects->resources ||
        effects->outcomes_owner || !effect_outcomes_empty(&effects->outcomes_work)) return NULL;
    const EffectFinalOutcomes *final=effects->invocations->final->outcomes;
    if (!final || final->resources!=effects->resources || final->count!=effects->count ||
        !final->count || !final->functions || !final->witnesses || !final->errors ||
        !final->task_errors || !final->go_safe || !final->task_creation ||
        final->atom_count>final->atom_capacity || !!final->atoms!=!!final->atom_capacity ||
        final->words!=((uint64_t)final->atom_count+65)/64 || final->error_rows<final->count ||
        (uint64_t)final->error_rows>(uint64_t)final->count+effects->invocations->final->count)
        return NULL;
    return final;
}

/* All fallible work precedes the ownership move. No allocation or callback
 * separates the last current-input comparison from the only public owner. */
static XrXirStatus effect_outcomes_publish(const XrXirCompileContext *work,
    const XrXirModule *source,XrXirEffects *effects) {
    if (!xir_effects_context_matches(work,effects,source?source->function_count:0) ||
        !effects->invocations || !effects->invocations->final ||
        effects->invocations->final->outcomes || !effects->outcomes_owner ||
        !effect_outcomes_empty(effects->outcomes_owner)) return XR_XIR_BAD_STRUCTURE;
    EffectFinalOutcomes *records=&effects->outcomes_work;
    if (records->resources || records->count || !records->functions || !records->witnesses ||
        !records->errors || !records->task_errors || !records->go_safe || !records->task_creation ||
        records->error_rows<effects->count || records->atom_count>records->atom_capacity ||
        !!records->atoms!=!!records->atom_capacity || records->words!=((uint64_t)records->atom_count+65)/64)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_invocation_current_input_match(work,source,effects->invocations->body);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(work,2*sizeof(*records)+sizeof(records->resources)+sizeof(records->count)))
        return XR_XIR_BUDGET;
    EffectFinalOutcomes *final=effects->outcomes_owner;
    *final=*records;final->resources=effects->resources;final->count=effects->count;
    effects->invocations->final->outcomes=final;effects->outcomes_owner=NULL;
    *records=(EffectFinalOutcomes){0};
    return XR_XIR_OK;
}
