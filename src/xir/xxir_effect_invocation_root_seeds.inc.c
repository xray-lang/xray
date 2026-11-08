/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_root_seeds.inc.c - Public environment seeds from actual parameter types
 *
 * KEY CONCEPT:
 *   The same current Input rule constructs and verifies a root key. A role is
 *   an operand whose derivation is proved separately; stored advertisements do not qualify it.
 */
static XrXirStatus effect_role_range(const XrXirCompileContext *work,const void *pointer,uint64_t count,size_t size);
typedef struct EffectInvocationSeedRead {
    const XrXirCompileContext *work;
    const XrXirTypes *types;
    EffectBodyFunctionSignature head;
    EffectInvocationBasis basis;
    uint32_t sites;
} EffectInvocationSeedRead;
typedef struct EffectInvocationSeedBits {
    uint32_t origins[3],cells[2],origin_count,cell_count;
} EffectInvocationSeedBits;
