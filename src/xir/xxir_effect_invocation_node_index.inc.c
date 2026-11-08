/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_node_index.inc.c - Temporary exact equation lookup by body
 *
 * KEY CONCEPT:
 *   Immutable body numbers select buckets in the sole node inventory. Every
 *   root and complete input still compares exactly; no lookup enters Final.
 */
typedef struct EffectInvocationNodeLookup {
    uint32_t root,body,parameters;
    const uint64_t *input;
    size_t words;
} EffectInvocationNodeLookup;

/* Links descend through original node numbers. Array growth copies numeric
 * links only; no pointer to a movable node or stored permission is retained. */
