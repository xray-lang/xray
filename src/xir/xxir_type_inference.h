/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_type_inference.h - Internal incremental value-shape inference
 *
 * KEY CONCEPT:
 *   Partial evidence can guide one later expression without publishing a call.
 */
#ifndef XXIR_TYPE_INFERENCE_H
#define XXIR_TYPE_INFERENCE_H
#include "xxir_types.h"
typedef struct XrXirInferenceState XrXirInferenceState;
typedef struct XrXirInferencePair { XrXirType formal, actual; } XrXirInferencePair;
typedef struct XrXirInferenceRequest {
    const XrXirTypes *types;
    const XrXirType *prefix;
    uint32_t prefix_count, own_count, caller_parameter_count;
    const uint32_t *parameter_kinds;
} XrXirInferenceRequest;
typedef struct XrXirInferenceKnown {
    bool known;
    const XrXirType *arguments;
    uint32_t argument_count;
} XrXirInferenceKnown;
/* Internal source API, not proof authority. Descriptor shapes are prior-admitted.
 * A single append-only type arena owns all IDs. Pass its current immutable view
 * per operation; state retains IDs, never node pointers across expression growth.
 * Budget is borrowed through dispose. Persistent state holds scratch until free;
 * temporary operation storage is released on success and failure. Work consumed
 * by failed observations remains consumed; a failed state cannot be finalized. */
XR_FUNC XrXirStatus xr_xir_inference_begin(const XrXirInferenceRequest *request,
    XrXirBudget *budget, XrXirInferenceState **output);
XR_FUNC XrXirStatus xr_xir_inference_observe(XrXirInferenceState *state,
    const XrXirTypes *types, XrXirInferencePair pair);
/* Unknown is successful with a zero view. A known view is borrowed solely for
 * substituting this formal expression; other own slots can still be unsolved.
 * This view cannot publish a generic tuple or discharge a constraint. */
XR_FUNC XrXirStatus xr_xir_inference_expected_known(XrXirInferenceState *state,
    const XrXirTypes *types, XrXirType formal, XrXirInferenceKnown *output);
/* Caller output has total prefix+own slots and is untouched on failure.
 * Whole constraints/access must still be proven before emitting a call. */
XR_FUNC XrXirStatus xr_xir_inference_finalize(XrXirInferenceState *state,
    const XrXirTypes *types, XrXirType *output, uint32_t output_count);
XR_FUNC void xr_xir_inference_dispose(XrXirInferenceState *state);
#endif // XXIR_TYPE_INFERENCE_H
