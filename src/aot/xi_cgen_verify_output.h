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
 * The check is a single linear pass (string / char / comment aware) and is
 * cheap enough to stay always-on (<1% of AOT compile time).
 */

#ifndef XI_CGEN_VERIFY_OUTPUT_H
#define XI_CGEN_VERIFY_OUTPUT_H

#include "../base/xdefs.h"
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
} XiCgenVerifyCategory;

typedef struct XiCgenVerifyResult {
    XiCgenVerifyCategory category; /* XI_CGEN_VERIFY_OK when well-formed */
    int line;                      /* 1-based line of the violation (0 if none) */
    char message[256];             /* human-readable detail */
} XiCgenVerifyResult;

/* Single-pass structural check of a generated C translation unit.
 *
 * Returns true if the source is well-formed. On the first (highest-priority
 * W1 > W2 > W3 > W4) violation it returns false and fills *out. Pure and
 * side-effect free — safe to feed crafted malformed strings from unit tests.
 * A NULL/empty buffer is treated as well-formed. */
XR_FUNC bool xi_cgen_verify_output(const char *c_src, size_t len, XiCgenVerifyResult *out);

/* Fail-closed wrapper used at the C-write boundary in the AOT driver.
 * Verifies the generated TU; on violation it reports an internal compiler
 * error (translation unit + category + line + detail), dumps the full
 * generated C to a diagnostics file, and aborts so malformed C can never
 * reach the C toolchain. Never returns on violation. No bypass. */
XR_FUNC void xi_cgen_verify_output_or_ice(const char *c_src, size_t len, const char *tu_name);

/* Stable short name for a category, e.g. "W1_BALANCE". */
XR_FUNC const char *xi_cgen_verify_category_name(XiCgenVerifyCategory category);

#endif /* XI_CGEN_VERIFY_OUTPUT_H */
