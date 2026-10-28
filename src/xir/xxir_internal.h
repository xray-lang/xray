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

/* Structural admission precedes this synchronous declaration-local body check. */
XR_FUNC XrXirStatus xir_compile_local_errors_verify(const XrXirCompileContext *context,
    const XrXirModule *module,XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xir_effects_local_errors_verify(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirEffects *effects,XrXirDiagnostic *diagnostic);

/* Requires verified source and type pools; proves function correspondence and instance closure. */
XR_FUNC XrXirStatus xr_xir_compile_provenance_functions_match(const XrXirCompileContext *compile_context, const XrXirModule *source, const XrXirModule *destination, const XrXirProvenance *instance, XrXirDiagnostic *diagnostic);
/* Complete current Source admission and correspondence use a genuine
 * same-call derivation. A raw entry constructs its owner; the private producer
 * borrow still repeats every field, type, contract and permission gate. */
XR_FUNC XrXirStatus xr_xir_compile_provenance_verify(const XrXirCompileContext *context,
    const XrXirModule *destination, const XrXirProvenance *instance,
    const XrXirEffects *source_effects, XrXirDiagnostic *diagnostic);
XR_FUNC void xr_xir_compile_provenance_free(XrXirProvenance *provenance);
XR_FUNC XrXirStatus xr_xir_compile_provenance_copy(const XrXirCompileContext *compile_context,
    const XrXirProvenance *source, XrXirProvenance **output);
/* Only the same specialization call supplies the fresh owning derivation.
 * Full Source verification and physical correspondence precede every copy. */
XR_FUNC XrXirStatus xr_xir_compile_provenance_copy_source_effects(const XrXirCompileContext *context,
    const XrXirProvenance *source, const XrXirEffects *source_effects, XrXirProvenance **output);
XR_FUNC XrXirStatus xir_effects_source_body_match(const XrXirCompileContext *context,
    const XrXirModule *source, const XrXirEffects *effects);
/* Exact Lowered fields are compared without allocating or running callbacks.
 * This supplies correspondence only; independent semantic proof remains required. */
XR_FUNC XrXirStatus xir_effects_program_body_match(const XrXirCompileContext *context,
    const XrXirModule *source, const XrXirEffects *effects);
/* These borrows are compare-only metadata of the same owned semantic input;
 * current same-ledger payload bounds precede canonical metadata traversal.
 * Bounds grant no effect proof. Callers must compare every current field
 * before publishing a Program. */
XR_FUNC XrXirStatus xir_effects_program_metadata(const XrXirCompileContext *context,
    const XrXirEffects *effects, const XrXirTypes **types, const XrXirDeclarations **declarations);
XR_FUNC XrXirStatus xir_effects_program_signature_match(const XrXirCompileContext *context,
    const XrXirEffects *effects, uint32_t function, const XrXirType *parameters,
    uint32_t parameter_count, XrXirType result);
XR_FUNC XrXirStatus xir_effects_program_final_verify(const XrXirCompileContext *context,
    const XrXirModule *source,const XrXirEffects *effects);
XR_FUNC XrXirStatus xir_effects_program_layout_match(const XrXirCompileContext *context,
    const XrXirEffects *effects,const XrXirTarget *target,XrXirFunctionLayout *layouts);

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
/* Requires the real module's complete prior descriptor, generic-call,
 * binder/Unit-use and body admission. This helper does not reconstruct those
 * proofs from metadata. Definition-local structural matching of open callable
 * types: both pools
 * must be the authentic module pool; the function's full declaration context
 * supplies symbolic binder identities. This never proves execution authority
 * and cannot replace a real generic-call substitution vector. */
XR_FUNC XrXirStatus xir_effect_callable_bound_matches_definition(const XrXirCompileContext *context,
    const XrXirModule *module,uint32_t function,const XirEffectCallableBound *request);
/* These values borrow only the current verified effects owner. */
XR_FUNC const XrXirFunctionEffectContract *xir_effects_contract(
    const XrXirEffects *effects, uint32_t function);
/* Provisional ROOT copies belong only to genuine unpublished construction. */
XR_FUNC XrXirStatus xir_effects_construction_root(const XrXirCompileContext *context,
    const XrXirEffects *effects,uint32_t function,XrXirRootEffects *output);
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

typedef struct XirSourceFacts XirSourceFacts;

struct XrXirArtifact {
    XrXirModule module;
    XrXirConstruction *construction;
    XrXirCompileContext context;
    XrXirTarget target;
    XrXirFunctionLayout *layouts;
    XrXirCheckedPacket checked_packet;
    uint8_t checked_identity[32];
    XirSourceFacts *source_facts;
    XrXirEffects *program_final;
    uint32_t evaluation_only;
};

/* Purpose is a grammar fence, never a proof. Current full correspondence and
 * the unique evaluation root are independently verified on every operation. */
static inline bool xir_artifact_is_evaluation(const XrXirArtifact *artifact) {
    return artifact && (artifact->evaluation_only || xir_effect_evidence_is_evaluation(&artifact->module));
}
/* The private packet grammar cannot enter ordinary Checked transport or a
 * runtime Program. It owns the same complete current semantic field traversal. */
XR_FUNC XrXirStatus xir_evaluation_write(const XrXirArtifact *artifact,
    XrXirCheckedPacket *output,XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xir_evaluation_read(const XrXirCompileContext *context,
    const void *bytes,size_t length,XrXirArtifact **output,XrXirDiagnostic *diagnostic);

/* Immutable Source inputs are owned only by a genuine final Checked Template.
 * Acquisition repeats full current field/permission and independent terminal
 * proof on that same live ledger. The returned lifetime lease is no permission;
 * each later Artifact operation still performs its complete fresh admission. */
XR_FUNC XrXirStatus xir_source_facts_acquire(const XrXirArtifact *artifact,
    XirSourceFacts **output,XrXirDiagnostic *diagnostic);
XR_FUNC void xir_source_facts_release(XirSourceFacts *facts);
XR_FUNC const XrXirEffects *xir_source_facts_effects(const XirSourceFacts *facts);
/* Only the private decoder calls this on its unpublished complete EOF owner. */
XR_FUNC XrXirStatus xir_source_facts_prepare_decoded(XrXirArtifact *artifact,
    XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xir_effects_source_terminal(const XrXirCompileContext *context,
    const XrXirModule *source,const XrXirEffects *effects);

/* Requires completed structural and type verification; never re-enters verification. */
XR_FUNC XrXirStatus xr_xir_compile_effects_infer_verified(const XrXirCompileContext *compile_context, const XrXirModule *module, XrXirEffects **output);
/* Private construction preparation grants no artifact or execution authority. */
typedef struct XrXirRootRefiner {
    void *context;
    XrXirStatus (*update)(void *, const XrXirEffects *, bool *);
} XrXirRootRefiner;
XR_FUNC XrXirStatus xr_xir_compile_structure_verify_v2(const XrXirCompileContext *context,
    const XrXirModule *module, const XrXirConstruction *construction, XrXirDiagnostic *diagnostic);
/* These current readers consume genuine canonical Input and Final storage. */
XR_FUNC XrXirStatus xir_effects_program_root_current(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirEffects *effects);
XR_FUNC XrXirStatus xir_effects_program_cells_match(const XrXirCompileContext *context,
    const XrXirEffects *effects,XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xir_compile_verify_program_effects(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirConstruction *construction,
    const XrXirEffects *effects,XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_compile_verify_effects_v2(const XrXirCompileContext *context,
    const XrXirModule *module, const XrXirConstruction *construction, const XrXirEffects *effects, XrXirDiagnostic *diagnostic);
/* This same-call Source borrow is never a verification exemption. Fresh
 * structure, body/domain correspondence and all permission obligations run. */
XR_FUNC XrXirStatus xr_xir_compile_verify_source_effects_v2(const XrXirCompileContext *context,
    const XrXirModule *source, const XrXirConstruction *construction,
    const XrXirEffects *effects, XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_compile_verify_provenance_effects_v2(const XrXirCompileContext *context,
    const XrXirModule *module, const XrXirConstruction *construction,
    const XrXirEffects *source_effects, XrXirDiagnostic *diagnostic);
/* Returns only the same complete checker's fresh, owned Template derivation.
 * Output must be empty. Failure preserves it; success transfers ownership to
 * this private producer call. Neither entry accepts prepared caller facts. */
XR_FUNC XrXirStatus xr_xir_compile_verify_owned_effects_v2(const XrXirCompileContext *context,
    const XrXirModule *module, const XrXirConstruction *construction,
    XrXirEffects **output, XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_compile_artifact_verify_owned_effects(const XrXirArtifact *artifact,
    XrXirEffects **output, XrXirDiagnostic *diagnostic);
/* Complete analysis collects this call's fresh owner for every admitted stage.
 * It accepts no prepared facts and publishes only after all obligations pass. */
XR_FUNC XrXirStatus xr_xir_compile_verify_receiving_effects_v2(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirConstruction *construction,
    const XrXirEffects *source_effects,XrXirEffects **output,XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_compile_verify_analyzed_effects_v2(const XrXirCompileContext *context,
    const XrXirModule *module, const XrXirConstruction *construction,
    XrXirEffects **output, XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_compile_effects_refine_verified(const XrXirCompileContext *context,
    const XrXirModule *module, const XrXirRootRefiner *refiner, XrXirEffects **output);
XR_FUNC XrXirStatus xr_xir_compile_check_refined_v2(const XrXirCompileContext *context,
    const XrXirModule *built, const XrXirConstruction *construction, const XrXirRootRefiner *refiner,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic);
/* A derived complete-outcome fact grants no call or source permissions.
 * Checked import and specialization rebuild it from authentic declarations. */
XR_FUNC XrXirStatus xir_compile_worker_error_verify(const XrXirCompileContext *context,
    const XrXirModule *module,uint32_t function);
/* Derived from the verified direct graph; neither fact is wire authority. */
XR_FUNC XrXirStatus xr_xir_effects_go_safe(const XrXirEffects *effects, uint32_t function);
XR_FUNC XrXirEffect xr_xir_effects_task_creation(const XrXirEffects *effects, uint32_t function);

XR_FUNC XrXirStatus xr_xir_compile_layout_build(XrXirArtifact *artifact, XrCompileRequestControl *control);
XR_FUNC XrXirStatus xr_xir_compile_recheck_v2(const XrXirCompileContext *compile_context, const XrXirModule *checked, const XrXirConstruction *construction, XrXirArtifact **output, XrXirDiagnostic *diagnostic);
/* Rejection preserves both slots. Acceptance clears the unique provenance
 * slot and every subsequent failure releases that owner. The main body and
 * construction are still cloned; source and final receiving proofs are fresh.
 * The provenance must have been allocated on this same finite ledger. */
typedef struct XirProvenanceReceiving {
    const XrXirConstruction *construction;
    const XrXirEffects *source_effects;
} XirProvenanceReceiving;
XR_FUNC XrXirStatus xr_xir_compile_recheck_provenance_take(const XrXirCompileContext *context,
    const XrXirModule *checked, const XirProvenanceReceiving *receiving,
    XrXirProvenance **owned_provenance, XrXirArtifact **output, XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_compile_layout_verify(const XrXirArtifact *artifact);
/* Only the complete artifact checker in this call supplies the metadata
 * admission. Every physical row and function bound is still recomputed. */
XR_FUNC XrXirStatus xr_xir_compile_layout_match_verified(const XrXirArtifact *artifact);

/* Request rejection preserves both slots and charges no work. Acceptance
 * clears the unique input slot; every later failure releases that owner.
 * Same-slot and occupied-output aliases are rejected before acceptance.
 * The caller must own the Artifact exclusively; all full proofs still run. */
XR_FUNC XrXirStatus xr_xir_compile_lower_take(XrXirArtifact **owned_input,
    const XrXirTarget *target, XrCompileRequestControl *control, XrXirArtifact **output, XrXirDiagnostic *diagnostic);

/* Couples fresh owned decoding and its full verification to immediate lowering.
 * Accepts bytes only; no published artifact grants a verification exemption. */
XR_FUNC XrXirStatus xr_xir_compile_checked_read_lower(const XrXirCompileContext *context,
    const void *bytes, size_t length, const XrXirTarget *target,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic);


#endif // XXIR_INTERNAL_H
