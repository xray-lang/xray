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
/* The borrowed fact remains valid until its owning summary is freed. */
XR_FUNC const XrXirFunctionEffects *xr_xir_effects_function(const XrXirEffects *effects, uint32_t function);
/* NULL for NONE/invalid queries; call steps strictly decrease distance.
 * The summary owns numeric identities, not source locations or input storage. */
XR_FUNC const XrXirEffectWitness *xr_xir_effects_suspend_witness(const XrXirEffects *effects, uint32_t function);
/* These independent facts are owned by the same summary, without input borrows.
 * A complete false/false result proves only the admitted direct execution graph. */
XR_FUNC const XrXirRootEffects *xr_xir_effects_root(const XrXirEffects *effects, uint32_t function);
/* NULL for an absent fact or invalid query. Local terminals have distance zero;
 * call/cleanup steps strictly decrease distance and use analyzed numeric IDs. */
XR_FUNC const XrXirRootEffectWitness *xr_xir_effects_root_witness(const XrXirEffects *effects, uint32_t function);
XR_FUNC const XrXirRootEffectWitness *xr_xir_effects_unresolved_witness(const XrXirEffects *effects, uint32_t function);
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
XR_FUNC bool xr_xir_effects_error_unidentified(const XrXirEffects *effects, uint32_t function);
XR_FUNC bool xr_xir_effects_error_unknown(const XrXirEffects *effects, uint32_t function);
XR_FUNC void xr_xir_compile_effects_free(XrXirEffects *effects);
#endif // XXIR_EFFECTS_H
