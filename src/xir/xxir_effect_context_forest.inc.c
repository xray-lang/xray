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

/* Dense ordinals identify public declarations. An executing cause can visit
 * one body in several distinct invocation environments before its terminal. */
