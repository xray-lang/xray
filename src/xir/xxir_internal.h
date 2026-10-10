/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_internal.h - Private ownership of immutable XIR artifacts
 *
 * KEY CONCEPT:
 *   Only checked stage transitions construct physical layout tables.
 */

#ifndef XXIR_INTERNAL_H
#define XXIR_INTERNAL_H

#include "xxir.h"
#include "xxir_checked.h"
#include "xxir_effects.h"
#include "xxir_effect_contract_internal.h"
#include "xxir_construction_internal.h"

/* Requires verified source and type pools; proves function correspondence and instance closure. */
XR_FUNC XrXirStatus xr_xir_compile_provenance_functions_match(const XrXirCompileContext *compile_context, const XrXirModule *source, const XrXirModule *destination, const XrXirProvenance *instance, XrXirDiagnostic *diagnostic);
XR_FUNC void xr_xir_compile_provenance_free(XrXirProvenance *provenance);
XR_FUNC XrXirStatus xr_xir_compile_provenance_copy(const XrXirCompileContext *compile_context,
    const XrXirProvenance *source, XrXirProvenance **output);
/* Metadata shape cannot authorize substitutions; the common owner derives facts. */
XR_FUNC XrXirStatus xir_effect_contract_shape_verify(const XrXirCompileContext *context,
    const XrXirModule *module);
/* Called only after an ordinary scalar direct/capture argument match BAD_TYPE.
 * This bounded candidate is not proof; the same-owner final gate derives it. */
XR_FUNC XrXirStatus xir_effect_call_binding_candidate(const XrXirCompileContext *context,
    const XrXirModule *module, uint32_t caller, uint32_t instruction, uint32_t parameter);
typedef struct XirEffectCallableBound {
    const XrXirTypes *source, *destination;
    const XrXirType *arguments;
    uint32_t argument_count;
    XrXirType declared, actual;
} XirEffectCallableBound;
/* Outer callable bounds alone may weaken; all nested substitutions are exact.
 * This structural relation is not an effect-instance or permission proof. */
XR_FUNC XrXirStatus xir_effect_callable_bound_matches(const XrXirCompileContext *context,
    const XirEffectCallableBound *request);
/* These values borrow only the current verified effects owner. */
XR_FUNC const XrXirFunctionEffectContract *xir_effects_contract(
    const XrXirEffects *effects, uint32_t function);
/* A ledger identity guard grants no source, type or execution proof. */
XR_FUNC bool xir_effects_context_matches(const XrXirCompileContext *context,
    const XrXirEffects *effects, uint32_t functions);
XR_FUNC XrXirStatus xir_effects_parameters_match(const XrXirCompileContext *context,
    const XrXirModule *module, const XrXirEffects *effects);

/* Requires verified source and pools plus that source's freshly derived owner.
 * The actual origins and capture operands are correspondence, never authority. */
typedef struct XirEffectCaptureRequest {
    const XrXirModule *source;
    const XrXirTypes *types;
    const XrXirFunction *actual;
    const XrXirEffects *effects;
    const XrXirOrigin *caller, *target;
    uint32_t instruction;
    const XrXirOrigin *owners;
    uint32_t owner_count;
} XirEffectCaptureRequest;
typedef struct XirEffectCaptureBound { uint32_t flags; bool refined; } XirEffectCaptureBound;
XR_FUNC XrXirStatus xir_effect_capture_bound(const XrXirCompileContext *context,
    const XirEffectCaptureRequest *request, XirEffectCaptureBound *output);

struct XrXirArtifact {
    XrXirModule module;
    XrXirConstruction *construction;
    XrXirCompileContext context;
    XrXirTarget target;
    XrXirFunctionLayout *layouts;
    XrXirCheckedPacket checked_packet;
    uint8_t checked_identity[32];
};

/* Requires completed structural and type verification; never re-enters verification. */
XR_FUNC XrXirStatus xr_xir_compile_effects_infer_verified(const XrXirCompileContext *compile_context, const XrXirModule *module, XrXirEffects **output);
/* Private construction preparation grants no artifact or execution authority. */
typedef struct XrXirRootRefiner {
    void *context;
    XrXirStatus (*update)(void *, const XrXirEffects *, bool *);
} XrXirRootRefiner;
XR_FUNC XrXirStatus xr_xir_compile_structure_verify_v2(const XrXirCompileContext *context,
    const XrXirModule *module, const XrXirConstruction *construction, XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_compile_verify_effects_v2(const XrXirCompileContext *context,
    const XrXirModule *module, const XrXirConstruction *construction, const XrXirEffects *effects, XrXirDiagnostic *diagnostic);
/* Returns only the same complete checker's fresh, owned Template derivation.
 * Output must be empty. Failure preserves it; success transfers ownership to
 * this private producer call. Neither entry accepts prepared caller facts. */
XR_FUNC XrXirStatus xr_xir_compile_verify_owned_effects_v2(const XrXirCompileContext *context,
    const XrXirModule *module, const XrXirConstruction *construction,
    XrXirEffects **output, XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_compile_artifact_verify_owned_effects(const XrXirArtifact *artifact,
    XrXirEffects **output, XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_compile_effects_refine_verified(const XrXirCompileContext *context,
    const XrXirModule *module, const XrXirRootRefiner *refiner, XrXirEffects **output);
XR_FUNC XrXirStatus xr_xir_compile_check_refined_v2(const XrXirCompileContext *context,
    const XrXirModule *built, const XrXirConstruction *construction, const XrXirRootRefiner *refiner,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic);
/* A derived complete-outcome fact grants no call or source permissions.
 * Checked import and specialization rebuild it from authentic declarations. */
XR_FUNC XrXirStatus xr_xir_effects_task_errors(const XrXirEffects *effects, uint32_t function);
/* Derived from the verified direct graph; neither fact is wire authority. */
XR_FUNC XrXirStatus xr_xir_effects_go_safe(const XrXirEffects *effects, uint32_t function);
XR_FUNC XrXirEffect xr_xir_effects_task_creation(const XrXirEffects *effects, uint32_t function);

XR_FUNC XrXirStatus xr_xir_compile_layout_build(XrXirArtifact *artifact);
XR_FUNC XrXirStatus xr_xir_compile_recheck_v2(const XrXirCompileContext *compile_context, const XrXirModule *checked, const XrXirConstruction *construction, XrXirArtifact **output, XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_compile_layout_verify(const XrXirArtifact *artifact);

/* Couples fresh owned decoding and its full verification to immediate lowering.
 * Accepts bytes only; no published artifact grants a verification exemption. */
XR_FUNC XrXirStatus xr_xir_compile_checked_read_lower(const XrXirCompileContext *context,
    const void *bytes, size_t length, const XrXirTarget *target,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic);


#endif // XXIR_INTERNAL_H
