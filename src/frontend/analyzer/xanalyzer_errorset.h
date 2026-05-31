/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_errorset.h - Error set inference pass (Pass 3)
 *
 * KEY CONCEPT:
 *   After Pass 2 (type inference), this pass walks the AST again to
 *   infer which enum error types each function may produce.
 *   Results are stored in XaSymbolLinks.error_set and
 *   XrType.function.error_set.
 *
 * WHY THIS DESIGN:
 *   - Purely static, no runtime changes needed
 *   - Runs after type inference so all types are resolved
 *   - Supports fixpoint iteration for recursive functions
 */

#ifndef XANALYZER_ERRORSET_H
#define XANALYZER_ERRORSET_H

#include "xanalyzer.h"
#include "../parser/xast.h"
#include "../../base/xdefs.h"

/* Run error set inference on the analyzed AST.
 * Must be called after xa_analyze_ast() / xa_visit_infer().
 * Populates XaSymbolLinks.error_set for every function symbol. */
XR_FUNC void xa_infer_error_sets(XaAnalyzer *analyzer, AstNode *ast);

#endif /* XANALYZER_ERRORSET_H */
