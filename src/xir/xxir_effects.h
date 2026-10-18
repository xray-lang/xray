/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effects.h - Owned control facts derived from verified XIR
 */
#ifndef XXIR_EFFECTS_H
#define XXIR_EFFECTS_H
#include "xxir.h"
typedef enum XrXirEffect { XR_XIR_EFFECT_NONE, XR_XIR_EFFECT_UNKNOWN, XR_XIR_EFFECT_MAY } XrXirEffect;
typedef struct XrXirFunctionEffects { XrXirEffect suspend, throws; } XrXirFunctionEffects;

typedef enum XrXirEffectCause {
    XR_XIR_EFFECT_CAUSE_NONE, XR_XIR_EFFECT_CAUSE_SUSPEND,
    XR_XIR_EFFECT_CAUSE_INDIRECT, XR_XIR_EFFECT_CAUSE_CALL
} XrXirEffectCause;
typedef struct XrXirEffectWitness {
    XrXirEffectCause cause;
    uint32_t instruction, callee, distance;
} XrXirEffectWitness;

typedef struct XrXirRootEffects { bool requires_root, unresolved; } XrXirRootEffects;
typedef enum XrXirRootEffectCause {
    XR_XIR_ROOT_CAUSE_NONE, XR_XIR_ROOT_CAUSE_MUTABLE_SLOT,
    XR_XIR_ROOT_CAUSE_NON_SENDABLE_CONST, XR_XIR_ROOT_CAUSE_INITIALIZER,
    XR_XIR_ROOT_CAUSE_INDIRECT, XR_XIR_ROOT_CAUSE_REQUIREMENT,
    XR_XIR_ROOT_CAUSE_CALL, XR_XIR_ROOT_CAUSE_CLEANUP,
    XR_XIR_ROOT_CAUSE_PARAMETER, XR_XIR_ROOT_CAUSE_CONTEXT_CALL,
    XR_XIR_ROOT_CAUSE_CELL_ACCESS, XR_XIR_ROOT_CAUSE_CELL_PARAMETER
} XrXirRootEffectCause;
typedef struct XrXirRootEffectWitness {
    XrXirRootEffectCause cause;
    uint32_t instruction, callee, slot, distance;
} XrXirRootEffectWitness;
typedef struct XrXirEffects XrXirEffects;
typedef struct XrXirRootCauseTrace XrXirRootCauseTrace;
typedef struct XrXirRootCauseStep {
    uint32_t function, instruction, callee, slot, distance;
    XrXirRootEffectCause cause;
} XrXirRootCauseStep;
#define XR_XIR_ERROR_SYMBOLIC_VARIANT UINT32_MAX
/* Reverification and inference consume one cumulative work allowance.
 * Success publishes into an empty output; every failure preserves it. */
XR_FUNC XrXirStatus xr_xir_compile_effects_analyze(const XrXirArtifact *artifact, XrXirEffects **output);
/* Every query returns independent numeric values. Invalid queries carry an
 * explicit status; NONE witnesses have a zero cause in an admitted result. */
typedef struct XrXirEffectFacts {
    XrXirStatus status;
    XrXirFunctionEffects control;
    XrXirRootEffects root;
    XrXirEffectWitness suspend_witness;
    XrXirRootEffectWitness root_witness,unresolved_witness;
} XrXirEffectFacts;
XR_FUNC XrXirEffectFacts xr_xir_effects_read(const XrXirEffects *effects,uint32_t function);
/* Copy both existing cause chains into one independent owner on the same
 * compiler ledger. Invalid/occupied outputs consume no work and remain intact. */
XR_FUNC XrXirStatus xr_xir_compile_root_cause_trace_copy(
    const XrXirCompileContext *context, const XrXirEffects *effects,
    uint32_t function, XrXirRootCauseTrace **output);
/* Returned values borrow only the trace, which may outlive all input owners. */
XR_FUNC const XrXirRootEffects *xr_xir_root_cause_trace_facts(const XrXirRootCauseTrace *trace);
/* False selects the root chain; true selects the unresolved chain. An absent
 * chain writes zero; invalid trace/count arguments leave count untouched. */
XR_FUNC const XrXirRootCauseStep *xr_xir_root_cause_trace_steps(
    const XrXirRootCauseTrace *trace, bool unresolved, uint32_t *count);
XR_FUNC void xr_xir_compile_root_cause_trace_free(XrXirRootCauseTrace *trace);
/* Numeric type identities refer to the analyzed artifact, without borrowing it. */
XR_FUNC bool xr_xir_effects_error(const XrXirEffects *effects, uint32_t function,
    XrXirType type, uint32_t variant);
/* Constant-time worst-case work for one exact error lookup: two setup units
 * and at most three per immutable atom (iteration and two comparisons).
 * The receiving runtime prepays this bound before membership scanning. The
 * caller retains effects across both calls; neither call uses a compiler
 * ledger. Invalid owners, functions or outputs leave work untouched. */
XR_FUNC XrXirStatus xr_xir_effects_error_query_work(const XrXirEffects *effects,
    uint32_t function, uint64_t *work);
XR_FUNC bool xr_xir_effects_error_unidentified(const XrXirEffects *effects, uint32_t function);
XR_FUNC bool xr_xir_effects_error_unknown(const XrXirEffects *effects, uint32_t function);
XR_FUNC void xr_xir_compile_effects_free(XrXirEffects *effects);
#endif // XXIR_EFFECTS_H
