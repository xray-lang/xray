/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_context_dense.inc.c - Complete physical type vectors
 *
 * KEY CONCEPT:
 *   Every physical position has an identity. Only freshly classified variable
 *   callable arguments retain precision; other arguments keep ordinary exact
 *   types or the original complete callable bound.
 */
/* This internal view is consumed only by its execution owner. Its seeded
 * ordinary descriptor prefix keeps identical IDs and is never rewritten;
 * all later structural descriptors are append-only in the same type pool. */
typedef struct EffectDenseRequest {
    const EffectOrdinaryContexts *contexts;
    EffectOrdinaryContexts *execution;
    uint32_t selected, caller_index;
    const XrXirTypes *actual_types;
    const XrXirFunction *caller;
    const XrXirInstruction *instruction;
    const uint8_t *fixed;
    uint32_t value_count;
} EffectDenseRequest;

/* Stack view; its complete physical array is owned by execution->terms. */
typedef struct EffectDenseVector {
    const EffectTerms *terms;
    const XrXirType *physical;
    uint32_t count;
} EffectDenseVector;

typedef struct EffectDenseActual {
    XrXirType type;
    bool fixed, present;
} EffectDenseActual;





/* The owner construction deep-copies and completely checks the ordinary
 * declaration domain once. No foreign pool is admitted by this edge helper.
 * Missing capture suffixes keep original bounds; unknown never becomes NONE. */
