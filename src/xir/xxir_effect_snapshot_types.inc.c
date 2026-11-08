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

/* Declaration IDs retain their public function admission. Private ordinary
 * environments and invocation keys share the existing finite equation bound;
 * every descriptor and expansion still consumes the original resource ledger. */

enum { EFFECT_BODY_WORDS=10, EFFECT_BODY_MASK=1023 };
/* The sole owned representation is an actual affine record. Inactive delta
 * slots are neither initialized nor read. No summary, authority or pointer to
 * the generating Module is retained by a record. */
typedef struct EffectBodyRun {
    uint32_t first,count,mask;
    XrXirInstruction base;
    uint32_t delta[EFFECT_BODY_WORDS];
} EffectBodyRun;
typedef struct EffectBodyRunStorage {
    XrCompileResources *resources;
    uint32_t references,capacity;
} EffectBodyRunStorage;
_Static_assert(sizeof(EffectBodyRunStorage)%_Alignof(EffectBodyRun)==0,
    "Physical record storage preserves run alignment");
/* This request borrows the actual generating contexts only during construction.
 * Its owned ordinal products contain no caller-supplied role or authority bit. */
typedef struct EffectBodyOrdinaryInput {
    EffectOrdinaryContexts *contexts;
    const XrXirModule *source;
    /* A live generating ordinary owner is a lifetime input, never authority. */
    EffectOrdinaryContexts *basis;
    /* The current operation retains this genuine initial certificate body. */
    struct EffectBodyMaterial *initial;
    /* Only lossless records are candidates when refinement forbids sharing
     * an earlier generation's semantic products. */
    struct EffectBodyMaterial *record_source;
} EffectBodyOrdinaryInput;
typedef struct EffectBodyOrdinaryIdentity {
    uint32_t declaration,owner;
} EffectBodyOrdinaryIdentity;

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
    XrXirType error_type;
    uint32_t consume_parameter_count;
    const uint32_t *consume_parameters;
    EffectBodyRun *runs;
    uint32_t run_count,run_capacity;
    /* The record allocation owns its reference count, never the caller. */
    bool borrowed_runs;
    /* Concrete initial roles follow the constructor's real evidence proof. */
    const uint8_t *initial_roles;
} EffectBodyFunction;
typedef struct EffectBodyOrdinaryOverlay {
    uint32_t count;
    EffectBodyFunction *functions;
    XrXirFunctionIdentity *identities;
    XrXirGeneric *generics;
    EffectBodyOrdinaryIdentity *ordinary_identities;
    XrXirDeclarations declarations;
} EffectBodyOrdinaryOverlay;
typedef struct EffectBodyMaterial {
    XrCompileResources *resources;
    /* Only private compiler owners acquire/release these prepaid references. */
    uint32_t references;
    EffectInputFacts *input_facts;
    /* A thin generation owns its mutable advertisement overlay and retains
     * the complete immutable backbone. It never frees borrowed child owners. */
    struct EffectBodyMaterial *backbone;
    /* A fully copied ordinary body owns the complete canonical source too.
     * Thin holders borrow this pointer only through their retained backbone. */
    struct EffectBodyMaterial *canonical_source;
    /* Only an initial ordinary holder owns these transformed heads. */
    EffectBodyOrdinaryOverlay *overlay;
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
} EffectBodyMaterial;
/* Memory identity and execution read policy are distinct borrowed inputs.
 * Compiler reads carry no accounting callback or cumulative work allowance. */
typedef struct EffectBodyRead {
    const XrXirCompileContext *owner;
    const XirEffectRuntimeRead *execution;
} EffectBodyRead;

static inline bool effect_body_execution_work(const XirEffectRuntimeRead *read,uint64_t units) {
    return read && read->work && read->work(read->owner,units);
}
static inline uint32_t effect_body_count(const EffectBodyMaterial *body) {
    return body->domain.function_count;
}

/* A holder owns bookkeeping and declared bounds, while payload storage stays
 * on a live parent. These comparisons express only the bounded lifetime edge. */
static inline bool effect_body_parent_shape(const EffectBodyMaterial *body,
    const EffectBodyMaterial *parent) {
    return parent!=body && parent->references && parent->resources==body->resources &&
        body->declared && body->declared!=parent->declared &&
        parent->domain.types==&parent->terms.types &&
        body->domain.function_count==parent->domain.function_count &&
        body->stage==parent->stage && body->linkage_kind==parent->linkage_kind &&
        body->terms.types.nodes==parent->terms.types.nodes &&
        body->terms.types.count==parent->terms.types.count &&
        body->terms.types.nominals==parent->terms.types.nominals &&
        body->terms.types.interfaces==parent->terms.types.interfaces &&
        body->terms.memory==parent->terms.memory && body->owned_types==parent->owned_types &&
        body->physical==parent->physical && body->cells==parent->cells &&
        !!body->domain.defaults==!!parent->domain.defaults &&
        body->defaults.records==parent->defaults.records && body->defaults.count==parent->defaults.count;
}

static inline bool effect_body_overlay_shape(const EffectBodyMaterial *body) {
    const EffectBodyOrdinaryOverlay *overlay=body->overlay;
    return overlay && overlay->count==body->domain.function_count &&
        overlay->functions && overlay->identities && overlay->generics && overlay->ordinary_identities &&
        body->functions==overlay->functions && body->generics==overlay->generics &&
        body->ordinary_identities==overlay->ordinary_identities &&
        body->ordinary_source_count==overlay->count && !body->evidence_kind &&
        body->declarations==&overlay->declarations && overlay->declarations.functions==overlay->identities &&
        body->domain.declarations==body->declarations && body->domain.generics==body->generics;
}

/* There are at most two edges: dense -> ordinary overlay -> initial. Old
 * ordinary owners remain valid parents. Neither self-links nor a third edge
 * are storage; no recursion or public source comparison is replaced here. */
static inline bool effect_body_backbone_shape(const EffectBodyMaterial *body) {
    const EffectBodyMaterial *parent=body->backbone;
    if (!parent) return !body->overlay;
    if (!effect_body_parent_shape(body,parent) || !body->ordinary_identities || body->evidence_kind)
        return false;
    if (body->overlay) return effect_body_overlay_shape(body) && !parent->backbone &&
        !parent->overlay && !parent->ordinary_identities;
    if (body->functions!=parent->functions || body->declarations!=parent->declarations ||
        body->generics!=parent->generics || body->ordinary_identities!=parent->ordinary_identities ||
        body->ordinary_source_count!=parent->ordinary_source_count || parent->evidence_kind)
        return false;
    if (!parent->backbone) return !parent->overlay && parent->ordinary_identities;
    const EffectBodyMaterial *initial=parent->backbone;
    return initial!=body && !initial->backbone && !initial->overlay && !initial->ordinary_identities &&
        effect_body_overlay_shape(parent) && effect_body_parent_shape(parent,initial);
}

/* Ownership consistency is only a storage precondition. Semantic callers
 * still prove complete external correspondence and actual equation closure. */
static inline bool effect_body_material_shape(const EffectBodyMaterial *body) {
    const EffectBodyMaterial *canonical=body?body->canonical_source:NULL;
    bool canonical_shape=!canonical || (canonical!=body && canonical->references && canonical->resources==body->resources &&
        !canonical->ordinary_identities && !canonical->backbone && !canonical->canonical_source &&
        canonical->domain.function_count==body->ordinary_source_count &&
        canonical->domain.types==&canonical->terms.types && canonical->domain.declarations==canonical->declarations);
    return body && canonical_shape && (!body->backbone || body->canonical_source==body->backbone->canonical_source) &&
        body->resources && body->references && effect_body_backbone_shape(body) &&
        body->domain.types==&body->terms.types &&
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
        xir_effect_kind_is_closed(body->evidence_kind);
}
