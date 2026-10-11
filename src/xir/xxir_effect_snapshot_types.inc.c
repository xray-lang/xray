/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_snapshot_types.inc.c - Complete immutable certificate body storage
 *
 * KEY CONCEPT:
 *   Every physical word and ordinal belongs to one final receiving owner.
 *   A metadata domain and a body snapshot confer no executable admission.
 */
#include "xxir_metadata_domain_internal.h"

enum { EFFECT_BODY_WORDS=10, EFFECT_BODY_MASK=1023, EFFECT_BODY_HEADER=13 };
/* This request borrows the actual generating contexts only during construction.
 * Its owned ordinal products contain no caller-supplied role or authority bit. */
typedef struct EffectBodyOrdinaryInput {
    EffectOrdinaryContexts *contexts;
    const XrXirModule *source;
    /* A live generating ordinary owner is a lifetime input, never authority. */
    EffectOrdinaryContexts *basis;
} EffectBodyOrdinaryInput;
typedef struct EffectBodyOrdinaryIdentity {
    uint32_t declaration,owner;
} EffectBodyOrdinaryIdentity;
static XrXirStatus effect_body_ordinary_input_verify(const XrXirCompileContext *work,
    const XrXirModule *module,const EffectBodyOrdinaryInput *input);
static XrXirStatus effect_body_ordinary_identity(const XrXirCompileContext *work,
    const EffectBodyOrdinaryInput *input,uint32_t function,EffectBodyOrdinaryIdentity *output);

typedef struct EffectBodyFunction {
    const char *name;
    uint32_t name_length;
    const XrXirType *parameters;
    uint32_t parameter_count;
    XrXirType result;
    const XrXirBlock *blocks;
    uint32_t block_count;
    uint32_t instruction_count;
    const uint32_t *operands;
    uint32_t operand_count;
    uint32_t *stream;
    uint32_t stream_words,stream_capacity,run_count;
    /* Concrete initial roles follow the constructor's real evidence proof. */
    const uint8_t *initial_roles;
} EffectBodyFunction;
typedef struct EffectBodyMaterial {
    XrCompileResources *resources;
    /* Only private compiler owners acquire/release these prepaid references. */
    uint32_t references;
    /* A thin generation owns its mutable advertisement overlay and retains
     * the complete immutable backbone. It never frees borrowed child owners. */
    struct EffectBodyMaterial *backbone;
    EffectTerms terms;
    XrXirTypes *owned_types;
    XirMetadataDomain domain;
    XrXirStage stage;
    XrXirLinkageKind linkage_kind;
    EffectBodyFunction *functions;
    XrXirDeclarations *declarations;
    XrXirGeneric *generics;
    XrXirDefaultTable defaults;
    uint32_t evidence_kind;
    /* Every actual internal ordinal is copied; formal types remain in functions.
     * Public raw evidence and internal physical-use products are separate priors. */
    const EffectBodyOrdinaryIdentity *ordinary_identities;
    uint32_t ordinary_source_count;
    EffectPhysicalDomain *physical;
    XrXirCellProvenance *cells;
    EffectInvocationDeclaredBounds *declared;
    XrXirFunctionEffects *control;
    XrXirRootEffects *local_roots;
    XrXirRootEffectWitness *local_witnesses;
} EffectBodyMaterial;
static XrXirStatus effect_body_ordinary_basis(const XrXirCompileContext *work,
    const EffectBodyOrdinaryInput *input,EffectBodyMaterial **output);
typedef struct EffectBodyRead {
    const void *owner;
    bool (*consume)(const void *owner,uint64_t units);
} EffectBodyRead;
typedef struct EffectBodyRunRef {
    const uint32_t *base,*delta;
    uint32_t first,count,mask,next;
} EffectBodyRunRef;
typedef struct EffectBodyPendingRun {
    uint32_t first,count,mask;
    uint32_t base[EFFECT_BODY_WORDS],delta[EFFECT_BODY_WORDS];
} EffectBodyPendingRun;

static inline bool effect_body_compile_consume(const void *owner,uint64_t units) {
    return xir_compile_work((const XrXirCompileContext *)owner,units);
}
static inline EffectBodyRead effect_body_compile_read(const XrXirCompileContext *work) {
    return (EffectBodyRead){work,effect_body_compile_consume};
}
static inline bool effect_body_runtime_consume(const void *owner,uint64_t units) {
    const XirEffectRuntimeRead *read=owner;
    return read && read->work && read->work(read->owner,units);
}
static inline EffectBodyRead effect_body_runtime_read(const XirEffectRuntimeRead *read) {
    return (EffectBodyRead){read,effect_body_runtime_consume};
}
static inline bool effect_body_read_work(const EffectBodyRead *read,uint64_t units) {
    return read && read->consume && read->consume(read->owner,units);
}
static inline uint32_t effect_body_count(const EffectBodyMaterial *body) {
    return body->domain.function_count;
}

/* Ownership consistency is only a storage precondition. Semantic callers
 * still prove complete external correspondence and actual equation closure. */
static inline bool effect_body_material_shape(const EffectBodyMaterial *body) {
    return body && body->resources && body->references && body->domain.types==&body->terms.types &&
        (!!body->domain.function_count==!!body->functions) &&
        body->domain.declarations==body->declarations && body->domain.generics==body->generics &&
        (!body->domain.defaults || body->domain.defaults==&body->defaults) &&
        (!!body->ordinary_identities==!!body->ordinary_source_count) &&
        (!body->ordinary_identities || (!body->evidence_kind &&
         body->ordinary_source_count<=body->domain.function_count));
}

/* The concrete initial-role product, not a provenance alias, is consumed by
 * physical-use derivation. This says nothing about the resulting ROOT equation. */
static inline bool effect_body_variable_roles(const EffectBodyMaterial *body) {
    return body->ordinary_identities || body->evidence_kind==XR_XIR_EVIDENCE_TEMPLATE ||
        body->evidence_kind==XR_XIR_EVIDENCE_INSTANCE;
}
static XrXirStatus effect_body_initial_roles_match(const XrXirCompileContext *work,
    const EffectBodyMaterial *body,uint32_t function) {
    if (!xir_compile_context_valid(work) || !effect_body_material_shape(body) ||
        body->resources!=work->resources || function>=effect_body_count(body)) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *head=&body->functions[function];
    if (head->parameter_count && (!head->parameters || !head->initial_roles)) return XR_XIR_BAD_STRUCTURE;
    if (body->ordinary_identities) {
        if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
        EffectBodyOrdinaryIdentity id=body->ordinary_identities[function];
        if (id.declaration>=body->ordinary_source_count ||
            (id.owner!=UINT32_MAX && id.owner>=effect_body_count(body)) || !body->declarations ||
            !body->declarations->functions || body->declarations->functions[function].cleanup_owner!=
            (id.owner==UINT32_MAX?0:id.owner+1)) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t p=0;p<head->parameter_count;++p) {
        if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
        uint32_t role=effect_body_variable_roles(body) &&
            xr_xir_callable_signature(body->domain.types,head->parameters[p]) ?
            XR_XIR_EFFECT_PARAMETER_VARIABLE:XR_XIR_EFFECT_PARAMETER_FIXED;
        if (head->initial_roles[p]!=role) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
