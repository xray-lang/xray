/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_edge_index.inc.c - Construction-local exact edge keys
 */
typedef struct EffectInvocationEdgeKey {
    uint32_t caller,target,instruction,producer,latent;
} EffectInvocationEdgeKey;

/* Hashes locate buckets only. Every occupied bucket compares the complete
 * current key, and numeric edge indices survive the original array growth. */

/* A replacement remains private until every old edge has been reinserted.
 * Failure leaves the old index and all graph chains owned and unchanged. */
