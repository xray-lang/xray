/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcanon.h - Typed AST canonicalizer
 *
 * KEY CONCEPT:
 *   Transforms a typed AST (post-analyzer) into canonical form before
 *   Xi IR lowering.  Canonicalization responsibilities:
 *     - Fix evaluation order for complex sub-expressions
 *     - Introduce temporary variables for multi-use receivers/indices
 *     - Expand compound assignments (+=, etc.)
 *     - Expand short-circuit logic into explicit control flow
 *     - Normalize for-in, match, defer, try/finally
 *     - Normalize top-level declaration order
 *
 *   The canonicalizer operates on AST nodes in-place (mutating),
 *   allocating new nodes through the isolate's arena.  It reads type
 *   and symbol info from the analyzer's node table and preserves
 *   node_id stability for existing nodes.
 */

#ifndef XCANON_H
#define XCANON_H

#include "../../base/xdefs.h"
#include <stdbool.h>

struct AstNode;
struct XaAnalyzer;
struct XrayIsolate;

/* Canonicalization status codes. */
typedef enum {
    XR_CANON_OK = 0,
    XR_CANON_ERR_NULL_INPUT,
    XR_CANON_ERR_INTERNAL,
} XrCanonStatus;

/* Canonicalize a program AST in-place. Must be called after the
 * analyzer has populated the typed node table.
 *
 * Returns XR_CANON_OK on success.  The AST is modified in-place;
 * newly synthesized nodes get fresh node_id values from the isolate. */
XR_FUNC XrCanonStatus xr_canon_program(struct AstNode *program,
                                        struct XaAnalyzer *analyzer,
                                        struct XrayIsolate *isolate);

/* Canonicalize a single function body. Used by the pipeline when
 * compiling individual functions rather than full programs. */
XR_FUNC XrCanonStatus xr_canon_func(struct AstNode *func_node,
                                     struct XaAnalyzer *analyzer,
                                     struct XrayIsolate *isolate);

#endif  // XCANON_H
