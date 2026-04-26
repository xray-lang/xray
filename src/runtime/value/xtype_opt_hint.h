/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_opt_hint.h - Type-derived optimization hints
 *
 * KEY CONCEPT:
 *   Pure XrType -> hint classification helpers. The result is consumed
 *   by both the analyzer (Pass 3 JIT metadata) and the codegen
 *   (constant folding, devirtualisation), so the helpers must NOT live
 *   in either layer. They sit next to XrType itself in
 *   runtime/value/.
 *
 * WHY THIS DESIGN:
 *   The type-classification subset lives in runtime/value (next to
 *   XrType) so that both the analyzer and the codegen can include it
 *   cleanly. Putting it under codegen/ would force the analyzer to
 *   take a downward analyzer->codegen include, which the architecture
 *   lints reject.
 */

#ifndef XTYPE_OPT_HINT_H
#define XTYPE_OPT_HINT_H

#include <stdbool.h>
#include "../../base/xdefs.h"

// Forward declaration; full definition in runtime/value/xtype.h.
struct XrType;

/*
 * Optimization hint derived from type analysis.
 *
 * NOTE: xray uses a 16-byte tagged union, so int/float/bool are stored
 * inline WITHOUT heap allocation. "Unboxing" here means skipping type
 * checks, not memory layout changes.
 *
 * Current status:
 * - XR_OPT_KNOWN_*: Skip runtime type checks (minor benefit)
 * - XR_OPT_INLINE_*: Not implemented (needs IC support in VM)
 * - XR_OPT_DEVIRT_CALL: Not implemented (needs class hierarchy)
 * - XR_OPT_ELIM_NULL_CHECK: Useful with flow analysis
 */
typedef enum XrOptHint {
    XR_OPT_NONE = 0,
    XR_OPT_KNOWN_INT,        // Type is definitely int, skip type check
    XR_OPT_KNOWN_FLOAT,      // Type is definitely float
    XR_OPT_KNOWN_BOOL,       // Type is definitely bool
    XR_OPT_KNOWN_STRING,     // Type is definitely string
    XR_OPT_KNOWN_NULL,       // Type is definitely null
    XR_OPT_INLINE_ARRAY,     // (Future) Array access inline cache
    XR_OPT_INLINE_MAP,       // (Future) Map access inline cache
    XR_OPT_INLINE_FIELD,     // (Future) Field access inline cache
    XR_OPT_DEVIRT_CALL,      // (Future) Devirtualize method call
    XR_OPT_ELIM_NULL_CHECK,  // Skip null check (non-nullable type)
} XrOptHint;

// Classify a type into an optimisation hint. Returns XR_OPT_NONE on
// NULL or for kinds with no specific hint.
XR_FUNC XrOptHint xr_opt_get_hint(struct XrType *type);

// True if both operands of an arithmetic op are unboxable numeric
// primitives (int / float). NULL operands -> false.
XR_FUNC bool xr_opt_can_unbox_arith(struct XrType *left, struct XrType *right);

// True if the receiver type and method name allow devirtualising the
// call (i.e. the concrete target is statically known). NULL -> false.
XR_FUNC bool xr_opt_can_devirt(struct XrType *receiver_type, const char *method);

#endif  // XTYPE_OPT_HINT_H
