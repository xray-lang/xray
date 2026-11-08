/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_runtime_data.inc.c - Current semantic coordinates in one sealed owner
 */
typedef struct EffectRuntimeKeyRead {
    const EffectInvocationCertificate *certificate;
    const XirEffectInvocationRequest *request;
    const EffectInvocationNode *from;
    EffectInvocationBasis basis;
    uint32_t node;
} EffectRuntimeKeyRead;

/* Data atoms have no causes. Identity and capture atoms were independently
 * proved before this immutable Final entered the Program. Their numeric
 * coordinates project the same complete row, with no reconstructed graph. */
