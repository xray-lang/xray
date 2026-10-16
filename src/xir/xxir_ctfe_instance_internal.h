/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_instance_internal.h - Owned results retain real generic definitions
 *
 * KEY CONCEPT:
 *   A closed instance, its original substitution and one freshly evaluated
 *   result share the original compiler ledger. Borrowing grants no permission.
 */
#ifndef XXIR_CTFE_INSTANCE_INTERNAL_H
#define XXIR_CTFE_INSTANCE_INTERNAL_H
#include "xxir_ctfe.h"

typedef struct XirCtfeInstanceResult XirCtfeInstanceResult;
typedef struct XirCtfeInstanceBatch XirCtfeInstanceBatch;
typedef struct XirCtfeInstanceRequest {
    const XrXirArtifact *checked;
    uint32_t function;
    XrXirTarget target;
    XrXirCtfeLimits limits;
    uint32_t call_depth;
} XirCtfeInstanceRequest;

typedef struct XirCtfeInstanceView {
    const XrXirArtifact *checked,*lowered,*definition;
    const XrXirValue *value;
    uint32_t instance_function,definition_function;
} XirCtfeInstanceView;

typedef struct XirCtfeInstanceBatchRequest {
    const XrXirArtifact *checked;
    const uint32_t *functions;
    uint32_t count;
    XrXirTarget target;
    XrXirCtfeLimits limits;
    uint32_t call_depth;
} XirCtfeInstanceBatchRequest;

/* Only genuine zero-runtime-argument generic instances qualify. This entry
 * independently receives actual Checked bytes, lowers and executes the common
 * evaluator every time. It cannot accept a caller-supplied value or PASS hint.
 * All work and allocation belong to checked's original finite compiler ledger.
 * Failure preserves an empty output and never refunds consumed resources. */
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_instance(
    const XirCtfeInstanceRequest *request,XirCtfeInstanceResult **output);

/* The caller keeps owner alive and excludes mutation throughout the borrow.
 * Both owned artifacts undergo complete current verification before publication,
 * and current Checked bytes must exactly match the actual Lowered source packet.
 * The view identifies real definitions and closed substitutions, not access,
 * construction or executable authority. The output must be entirely zero. */
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_instance_view(
    const XirCtfeInstanceResult *owner,XirCtfeInstanceView *output);
XR_FUNC void xr_xir_compile_ctfe_instance_free(XirCtfeInstanceResult *owner);
/* One synchronous operation receives the immutable Checked input once and
 * evaluates every genuine instance under its own execution limits. Selection
 * storage remains immutable until return. Failure publishes no owner. */
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_instance_batch(
    const XirCtfeInstanceBatchRequest *request,XirCtfeInstanceBatch **output);
/* Complete current correspondence is checked once during this synchronous
 * borrow, followed by every real definition and result. All count views must
 * initially be zero; a failure publishes none. No validation hint survives. */
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_instance_batch_views(
    const XirCtfeInstanceBatch *owner,XirCtfeInstanceView *output,uint32_t count);
XR_FUNC void xr_xir_compile_ctfe_instance_batch_free(XirCtfeInstanceBatch *owner);
#endif // XXIR_CTFE_INSTANCE_INTERNAL_H
