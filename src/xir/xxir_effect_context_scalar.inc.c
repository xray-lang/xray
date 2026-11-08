/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_context_scalar.inc.c - Complete callable bounds on scalar carriers
 *
 * KEY CONCEPT:
 *   Carrier transfer keeps every nested type, mode, and result exact. Explicit
 *   weakening remains a real barrier; latent capture reads a derived formula.
 */
typedef struct EffectScalarFlow {
    EffectTerms *terms;
    XrXirFunction *function;
    const EffectOrdinaryContexts *contexts;
    XrXirType *declared;
    XrXirType *values;
    uint8_t *fixed;
    uint32_t value_count;
    bool changed;
    const XrXirDeclarations *declarations;
} EffectScalarFlow;

/* A return receiver pins the physical scalar type, not its opaque origin.
 * The temporary bit is removed before physical-vector selection. */
enum { EFFECT_SCALAR_OPAQUE=1, EFFECT_SCALAR_RETURN_PIN=2, EFFECT_SCALAR_STORAGE_PIN=4 };

#include "xxir_effect_context_storage.inc.c"























/* The caller must supply freshly derived contextual contracts and already
 * selected physical actuals. This helper cannot select a dense vector itself. */
