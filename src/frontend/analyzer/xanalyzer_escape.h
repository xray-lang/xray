/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_escape.h - Coroutine sharing validation pass
 *
 * KEY CONCEPT:
 *   Validates the explicit-sharing model for coroutine boundaries.
 *   This pass is purely diagnostic: it emits compile errors for
 *   disallowed patterns and does not mutate the AST.
 *
 * RULES ENFORCED:
 *   - Plain `let x` captured by a go closure           -> ERROR
 *     (must be passed via argument or declared `shared const`)
 *   - Mutable `shared let x` captured by a go closure  -> ERROR
 *     (only `shared const` may be captured; `shared let` is move-only)
 *   - `move x` where `x` is a plain `let`              -> ERROR
 *     (move requires `shared let`)
 *   - `move x` where `x` is `const` / `shared const`   -> handled by
 *     xa_visit_move_expr in the type checker
 *
 * Function-local analysis only (no cross-function inference).
 */

#ifndef XANALYZER_ESCAPE_H
#define XANALYZER_ESCAPE_H

#include "../parser/xast_nodes.h"

/* Forward declaration to avoid analyzer header dependency. */
typedef struct XaAnalyzer XaAnalyzer;

/*
 * Run escape analysis on the AST.
 *
 * - analyzer: optional analyzer handle for emitting diagnostics. When NULL,
 *   the pass stays silent (used by tools that run escape analysis outside
 *   the main compile pipeline). When non-NULL, violations of the explicit
 *   sharing model produce compile errors through the analyzer's diagnostic
 *   channel.
 *
 * Must be called after parsing, before codegen.
 */
void xa_escape_analyze(AstNode *ast, XaAnalyzer *analyzer);

#endif // XANALYZER_ESCAPE_H
