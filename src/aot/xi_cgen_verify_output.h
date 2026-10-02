/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_verify_output.h - CGen output well-formedness verifier
 *
 * Task 218 defense line 3. Every generated C translation unit is checked
 * for structural well-formedness *before* it is written to disk / handed to
 * the C toolchain. This turns the historical "CGen streaming misalignment
 * produces corrupt C, then clang rolls the dice" accident class (task 190 /
 * 212 / wasm3) into a loud, fail-closed internal compiler error with a full
 * on-disk dump of the offending source. There is no bypass switch:
 * well-formedness is not negotiable.
 *
 * The structural passes are string / char / comment aware. Their scratch
 * storage and actual traversal work use the caller's compiler ledger.
 */

#ifndef XI_CGEN_VERIFY_OUTPUT_H
#define XI_CGEN_VERIFY_OUTPUT_H

#include "../base/xdefs.h"
#include "../base/xcompile_resources.h"
#include <stdbool.h>
#include <stddef.h>

/* Categories of generated-C well-formedness violations. W1-W4 map to the
 * four structural invariants in the task 218 design doc. */
typedef enum XiCgenVerifyCategory {
    XI_CGEN_VERIFY_OK = 0,
    XI_CGEN_VERIFY_W1_BALANCE,     /* brace/paren/quote/comment imbalance */
    XI_CGEN_VERIFY_W2_IDENTIFIER,  /* path/space/source fragment in a symbol */
    XI_CGEN_VERIFY_W3_SCOPE,       /* statement-shaped line at file scope */
    XI_CGEN_VERIFY_W4_FORWARD_REF, /* vN used before it is defined in a function */
    XI_CGEN_VERIFY_C90_RESTRICTED, /* C99/C11 or governed-runtime residue in C90 */
} XiCgenVerifyCategory;

typedef struct XiCgenVerifyResult {
    XiCgenVerifyCategory category; /* XI_CGEN_VERIFY_OK when well-formed */
    int line;                      /* 1-based line of the violation (0 if none) */
    char message[256];             /* human-readable detail */
} XiCgenVerifyResult;

typedef enum XiCgenVerifyStatus {
    XI_CGEN_VERIFY_PASSED = 0,
    XI_CGEN_VERIFY_OUT_OF_MEMORY = 1,
    XI_CGEN_VERIFY_BAD_ARGUMENT = 2,
    XI_CGEN_VERIFY_MALFORMED = 3,
    XI_CGEN_VERIFY_BUDGET = 4,
} XiCgenVerifyStatus;

/* Structural check of a generated C translation unit.
 *
 * PASSED clears the optional diagnostic; MALFORMED reports the first
 * highest-priority W1 > W2 > W3 > W4 violation. Resource or argument failures
 * preserve diagnostic bytes. NULL source is legal only with zero length.
 * Resources are mandatory and borrowed for this synchronous call. Scratch
 * blocks include ledger allocation overhead and coexist during growth.
 * Work charges precede each source/keyword byte read, byte-pair comparison,
 * traversal iteration, definition lookup/store, and diagnostic conversion;
 * copies, clears and writes charge their actual byte counts. Scalar control
 * state and static keyword tables do not allocate. Inputs above INT_MAX-1
 * bytes return BUDGET before reading, preserving bounded line/depth indices. */
XR_FUNC XiCgenVerifyStatus xr_compile_cgen_verify_output(XrCompileResources *resources,
    const char *c_src, size_t len, XiCgenVerifyResult *out);

/* Additional fail-closed policy check for XI_CGEN_C_DIALECT_C90 output.  This
 * rejects syntax/runtime residue outside the governed ISO C90 kernel subset;
 * it is intentionally separate from the language-neutral W1-W4 verifier. */
XR_FUNC XiCgenVerifyStatus xr_compile_cgen_verify_c90_output(XrCompileResources *resources,
    const char *c_src, size_t len, XiCgenVerifyResult *out);

/* Fail-closed wrapper used at the C-write boundary in the AOT driver.
 * Verifies the generated TU; on violation it reports an internal compiler
 * error (translation unit + category + line + detail), dumps the full
 * generated C to a diagnostics file, and aborts so malformed C can never
 * reach the C toolchain. Never returns on malformed C. Returns the exact
 * resource or argument failure without accepting unchecked output. */
XR_FUNC XiCgenVerifyStatus xr_compile_cgen_verify_output_or_ice(XrCompileResources *resources,
    const char *c_src, size_t len, const char *tu_name);

/* Stable short name for a category, e.g. "W1_BALANCE". */
XR_FUNC const char *xi_cgen_verify_category_name(XiCgenVerifyCategory category);

#endif /* XI_CGEN_VERIFY_OUTPUT_H */
