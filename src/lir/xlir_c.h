/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c.h - Owned portable C for an explicit scalar execution root
 *
 * KEY CONCEPT:
 *   The selected zero-argument function keeps its typed result. An execution
 *   adapter chooses how to consume it; emission never invents a source main.
 */
#ifndef XLIR_C_H
#define XLIR_C_H
#include "xlir.h"
#include "xlir_types.h"
#include "../base/xcompile_request.h"

typedef enum XrLirCStatus {
    XR_LIR_C_OK,
    XR_LIR_C_BAD_ARGUMENT,
    XR_LIR_C_INVALID_LIR,
    XR_LIR_C_UNSUPPORTED,
    XR_LIR_C_OUT_OF_MEMORY,
    XR_LIR_C_BUDGET,
    XR_LIR_C_CANCELLED,
    XR_LIR_C_MEMORY_LIMIT,
    XR_LIR_C_OVERFLOW,
    XR_LIR_C_DEADLINE,
    XR_LIR_C_CLOCK_FAILURE
} XrLirCStatus;

typedef struct XrLirCRequest {
    const XrLirModule *module;
    XrLirFunction entry;
    XrCompileResources *resources;
    const char *translation_unit;
    XrCompileRequestControl *control;
} XrLirCRequest;

typedef struct XrLirCOutput {
    char *bytes;
    size_t length;
    XrLirType entry_result;
    XrLirTypeView entry_shape;
    XrLirTypeLayout entry_layout;
} XrLirCOutput;

typedef struct XrLirCDiagnostic {
    XrLirCStatus status;
    uint32_t function, block, instruction;
    XrLirSource source;
    char message[192];
} XrLirCDiagnostic;

typedef struct XrLirValueLayouts XrLirValueLayouts;
typedef struct XrLirValueHomes XrLirValueHomes;
typedef struct XrLirCValueTypesRequest {
    const XrLirValueLayouts *layouts;
    XrCompileResources *resources;
    const char *translation_unit;
    XrCompileRequestControl *control;
} XrLirCValueTypesRequest;

/* Emit the receiving value plans as portable typed C declarations, without
 * consulting a compiler type pool. XrlValue_N names the set's one-based
 * dependency type ID; Record fields use fN and Nullable uses tag/payload.
 * Every owning leaf is an actual XrtManagedHeader* field. The fragment contains
 * only standard headers and an incomplete managed Header declaration, never
 * private runtime includes or an executable binding. It grants no ownership or
 * mutation permission. The borrowed set remains live throughout emission.
 * Output is empty on entry; successful text owns its receiving resource block
 * and survives set retirement. Entry facts stay zero. W1-W4 runs before text
 * publication, including for a declaration-only translation unit. */
XR_FUNC XrLirCStatus xr_lir_emit_c_value_types(const XrLirCValueTypesRequest *,
    XrLirCOutput *, XrLirCDiagnostic *);

typedef struct XrLirCValueTransfersRequest {
    const XrLirValueLayouts *layouts;
    const XrLirValueHomes *homes;
    XrCompileResources *resources;
    const char *translation_unit;
    XrCompileRequestControl *control;
} XrLirCValueTransfersRequest;
/* Emit the same typed declarations and callable function-specific stable-frame
 * and selected-edge helpers. Both borrowed inputs are the unchanged receiving
 * products for the exact final module. Home positions and owner events come
 * only from that common producer; this emitter computes no value layout or
 * lifetime analysis. Scalar/tag staging occurs inside the guarded publication
 * interval, before any replaced owner retires. POD-only edges use ordinary C
 * values and create no Root. Managed edge helpers accept an optional scalar
 * publication descriptor: its caller first protects every ordinary C scalar
 * source, and its callback writes only the corresponding non-owner destinations
 * in this same interval. It follows the public Place publication restrictions.
 * It returns normally, allocates nothing, calls no Root/Place or guest function,
 * and never writes an owner field. A rejected transfer never invokes it.
 * Return helpers require the real top callee frame and a distinct stable caller
 * landing of the exact result type; successful publication precedes callee leave.
 * This is executor preparation, not a second product
 * execution path or new guest mutation authority. Output follows the same
 * empty/publication/resource contract as the declaration emitter. */
XR_FUNC XrLirCStatus xr_lir_emit_c_value_transfers(const XrLirCValueTransfersRequest *,
    XrLirCOutput *, XrLirCDiagnostic *);

/* The public consumption boundary validates the finished module, then admits
 * only the selected direct-call closure. Inputs are borrowed synchronously.
 * Diagnostics borrow source paths until module destruction. Output must be
 * empty; failure publishes no text. Successful text owns its resource block
 * independently of the module and the caller's memory-owner reference. Control
 * is optional and borrowed only during this call; NULL disables request polling.
 * A supplied control shares its existing absolute deadline without resetting it.
 * Callbacks must keep all borrowed inputs live and unchanged. Polls occur at IR
 * traversal and validation boundaries, including immediately before publication.
 * Neither output nor its memory owner retains the control or parent request. Every TU
 * passes the mandatory existing W1-W4 ICE boundary before publication.
 * Result shape and layout are owned POD facts, including after module retirement.
 * xray_lir_entry has no parameters and keeps entry_result as its C result type.
 * With an output or possible arithmetic panic dependency it establishes the
 * runtime scope before invoking the complete selected root. Ordinary calls target typed workers;
 * without that dependency the root stays a direct function. Pure numeric leaves
 * can require the public runtime declarations without establishing an execution.
 * The complete TU, including the bound wrapper, is verified before publication.
 * There is no process exit-code mapping. */
XR_FUNC XrLirCStatus xr_lir_emit_c(const XrLirCRequest *, XrLirCOutput *, XrLirCDiagnostic *);
/* Emit the same typed root with a process main that discards its result. The
 * complete translation unit passes W1-W4 before publication. */
XR_FUNC XrLirCStatus xr_lir_emit_c_standalone(const XrLirCRequest *, XrLirCOutput *, XrLirCDiagnostic *);
/* Private test file. The caller selects authorized roles from the same plan.
 * Emission owns a single initializer and a table of typed action callbacks. */
XR_FUNC XrLirCStatus xr_lir_emit_c_test_session(const XrLirCRequest *, const XrLirFunction *actions,
    uint32_t count, XrLirCOutput *, XrLirCDiagnostic *);
XR_FUNC void xr_lir_c_output_free(XrLirCOutput *);
#endif  // XLIR_C_H
