/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_batch_internal.h - One immutable input, independently executed roots
 *
 * KEY CONCEPT:
 *   Complete current artifact admission belongs to this synchronous operation.
 *   Each root retains its full closure admission and ordinary bounded execution.
 */
#ifndef XXIR_CTFE_BATCH_INTERNAL_H
#define XXIR_CTFE_BATCH_INTERNAL_H
#include "xxir_ctfe.h"
typedef struct XirCtfeBatchRequest {
    const XrXirArtifact *artifact;
    const uint32_t *functions;
    uint32_t count;
    XrXirTarget target;
    uint8_t checked_identity[32];
    XrXirCtfeLimits limits;
    uint32_t call_depth;
} XirCtfeBatchRequest;
/* Input and function storage remain immutable and do not overlap output.
 * All roots have zero runtime arguments. Every root receives its own original
 * steps/frame/output/depth limits; the finite compiler ledger is shared and
 * never renewed. No callbacks, native code or stored validation hint enter.
 * Output has count canonical Unit slots. Any failure restores every slot. */
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_batch(const XirCtfeBatchRequest *request,
    XrXirValue *output);
#endif // XXIR_CTFE_BATCH_INTERNAL_H
