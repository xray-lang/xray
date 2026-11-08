/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_query.inc.c - Typed actual edge masks from sealed equations
 *
 * KEY CONCEPT:
 *   A query consumes the exact real call frontier and its complete equation.
 *   A missing edge or unbound input is UNKNOWN, never an empty permission.
 */

typedef struct EffectInvocationEdgeShape {
    const EffectInvocationFinal *equations;
    const XirMetadataDomain *domain;
    const XrXirTypes *types;
    const XrXirInstruction *op,*reference;
    uint32_t caller,edge_index,parameters,instructions,producer_instructions,target_parameters;
    XrXirType actual_type;
} EffectInvocationEdgeShape;

/* Raw construction and final owning readers supply the same complete request.
 * No reconstructed executable Module or alternate permission path is needed. */

/* The optional opcode belongs only to this completed edge request. Full
 * physical decoding and exact edge/type admission precede its publication. */

/* Each walk consumes the immutable local terminal copy. Actual call links
 * name the full child equation and real producer site, and strictly decrease
 * the owned forest distance. A same-number target cannot stand in for a link. */

/* This is a compile-time edge projection within the single common owner.
 * The public Source ordinals and physical vectors are matched by its existing
 * context selector before these private contextual IDs can reach the query. */

typedef struct EffectClassReferenceMask {
    const XrXirCompileContext *work;const XrXirEffects *effects;
    uint32_t function,instruction,mask,count;
} EffectClassReferenceMask;
