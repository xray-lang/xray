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
#include "xxir_effect_input_internal.h"
#include "xxir_lexical.h"

typedef enum XrXirEvidenceKind {
    XR_XIR_EVIDENCE_ABSENT = 0,
    XR_XIR_EVIDENCE_TEMPLATE = 1,
    XR_XIR_EVIDENCE_INSTANCE = 2,
    XR_XIR_EVIDENCE_EVALUATION = 3
} XrXirEvidenceKind;

typedef enum XrXirEffectParameterKind {
    XR_XIR_EFFECT_PARAMETER_FIXED = 0,
    XR_XIR_EFFECT_PARAMETER_VARIABLE = 1
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

/* Generic value arguments retain their actual callable identity. The fixed
 * storage contract does not make the binder a declared callable parameter. */
XR_FUNC bool xir_effect_parameter_identity(XrXirType, XrXirEffectParameterKind);

typedef struct XrXirEffectParameter {
    uint32_t kind;
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

typedef struct XirEvaluationRoot {
    uint32_t caller, instruction, callee;
} XirEvaluationRoot;

typedef struct XrXirProvenance {
    uint32_t kind;
    XrXirArtifact *source;
    XrXirOrigin *origins;
    uint32_t count;
    XrXirFunctionEffectContract *contracts;
    uint32_t contract_count;
    XrXirEffectBindingProof *bindings;
    uint32_t binding_count;
    XirEvaluationRoot evaluation;
    XirLexicalProgram lexical;
} XrXirProvenance;

/* Each raw correspondence entry replays the complete order/unique/range
 * inventory on its current input. These helpers publish membership only;
 * independent Source, destination, owner and permission checks still decide. */
XR_FUNC XrXirStatus xir_effect_binding_proofs_shape(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirProvenance *instance);
XR_FUNC XrXirStatus xir_effect_binding_proof_lookup(const XrXirCompileContext *context,
    const XrXirProvenance *instance,const XrXirEffectBindingProof *wanted,bool *output);

struct XrXirEffects;

/* The common verifier calls this with its freshly derived owner and the same
 * verified module. It consumes the real reference edge, never a target hint. */
XR_FUNC XrXirStatus xir_effects_reference_root_input(const XrXirCompileContext *context, const struct XrXirEffects *effects, XirEffectInput input, uint32_t function, uint32_t instruction, uint32_t *output);

/* A producer view names a real instruction from the sealed owned body. It
 * contains no permission bit, and a consumer keeps its Effects owner alive. */
typedef struct XirEffectProducerView {
    uint32_t site, function, instruction, target, capture_count;
    XrXirType type;
} XirEffectProducerView;

/* A dynamic class producer must match one exact independently sealed candidate.
 * Its runtime consumer separately proves the actual receiver's selected target. */

/* Runtime matching reads only existing completed equations. The callback
 * consumes the live runtime ledger, never the retired compile context. */
/* A request-local value is a borrowed physical handle, not a permission.
 * Child reads authenticate the same real parent before exposing its capture.
 * Iterator publication is zero only after authenticated Frame retirement;
 * active Iterator and all Function origins retain UINT32_MAX. */
typedef struct XirEffectRuntimeValue {
    uint32_t type,reserved,producer,publication;
    int64_t payload;
} XirEffectRuntimeValue;
typedef struct XirEffectRuntimeRead {
    void *owner;
    bool (*work)(void *owner,uint64_t units);
    uint32_t (*cell)(void *owner,uint32_t parameter);
    XrXirStatus (*class_value)(void *owner,uint32_t parameter,uint32_t *module_storage);
    XrXirStatus (*value)(void *owner,uint32_t parameter,const XirEffectRuntimeValue *parent,
        uint32_t capture,XirEffectRuntimeValue *output);
    void *(*allocate)(void *owner,size_t bytes,XrXirStatus *status);
    void (*deallocate)(void *owner,void *memory,size_t bytes);
} XirEffectRuntimeRead;
typedef enum XirEffectClassAccessKind {
    XIR_EFFECT_ACCESS_CELL_READ,XIR_EFFECT_ACCESS_CELL_WRITE,XIR_EFFECT_ACCESS_PROJECT,
    XIR_EFFECT_ACCESS_PATH_READ,XIR_EFFECT_ACCESS_PATH_WRITE,XIR_EFFECT_ACCESS_PUSH,
    XIR_EFFECT_ACCESS_LENGTH,XIR_EFFECT_ACCESS_CAPACITY
} XirEffectClassAccessKind;
typedef struct XirEffectClassAccessRequest {
    uint32_t caller,instruction,node;
    bool contextual,pure;
    XirEffectClassAccessKind kind;
    XrXirType input_type,root_type;
    XirEffectRuntimeRead read;
} XirEffectClassAccessRequest;
/* The genuine Instance keeps its Class reservation across this exact query. */

typedef struct XirEffectClassScopeRequest {
    uint32_t caller,node;
    bool contextual,pure;
    XrXirType root_type;
    XirEffectRuntimeRead read;
} XirEffectClassScopeRequest;
/* Paused control has no language instruction. The genuine Instance supplies
 * its actual frame selection and retains the Class reservation across replay. */

/* Original operation facts from the same sealed body, without a verdict. */
typedef struct XirEffectIteratorInstruction {uint32_t opcode;XrXirType receiver;} XirEffectIteratorInstruction;

typedef struct XirEffectInvocationRequest {
    uint32_t caller,instruction,target,producer,node,count,captures;
    bool contextual;
    XirEffectRuntimeRead read;
} XirEffectInvocationRequest;
typedef struct XirEffectInvocationSelection {
    const struct XrXirEffects *owner;
    uint32_t caller,instruction,target,producer,parent,node,edge;
} XirEffectInvocationSelection;

/* This private view reports complete owned Class sources. It grants no
 * execution permission and requires an empty scalar output. */

/* A discriminant is only a grammar choice. Complete checking proves authority. */
static inline bool xir_effect_evidence_is_instance(const XrXirModule *module) {
    return module && module->provenance &&
        module->provenance->kind == XR_XIR_EVIDENCE_INSTANCE;
}
static inline bool xir_effect_kind_is_closed(uint32_t kind) {
    return kind==XR_XIR_EVIDENCE_INSTANCE || kind==XR_XIR_EVIDENCE_EVALUATION;
}
static inline bool xir_effect_evidence_is_closed(const XrXirModule *module) {
    return module && module->provenance && xir_effect_kind_is_closed(module->provenance->kind);
}
static inline bool xir_effect_evidence_is_evaluation(const XrXirModule *module) {
    return module && module->provenance && module->provenance->kind==XR_XIR_EVIDENCE_EVALUATION;
}

/* Current call identity and types remain obligations of the admitted module.
 * The immutable semantic owner supplies only its numeric hidden-access fact. */
XR_FUNC XrXirStatus xir_effects_default_access_site(const XrXirCompileContext *context,
    const struct XrXirEffects *effects,XirEffectInput input,uint32_t caller,uint32_t instruction);
XR_FUNC XrXirStatus xir_effects_class_call_access_site(const XrXirCompileContext *context,
    const struct XrXirEffects *effects,XirEffectInput input,uint32_t caller,uint32_t instruction,XrXirType result);
#endif // XXIR_EFFECT_CONTRACT_INTERNAL_H
