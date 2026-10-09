/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe.h - Compiler-owned evaluation of closed scalar leaf functions
 *
 * KEY CONCEPT:
 *   Authentic Lowered instructions share ordinary VM numeric semantics.
 *   This compiler boundary grants no runtime or source-language authority.
 */
#ifndef XXIR_CTFE_H
#define XXIR_CTFE_H
#include "xxir.h"

#define XR_XIR_CTFE_MAX_STEPS UINT64_C(65536)
#define XR_XIR_CTFE_MAX_FRAME_BYTES UINT64_C(65536)
#define XR_XIR_CTFE_MAX_OUTPUT_BYTES UINT64_C(8)
typedef enum XrXirCtfeStatus {
    XR_XIR_CTFE_OK, XR_XIR_CTFE_BAD_ARGUMENT, XR_XIR_CTFE_BAD_ARTIFACT,
    XR_XIR_CTFE_PROFILE, XR_XIR_CTFE_IDENTITY, XR_XIR_CTFE_NOT_LEAF,
    XR_XIR_CTFE_BUDGET, XR_XIR_CTFE_OUT_OF_MEMORY, XR_XIR_CTFE_STEP_LIMIT,
    XR_XIR_CTFE_FRAME_LIMIT, XR_XIR_CTFE_OUTPUT_LIMIT,
    XR_XIR_CTFE_DIVIDE_BY_ZERO, XR_XIR_CTFE_NUMERIC_RANGE
} XrXirCtfeStatus;
typedef struct XrXirCtfeLimits { uint64_t steps, frame_bytes, output_bytes; } XrXirCtfeLimits;
typedef struct XrXirCtfeRequest {
    const XrXirArtifact *artifact;
    uint32_t function;
    XrXirTarget target;
    /* Digest of the closed Checked packet from which this artifact was lowered. */
    uint8_t checked_identity[32];
    const XrXirValue *arguments;
    uint32_t argument_count;
    XrXirCtfeLimits limits;
} XrXirCtfeRequest;

/* Borrowed input owners must remain live for this synchronous call. The selected
 * ordinary function must have only bool/integer inputs, results and instructions
 * and an acyclic explicit scalar opcode graph. Every call repeats full artifact
 * verification on its original compiler ledger; root/effect flags never grant
 * purity. Limits may deny all resources and must not exceed the fixed ceilings.
 * Output must be canonical Unit. Failure preserves it, and consumed compiler
 * allocation/work is never refunded. Output bytes measure target logical storage,
 * not the host carrier size. No source syntax, calls or runtime fallback is implied. */
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_leaf(const XrXirCtfeRequest *request,
    XrXirValue *output);
#endif // XXIR_CTFE_H
