/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_escape.h - Escape analysis pass
 *
 * KEY CONCEPT:
 *   Validates coroutine sharing rules and sets VarDeclNode.is_escaped
 *   for legitimate `move` uses on `shared let` variables. Under the
 *   explicit-sharing model, implicit auto-promotion is REJECTED:
 *   variables captured by go closures or moved across coroutine
 *   boundaries must be declared `shared` by the user.
 *
 * WHY THIS DESIGN:
 *   - No implicit allocation-site changes: what you write is what you get
 *   - Violations produce compile errors (not silent upgrades)
 *   - Function-local analysis only (no cross-function inference)
 *   - `move` with a plain `let` is rejected (must be `shared let`)
 *   - Non-shared captured variable in go closure is rejected
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
