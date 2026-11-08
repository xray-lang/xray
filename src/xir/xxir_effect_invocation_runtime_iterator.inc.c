/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_runtime_iterator.inc.c - Complete physical origin capture traversal
 *
 * KEY CONCEPT:
 *   Current static rows remain complete. Physical objects supply members and
 *   original captures; only an already active object closes its own back edge.
 */
typedef struct EffectRuntimeOriginRead {
    const EffectInvocationCertificate *certificate;
    const XirEffectInvocationRequest *request;
    EffectInvocationBasis basis;
    const uint64_t *row;
} EffectRuntimeOriginRead;
typedef struct EffectRuntimeOriginFrame {XirEffectRuntimeValue value;uint32_t next;} EffectRuntimeOriginFrame;
typedef struct EffectRuntimeOriginStack {
    EffectRuntimeOriginFrame *frames,first;size_t depth,capacity,bytes;
} EffectRuntimeOriginStack;

/* The fixed first frame is automatic. Deeper physical paths use only the
 * live runtime owner's finite allocation and scratch budget, never a C call. */
