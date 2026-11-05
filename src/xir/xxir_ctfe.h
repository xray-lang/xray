/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe.h - Bounded compiler execution of closed Lowered functions
 *
 * KEY CONCEPT:
 *   Authentic Lowered instructions share ordinary VM numeric semantics.
 *   This compiler boundary grants no runtime or source-language authority.
 */
#ifndef XXIR_CTFE_H
#define XXIR_CTFE_H
#include "xxir.h"
#include "../base/xcompile_request.h"

#define XR_XIR_CTFE_MAX_STEPS UINT64_C(65536)
#define XR_XIR_CTFE_MAX_FRAME_BYTES UINT64_C(65536)
/* Persistent result keys must include this policy and all actual evaluation and memory limits.
 * A result accepted with a larger ceiling grants no smaller-budget cache hit. */
#define XR_XIR_CTFE_OUTPUT_POLICY_VERSION UINT32_C(2)
#define XR_XIR_CTFE_MAX_OUTPUT_BYTES (UINT64_C(8)*1024*1024)
#define XR_XIR_CTFE_MAX_CALL_DEPTH UINT32_C(128)
typedef enum XrXirCtfeStatus {
    XR_XIR_CTFE_OK, XR_XIR_CTFE_BAD_ARGUMENT, XR_XIR_CTFE_BAD_ARTIFACT,
    XR_XIR_CTFE_PROFILE, XR_XIR_CTFE_IDENTITY, XR_XIR_CTFE_NOT_LEAF,
    XR_XIR_CTFE_COMPLEXITY_LIMIT, XR_XIR_CTFE_OUT_OF_MEMORY, XR_XIR_CTFE_STEP_LIMIT,
    XR_XIR_CTFE_FRAME_LIMIT, XR_XIR_CTFE_OUTPUT_LIMIT,
    XR_XIR_CTFE_DIVIDE_BY_ZERO, XR_XIR_CTFE_NUMERIC_RANGE,
    XR_XIR_CTFE_DEPTH_LIMIT, XR_XIR_CTFE_NOT_SUPPORTED, XR_XIR_CTFE_NULL_UNWRAP, XR_XIR_CTFE_BOUNDS, XR_XIR_CTFE_UNCAUGHT_ERROR,
    XR_XIR_CTFE_ASSERTION_FAILURE, XR_XIR_CTFE_MATCH_FAILURE, XR_XIR_CTFE_CLEANUP_PANIC,
    XR_XIR_CTFE_MEMORY_LIMIT, XR_XIR_CTFE_OVERFLOW, XR_XIR_CTFE_CANCELLED,
    XR_XIR_CTFE_DEADLINE, XR_XIR_CTFE_CLOCK_FAILURE
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
    /* Borrowed only until this synchronous evaluation returns. Nested calls
     * share it; no Value, Domain, Artifact or retained result stores it. */
    XrCompileRequestControl *control;
} XrXirCtfeRequest;

/* Receive a closed compiler value family from its structurally verified pool.
 * This memory-owned iterative admission accepts primitive/String/Unit and
 * immutable Tuple/Nullable/Array and declared struct/enum data. Finite storage
 * handles may close recursive metadata; inline cycles, pure Array cycles and
 * authority-bearing fields refuse. Actual value graphs are admitted separately and
 * no execution permission follows. Failure preserves caller metadata. */
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_type_admit(const XrXirCompileContext *context,
    const XrXirTypes *types, XrXirType type);

/* Borrowed input owners must remain live for this synchronous call. The selected
 * ordinary function has admitted closed data inputs and data or Unit results,
 * frame-owned locals and private Cells of admitted value families. Immutable
 * Tuples and Nullable values can be constructed/projected and exported by logical
 * active field-byte sum and canonical discriminants; Unit fields occupy zero
 * output bytes. Nullable unwrap of None returns NULL_UNWRAP. External composites
 * receive independent bounded snapshots after complete cross-arena type matching. Private storage
 * remains inside the bounded admitted opcode graph. Local
 * writes produce Unit without a result slot. Every call repeats full artifact
 * verification on its original compiler memory owner; root/effect flags never grant
 * purity. Limits may deny all resources and must not exceed the fixed ceilings.
 * Float operations use the existing integer-only IEEE kernel and bounded local
 * workspaces. Canonical f32/f64 values
 * export four/eight target bytes; numeric conversion range errors preserve output.
 * Output must be canonical Unit. Failure preserves it, and consumed compiler
 * temporary memory is released on every failure. Output bytes measure target logical storage,
 * not the host carrier size. No source syntax, calls or runtime fallback is implied. */
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_leaf(const XrXirCtfeRequest *request,
    XrXirValue *output);
/* Direct calls execute only a fully checked admitted-value reachable closure,
 * including frame-owned locals and private Cells on the compiler memory owner.
 * Internal ref/lexical cleanup parameters require fresh producer roles and
 * authentic frame loans. Static cleanup targets join the complete closure;
 * frontiers retire after child return and pending returns retain snapshots.
 * Numeric/CFG execution uses the common kernel. Depth
 * and live activation bytes bound the whole stack; steps are never reset
 * on a callee or refunded on return. Admission includes unused reachable body
 * operations and does not derive determinism from any ROOT/effect flags.
 * Borrowed inputs stay live during this synchronous call. All failures preserve
 * canonical Unit output. String exports own their compiler storage and measure
 * UTF-8 bytes. Comparison/search use ordinary Unicode index and byte-order rules,
 * report NUMERIC_RANGE on invalid starts. Immutable
 * Tuple/Nullable/Array and admitted struct/enum exports retain their actual type
 * arena and active fields. External Cell/Function and other identity arguments, Function
 * exports remain unadmitted. Internal PanicInfo values originate only at real
 * panic handlers; code/message use ordinary immutable Value operations. Panic
 * propagates through applicable cleanup to the actual caller's handler. An
 * escaping cleanup panic returns CLEANUP_PANIC without aborting the host.
 * External PanicInfo inputs and exports remain refused. Internal enum Error values use
 * authentic Value erasure/matching and explicit invoke/error continuations.
 * An uncaught error follows applicable cleanup before UNCAUGHT_ERROR; compiler
 * resource failures never enter language catch. Scoped projections use
 * existing authorized Cell paths and exact parent loans; selectors, child
 * preparation and common receiving scratch share the remaining frame quota.
 * This entry never invokes Instance, native code or a fallback. */
typedef struct XrXirCtfeCallRequest {
    XrXirCtfeRequest evaluation;
    uint32_t call_depth;
} XrXirCtfeCallRequest;
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_calls(const XrXirCtfeCallRequest *request,
    XrXirValue *output);
#endif // XXIR_CTFE_H
