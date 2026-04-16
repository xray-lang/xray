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
 *   Statically determines which variables escape to other coroutines
 *   (via 'move' in go/ch.send). Escaped variables are auto-promoted
 *   to shared allocation without requiring explicit 'shared' keyword.
 *
 * WHY THIS DESIGN:
 *   - Explicit 'shared' remains as a performance hint (always global heap)
 *   - Without 'shared', escape analysis decides allocation automatically
 *   - Function-local analysis only (no cross-function inference)
 */

#ifndef XANALYZER_ESCAPE_H
#define XANALYZER_ESCAPE_H

#include "../parser/xast_nodes.h"

/*
 * Run escape analysis on the AST.
 * Marks VarDeclNode.is_escaped = true for variables that escape
 * via 'move' in go arguments or ch.send arguments.
 *
 * Must be called after parsing, before codegen.
 */
void xa_escape_analyze(AstNode *ast);

#endif // XANALYZER_ESCAPE_H
