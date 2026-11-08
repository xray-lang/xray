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
    return !outcomes->resources && !outcomes->count && !outcomes->functions && !outcomes->witnesses &&
        !outcomes->control_summary &&
        !outcomes->go_safe && !outcomes->task_creation && !outcomes->root &&
        !outcomes->root_witnesses && !outcomes->unresolved_witnesses &&
        !outcomes->contracts;
}

/* Shape rejects invalid requests. Fresh receiving verification separately
 * compares all current Input fields and every reconstructed result row. */
static const EffectFinalOutcomes *effect_final_outcomes(const XrXirEffects *effects) {
    if (!effects || !effects->checked_roots ||
        effects->outcomes_owner || !effect_outcomes_empty(&effects->outcomes_work)) return NULL;
    const EffectFinalOutcomes *final=effects->final_outcomes;
    if (!final || final->resources!=effects->resources || final->count!=effects->count ||
        !final->count || !final->functions || !final->witnesses ||
        !final->go_safe || !final->task_creation ||
        !final->root || !final->root_witnesses || !final->unresolved_witnesses ||
        !final->contracts || !final->control_summary)
        return NULL;
    return final;
}

/* All fallible work precedes the ownership move. No allocation or callback
 * separates the last current-input comparison from the only public owner. */
static XrXirStatus effect_outcomes_publish(const XrXirCompileContext *work,
    const XrXirModule *source,XrXirEffects *effects) {
    if (!xir_effects_context_matches(work,effects,source?source->function_count:0) ||
        !effects->checked_roots || effects->final_outcomes ||
        !effects->outcomes_owner) return XR_XIR_BAD_STRUCTURE;
    EffectFinalOutcomes *records=&effects->outcomes_work;
    if (records->resources || records->count || !records->functions || !records->witnesses ||
        !records->go_safe || !records->task_creation ||
        !records->root || !records->root_witnesses || !records->unresolved_witnesses ||
        !records->contracts || !records->control_summary)
        return XR_XIR_BAD_STRUCTURE;

    EffectFinalOutcomes *final=effects->outcomes_owner;
    *final=*records;final->resources=effects->resources;final->count=effects->count;
    effects->final_outcomes=final;effects->outcomes_owner=NULL;
    *records=(EffectFinalOutcomes){0};
    return XR_XIR_OK;
}

/* Presentation uses the actual dense context only for closed Source bodies.
 * An open generic keeps its original symbolic function namespace. */

/* These public records are a semantic projection of the same immutable ROOT
 * proof. Every output is compared before a receiving trace allocates storage;
 * shape alone never blesses an overwritten permission or cause. */
