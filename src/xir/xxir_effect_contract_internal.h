/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_contract_internal.h - Owned symbolic callable effect evidence
 *
 * KEY CONCEPT:
 *   Stored formulas and origins describe independently checked correspondence.
 *   Their presence does not grant declaration or execution authority.
 */

#ifndef XXIR_EFFECT_CONTRACT_INTERNAL_H
#define XXIR_EFFECT_CONTRACT_INTERNAL_H

#include "xxir.h"

typedef enum XrXirEvidenceKind {
    XR_XIR_EVIDENCE_ABSENT = 0,
    XR_XIR_EVIDENCE_TEMPLATE = 1,
    XR_XIR_EVIDENCE_INSTANCE = 2
} XrXirEvidenceKind;

typedef enum XrXirEffectParameterKind {
    XR_XIR_EFFECT_PARAMETER_FIXED = 0,
    XR_XIR_EFFECT_PARAMETER_VARIABLE = 1,
    XR_XIR_EFFECT_PARAMETER_CONDITIONAL_STATIC = 2
} XrXirEffectParameterKind;

typedef enum XrXirRootTermKind {
    XR_XIR_ROOT_TERM_PARAMETER = 1,
    XR_XIR_ROOT_TERM_CONTEXT_CALL = 2,
    XR_XIR_ROOT_TERM_CELL_PARAMETER = 3
} XrXirRootTermKind;

typedef enum XrXirEffectValueMode {
    XR_XIR_EFFECT_VALUE_FIXED = 1,
    XR_XIR_EFFECT_VALUE_PROPAGATE = 2,
    XR_XIR_EFFECT_VALUE_CALL_BIND = 3,
    XR_XIR_EFFECT_VALUE_AUTHENTIC_REF = 4
} XrXirEffectValueMode;

typedef enum XrXirEffectBindingFamily {
    XR_XIR_EFFECT_BINDING_DIRECT = 1,
    XR_XIR_EFFECT_BINDING_CAPTURE = 2,
    XR_XIR_EFFECT_BINDING_DEFAULT = 3,
    XR_XIR_EFFECT_BINDING_REQUIREMENT = 4
} XrXirEffectBindingFamily;

enum XrXirEffectParameterUse {
    XR_XIR_EFFECT_USE_INVOKE = 1,
    XR_XIR_EFFECT_USE_FORWARD = 2,
    XR_XIR_EFFECT_USE_COPY = 4,
    XR_XIR_EFFECT_USE_FIXED = 8,
    XR_XIR_EFFECT_USE_CONST_CAPTURE = 16,
    XR_XIR_EFFECT_USE_STORE = 32,
    XR_XIR_EFFECT_USE_RETURN = 64,
    XR_XIR_EFFECT_USE_CHILD = 128,
    XR_XIR_EFFECT_USE_OTHER = 256
};

typedef struct XrXirEffectParameter {
    uint32_t kind;
    uint32_t uses;
} XrXirEffectParameter;

typedef struct XrXirRootTerm {
    uint32_t kind;
    uint32_t index;
} XrXirRootTerm;

typedef struct XrXirRootFormula {
    uint32_t constant_mask;
    uint32_t term_count;
    const XrXirRootTerm *terms;
} XrXirRootFormula;

typedef struct XrXirRootValueIdentity {
    uint32_t instruction;
    uint32_t mode;
    XrXirType declared_type;
} XrXirRootValueIdentity;

typedef struct XrXirEffectCallBinding {
    uint32_t family;
    uint32_t instruction;
    uint32_t parameter;
    uint32_t value;
} XrXirEffectCallBinding;

typedef struct XrXirFunctionEffectContract {
    uint32_t parameter_count;
    const XrXirEffectParameter *parameters;
    XrXirRootFormula formula;
    uint32_t value_count;
    const XrXirRootValueIdentity *values;
    uint32_t binding_count;
    const XrXirEffectCallBinding *bindings;
} XrXirFunctionEffectContract;

typedef struct XrXirEffectArgument {
    uint32_t parameter;
    XrXirType type;
} XrXirEffectArgument;

typedef struct XrXirEffectBindingProof {
    uint32_t family;
    uint32_t caller;
    uint32_t instruction;
    uint32_t callee;
    uint32_t parameter;
    uint32_t actual_value;
} XrXirEffectBindingProof;

typedef struct XrXirOrigin {
    uint32_t function;
    const XrXirType *arguments;
    uint32_t argument_count;
    const XrXirEffectArgument *effect_arguments;
    uint32_t effect_argument_count;
} XrXirOrigin;

typedef struct XrXirProvenance {
    uint32_t kind;
    XrXirArtifact *source;
    XrXirOrigin *origins;
    uint32_t count;
    XrXirFunctionEffectContract *contracts;
    uint32_t contract_count;
    XrXirEffectBindingProof *bindings;
    uint32_t binding_count;
} XrXirProvenance;

struct XrXirEffects;

/* Context selection rechecks a full structural origin against the owned
 * source-derived graph. A returned view borrows only its Effects owner. */
typedef struct XirEffectContextInput {
    const XrXirModule *source;
    const XrXirTypes *types;
    const XrXirOrigin *origin;
    uint32_t instruction;
    const XrXirOrigin *owners;
    uint32_t owner_count;
} XirEffectContextInput;
typedef struct XirEffectContextView {
    const XrXirTypes *types;
    const XrXirFunction *function;
    const XrXirType *physical_types, *arguments;
    uint32_t declaration, parameter_count, argument_count;
    bool requires_root, unresolved;
} XirEffectContextView;
XR_FUNC XrXirStatus xir_effects_refinement_capture(const XrXirCompileContext *context,
    const XrXirModule *source, const struct XrXirEffects *effects);
XR_FUNC bool xir_effects_context_available(const XrXirCompileContext *context,
    const struct XrXirEffects *effects);
XR_FUNC XrXirStatus xir_effects_context_refresh(const XrXirCompileContext *context,
    const XrXirModule *source, const struct XrXirEffects *effects);
XR_FUNC XrXirStatus xir_effects_context_select(const XrXirCompileContext *context,
    const struct XrXirEffects *effects, const XirEffectContextInput *input, XirEffectContextView *output);
/* The common verifier calls this with its freshly derived owner and the same
 * verified module. It consumes the real reference edge, never a target hint. */
XR_FUNC XrXirStatus xir_effects_reference_root(const XrXirCompileContext *context,
    const struct XrXirEffects *effects, const XrXirModule *module,
    uint32_t function, uint32_t instruction, uint32_t *output);

/* Sealed entry facts borrow only Effects, never the source module or formula.
 * Intrinsic ROOT flags retain every non-Cell unknown; sorted physical Cell
 * parameter IDs are closed by real runtime arguments and existing loan checks.
 * The view itself grants no execution or reference authority. */
typedef struct XirEffectEntryRootView {
    uint32_t intrinsic_mask;
    const uint32_t *parameters;
    uint32_t parameter_count;
} XirEffectEntryRootView;
XR_FUNC XrXirStatus xir_effects_entry_root(const XrXirCompileContext *context,
    const struct XrXirEffects *effects, uint32_t function, XirEffectEntryRootView *output);

/* A discriminant is only a grammar choice. Complete checking proves authority. */
static inline bool xir_effect_evidence_is_instance(const XrXirModule *module) {
    return module && module->provenance &&
        module->provenance->kind == XR_XIR_EVIDENCE_INSTANCE;
}

#endif // XXIR_EFFECT_CONTRACT_INTERNAL_H
