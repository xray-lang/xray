/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_atomic.h - Constructed atomic ownership and bounded shared execution
 *
 * KEY CONCEPT:
 *   A retry owns its receiver and evaluated operands until frame cleanup.
 */
#ifndef XXIR_ATOMIC_H
#define XXIR_ATOMIC_H
#include "xxir_call.h"

typedef enum XrXirAtomicOperation {
    XR_XIR_ATOMIC_OPERATION_LOAD, XR_XIR_ATOMIC_OPERATION_STORE, XR_XIR_ATOMIC_OPERATION_ADD, XR_XIR_ATOMIC_OPERATION_SUB,
    XR_XIR_ATOMIC_OPERATION_FETCH_ADD, XR_XIR_ATOMIC_OPERATION_FETCH_SUB, XR_XIR_ATOMIC_OPERATION_SWAP,
    XR_XIR_ATOMIC_OPERATION_COMPARE_EXCHANGE, XR_XIR_ATOMIC_OPERATION_TOGGLE, XR_XIR_ATOMIC_OPERATION_TO_STRING
} XrXirAtomicOperation;

typedef struct XrXirAtomicRequest {
    const XrXirValue *receiver, *operands, *ordering;
    uint32_t operand_count;
    XrXirAtomicOperation operation;
    XrXirType result_type;
} XrXirAtomicRequest;

/* Owned frame storage, initialized to zero and cleared exactly once. These
 * fields are runtime state, not an ordering or constructor authority channel. */
typedef struct XrXirAtomicProgress {
    XrXirValue receiver;
    uint64_t operands[2], observed;
    XrXirType result_type;
    XrXirAtomicOperation operation;
    uint32_t ordering, phase;
    bool observed_ready;
} XrXirAtomicProgress;

typedef struct XrXirAtomicOutcome {
    XrXirRunStatus status;
    XrXirCallStatus permission;
    bool continuing;
} XrXirAtomicOutcome;

/* Capability uses the actual aligned cell and reference layouts. */
XR_FUNC bool xr_xir_atomic_capability(void);
XR_FUNC XrXirValueStatus xr_xir_atomic_new(XrXirType type, const XrXirValue *initial,
    XrXirValueAdmission *admission, XrXirValue *output);
/* Ordering is an exact nullable governed enum, or NULL for omission. All
 * arguments are evaluated before entry. Product callbacks supply their active
 * view; NULL is reserved for trusted direct runtime callers without a Call.
 * A continuing outcome publishes no value and performs at most one cell CAS.
 * Success leaves progress owned until its frame clears it, including retries. */
XR_FUNC XrXirAtomicOutcome xr_xir_atomic_start(const XrXirAtomicRequest *request,
    XrXirValueAdmission *admission, const XrXirCallView *view,
    XrXirAtomicProgress *progress, XrXirValue *output);
XR_FUNC XrXirAtomicOutcome xr_xir_atomic_resume(XrXirAtomicProgress *progress,
    XrXirValueAdmission *admission, const XrXirCallView *view, XrXirValue *output);
XR_FUNC void xr_xir_atomic_progress_clear(XrXirAtomicProgress *progress);
#endif // XXIR_ATOMIC_H
