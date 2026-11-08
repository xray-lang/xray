/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_context_forest.inc.c - Owned contextual cause certificates
 *
 * KEY CONCEPT:
 *   Public ordinals identify declarations. Private context ordinals choose the
 *   exact environment and forest, and survive release of every input owner.
 */
typedef struct EffectForestNode {
    uint32_t declaration, argument_count, parameter_count, constant_mask, owner;
    const XrXirType *arguments, *physical_types;
} EffectForestNode;

typedef struct EffectContextCauseValue {
    XrXirRootEffectWitness witness;
    uint32_t next, parameter_owner;
    uint8_t domain, next_domain;
} EffectContextCauseValue;

typedef struct EffectContextForest {
    /* Type storage belongs to the transferred immutable invocation owner.
     * This arena owns only forest metadata and construction scratch. */
    EffectTerms terms;
    XrCompileResources *resources;
    uint32_t count;
    EffectForestNode *nodes;
    EffectInvocationCertificate *invocations;
    uint8_t **requirements;
} EffectContextForest;

/* Detached values live only for their requesting call. The retained Final
 * contains the only ROOT facts and four-domain cause witnesses. */
typedef struct EffectContextRootValue {
    XrXirRootEffects facts;
    XrXirRootEffectWitness root,unresolved;
} EffectContextRootValue;
static XrXirStatus effect_invocation_forest_values(const XrXirCompileContext *work,
    const EffectContextForest *forest,uint32_t function,EffectContextRootValue *output);
static XrXirStatus effect_role_range(const XrXirCompileContext *work,
    const void *memory,uint64_t count,size_t width);
static XrXirStatus effect_role_material_frame(const XrXirCompileContext *work,
    const EffectBodyMaterial *material);

static void effect_context_forest_free(EffectContextForest *forest) {
    if (!forest) return;
    /* Release all borrowing metadata before its retained type owner. */
    effect_terms_free(&forest->terms);
    effect_invocation_certificate_free(forest->invocations);
    xr_compile_resources_free(forest);
}

static XrXirStatus effect_invocation_context_edge_mask(const XrXirCompileContext *work,
    const EffectContextForest *forest,uint32_t caller,uint32_t instruction,uint32_t target,uint32_t *output);

/* Dense ordinals identify public declarations. An executing cause can visit
 * one body in several distinct invocation environments before its terminal. */
