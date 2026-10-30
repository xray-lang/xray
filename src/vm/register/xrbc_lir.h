/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir.h - Owned register bytecode compiled from immutable scalar SSA.
 *
 * KEY CONCEPT:
 *   The compiler reads only public LIR views. Publication is atomic and the
 *   resulting program never borrows its input module or diagnostic strings.
 */
#ifndef XRBC_LIR_H
#define XRBC_LIR_H

#include "xrbc.h"
#include "lir/xlir.h"
#include "runtime/native/xnative_locations.h"

typedef struct XrRbcProgram XrRbcProgram;
typedef enum {
    XR_RBC_COMPILE_OK,
    XR_RBC_COMPILE_INVALID,
    XR_RBC_COMPILE_UNSUPPORTED,
    XR_RBC_COMPILE_LIMIT,
    XR_RBC_COMPILE_OUT_OF_MEMORY
} XrRbcCompileStatus;

typedef struct {
    XrRbcCompileStatus status;
    uint32_t function, block, instruction, line, column;
    // Long paths are truncated; the diagnostic owns the retained prefix.
    char path[256], message[192];
} XrRbcCompileDiagnostic;

// Entry identifies the execution root. Only its direct-call closure is
// admitted, but the whole input is verified at this trust boundary. Output
// must initially be NULL and remains NULL on failure. I64, I1, unit and raw
// F64 transport, canonical F64 negation, arithmetic and comparisons are supported,
// as are an I64-to-I64 INTCAST and signed64/binary64 cross conversions. Other
// integer and floating conversions, including same-width FCAST, are not admitted.
// Native calls bind the shared i64 output and fixed typed system
// clocks and the I64-to-I64 UTC offset query. F64 arguments
// retain every payload bit. I64 cells belong to an activation and may be
// transported through verified internal calls and block parameters. Cells
// cannot be host entry arguments, results, constants or native arguments.
// Verified literal-address/length pairs used only by direct String output are
// compile-time operands, not general PTR/U64 values or register contents.
// The Program owns the selected immutable bytes independently of its input.
XR_FUNC XrRbcCompileStatus xr_rbc_compile(const XrLirModule *input, XrLirFunction entry,
    XrRbcProgram **output, XrRbcCompileDiagnostic *diagnostic);
XR_FUNC void xr_rbc_program_destroy(XrRbcProgram *program);
XR_FUNC XrRbcStatus xr_rbc_program_prepare(const XrRbcProgram *program, const uint64_t *arguments,
    size_t argument_count, XrRbcExecution **output);
// The view and everything reachable through it borrow the program.
XR_FUNC const XrRbcModule *xr_rbc_program_view(const XrRbcProgram *program);
// Borrowed immutable metadata, including a real owner for empty tables. Retain
// before using it beyond Program destruction; release only references you own.
XR_FUNC XrtLocationOwner *xr_rbc_program_locations(const XrRbcProgram *program);

#endif  // XRBC_LIR_H
