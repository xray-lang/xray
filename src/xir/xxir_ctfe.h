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
#define XR_XIR_CTFE_MAX_CALL_DEPTH UINT32_C(128)
typedef enum XrXirCtfeStatus {
    XR_XIR_CTFE_OK, XR_XIR_CTFE_BAD_ARGUMENT, XR_XIR_CTFE_BAD_ARTIFACT,
    XR_XIR_CTFE_PROFILE, XR_XIR_CTFE_IDENTITY, XR_XIR_CTFE_NOT_LEAF,
    XR_XIR_CTFE_BUDGET, XR_XIR_CTFE_OUT_OF_MEMORY, XR_XIR_CTFE_STEP_LIMIT,
    XR_XIR_CTFE_FRAME_LIMIT, XR_XIR_CTFE_OUTPUT_LIMIT,
    XR_XIR_CTFE_DIVIDE_BY_ZERO, XR_XIR_CTFE_NUMERIC_RANGE,
    XR_XIR_CTFE_DEPTH_LIMIT, XR_XIR_CTFE_NOT_SUPPORTED
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

/* Receive a closed compiler value family from its structurally verified pool.
 * This ledger-owned iterative admission accepts primitive/String/Unit and
 * immutable Tuples, rejects cycles and authority-bearing fields, and grants no
 * execution permission. Failure preserves all caller metadata. */
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_type_admit(const XrXirCompileContext *context,
    const XrXirTypes *types, XrXirType type);

/* Borrowed input owners must remain live for this synchronous call. The selected
 * ordinary function must have bool/integer/Float/Rune/String inputs and results or Unit results, with
 * frame-owned locals and private Cells of admitted value families. Immutable
 * Tuples of these values can be constructed/projected and exported by logical
 * field-byte sum; Unit fields occupy zero output bytes. External Tuple parameters
 * require bounded cross-arena receiving and currently reject. Private storage
 * in a bounded admitted opcode graph. Local
 * writes produce Unit without a result slot. Every call repeats full artifact
 * verification on its original compiler ledger; root/effect flags never grant
 * purity. Limits may deny all resources and must not exceed the fixed ceilings.
 * Float operations use the existing integer-only IEEE kernel and prepay a
 * fixed-width work reserve on the original ledger. Canonical f32/f64 values
 * export four/eight target bytes; numeric conversion range errors preserve output.
 * Output must be canonical Unit. Failure preserves it, and consumed compiler
 * allocation/work is never refunded. Output bytes measure target logical storage,
 * not the host carrier size. No source syntax, calls or runtime fallback is implied. */
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_leaf(const XrXirCtfeRequest *request,
    XrXirValue *output);
/* Direct calls execute only a fully checked scalar/Unit/String/Tuple reachable closure,
 * including frame-owned locals and private Cells on the compiler ledger.
 * Numeric/CFG execution uses the common kernel. Depth
 * and live activation bytes bound the whole stack; steps/work are never reset
 * on a callee or refunded on return. Admission includes unused reachable body
 * operations and does not derive determinism from any ROOT/effect flags.
 * Borrowed inputs stay live during this synchronous call. All failures preserve
 * canonical Unit output. String exports own their compiler storage and measure
 * UTF-8 bytes. Comparison/search use ordinary Unicode index and byte-order rules,
 * prepay scan/table work and report NUMERIC_RANGE on invalid starts. Immutable
 * Tuple exports retain their type arena and fields. Other owned exports and
 * non-private managed values are separate
 * boundaries; this entry never invokes Instance, native code or a fallback. */
typedef struct XrXirCtfeCallRequest {
    XrXirCtfeRequest evaluation;
    uint32_t call_depth;
} XrXirCtfeCallRequest;
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_calls(const XrXirCtfeCallRequest *request,
    XrXirValue *output);
#endif // XXIR_CTFE_H
