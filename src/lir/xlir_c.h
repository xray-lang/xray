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
#include "../base/xcompile_resources.h"

typedef enum XrLirCStatus {
    XR_LIR_C_OK,
    XR_LIR_C_BAD_ARGUMENT,
    XR_LIR_C_INVALID_LIR,
    XR_LIR_C_UNSUPPORTED,
    XR_LIR_C_OUT_OF_MEMORY,
    XR_LIR_C_BUDGET,
    XR_LIR_C_CANCELLED
} XrLirCStatus;

typedef struct XrLirCRequest {
    const XrLirModule *module;
    XrLirFunction entry;
    XrCompileResources *resources;
    const char *translation_unit;
} XrLirCRequest;

typedef struct XrLirCOutput {
    char *bytes;
    size_t length;
    XrLirType entry_result;
} XrLirCOutput;

typedef struct XrLirCDiagnostic {
    XrLirCStatus status;
    uint32_t function, block, instruction;
    XrLirSource source;
    char message[192];
} XrLirCDiagnostic;

/* The public consumption boundary validates the finished module, then admits
 * only the selected direct-call closure. Inputs are borrowed synchronously.
 * Diagnostics borrow source paths until module destruction. Output must be
 * empty; failure publishes no text. Successful text owns its resource block
 * independently of the module and the caller's ledger reference. Every TU
 * passes the mandatory existing W1-W4 ICE boundary before publication.
 * xray_lir_entry is an ordinary exported C function, with no parameters and
 * entry_result as its C result type. There is no process exit-code mapping. */
XR_FUNC XrLirCStatus xr_lir_emit_c(const XrLirCRequest *, XrLirCOutput *, XrLirCDiagnostic *);
XR_FUNC void xr_lir_c_output_free(XrLirCOutput *);
#endif  // XLIR_C_H
