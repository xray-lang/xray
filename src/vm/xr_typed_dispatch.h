/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_typed_dispatch.h - Typed TargetPlan scalar executor
 */

#ifndef XR_TYPED_DISPATCH_H
#define XR_TYPED_DISPATCH_H

#include "../plan/target/xr_target_plan.h"

typedef enum XrTypedDispatchStatus {
    XR_TYPED_DISPATCH_OK = 0,
    XR_TYPED_DISPATCH_INVALID_ARGUMENT,
    XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED,
    XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH,
    XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE,
    XR_TYPED_DISPATCH_PROGRAM_INVALID,
    XR_TYPED_DISPATCH_ARGUMENT_MISMATCH,
    XR_TYPED_DISPATCH_FRAME_ERROR,
    /* The executed program's own error edge, kept apart from every status
     * above: those report that the plan or the call was unacceptable, while
     * these report that an acceptable program divided by zero. They stay
     * distinct from each other so a caller can raise the exact panic the
     * language names for each operator. */
    XR_TYPED_DISPATCH_DIVIDE_BY_ZERO,
    XR_TYPED_DISPATCH_MODULO_BY_ZERO,
} XrTypedDispatchStatus;

/*
 * Arguments are positional signed i64 values. The count must equal the
 * parameter count the verified instruction group binds; a shorter, longer, or
 * absent vector is rejected rather than truncated or zero filled.
 */
XR_FUNC XrTypedDispatchStatus xr_typed_dispatch_execute_i64(
    const XrTargetPlan *verified_plan,
    const XrFingerprint *required_plan_fingerprint, uint32_t function,
    const int64_t *arguments, uint32_t argument_count, int64_t *result);

#endif  // XR_TYPED_DISPATCH_H
