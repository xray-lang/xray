/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_generated.h - Owned generated Built input and ordinary checking
 *
 * KEY CONCEPT:
 *   Owning generated data preserves its lifetime, never its semantic authority.
 */
#ifndef XXIR_GENERATED_H
#define XXIR_GENERATED_H
#include "xxir.h"

typedef struct XrXirGeneratedBuilt XrXirGeneratedBuilt;
typedef struct XrXirGeneratedText {
    const char *bytes;
    uint32_t length;
} XrXirGeneratedText;
typedef struct XrXirGeneratedOrigin {
    XrXirGeneratedText module, file, field_path;
    uint32_t line, column;
} XrXirGeneratedOrigin;
/* Each instruction has an explicit user origin. The entry covers declaration
 * and signature failures whose diagnostic has no instruction ordinal. Global
 * failures use the program entry's origin, or the first function of a library. */
typedef struct XrXirGeneratedFunctionOrigins {
    XrXirGeneratedOrigin entry;
    const XrXirGeneratedOrigin *instructions;
    uint32_t instruction_count;
} XrXirGeneratedFunctionOrigins;
typedef struct XrXirGeneratedReceive {
    const XrXirModule *built;
    const XrXirConstruction *construction;
    const XrXirGeneratedFunctionOrigins *origins;
    uint32_t function_count;
} XrXirGeneratedReceive;
/* origin borrows the live generated owner. Copy it before freeing that owner.
 * Neither a location nor a successful receive grants private member access. */
typedef struct XrXirGeneratedDiagnostic {
    XrXirDiagnostic cause;
    const XrXirGeneratedOrigin *origin;
} XrXirGeneratedDiagnostic;

/* Synchronously copies all metadata, function data and origin bytes onto the
 * original ledger. Physical/metadata admission is not body checking. Only Built
 * is accepted; a generator's Checked artifact or proof cannot substitute it. */
XR_FUNC XrXirStatus xr_xir_compile_generated_receive(const XrXirCompileContext *,
    const XrXirGeneratedReceive *, XrXirGeneratedBuilt **);
/* Runs the ordinary complete checker afresh, including current bodies, access,
 * constraints, ownership, borrow escape and inferred effects. Success publishes
 * an independent Checked owner; failure preserves the empty output. Retrying
 * uses the same cumulative ledger without refunds or prepared PASS facts. */
XR_FUNC XrXirStatus xr_xir_compile_generated_check(const XrXirGeneratedBuilt *,
    XrXirArtifact **, XrXirGeneratedDiagnostic *);
XR_FUNC void xr_xir_compile_generated_free(XrXirGeneratedBuilt *);
#endif // XXIR_GENERATED_H
