/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_runtime_key.inc.c - Exact current inputs before physical execution matching
 */
#include "xxir_effect_invocation_runtime_data.inc.c"
typedef struct EffectRuntimeKeySpan {uint32_t source,destination,count;} EffectRuntimeKeySpan;
typedef struct EffectRuntimeKeyProjection {
    EffectRuntimeKeySpan spans[2];
    uint32_t value,count,opaque_mask,class_mask;
} EffectRuntimeKeyProjection;

/* Spans contain only numeric locations in this request. They neither retain
 * decoded rows nor qualify an edge. Both directions below compare every real
 * bit, including unused columns and zero padding in the complete input. */
