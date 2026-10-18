/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_storage.h - Analyzer scratch and type storage on one compiler state
 */
#ifndef XANALYZER_STORAGE_H
#define XANALYZER_STORAGE_H

#include "../../base/xcompile_state.h"

struct XaAnalyzer;
struct AstNode;
struct XrCompilerSessionScope;

/* Arenas, type pool, symbol registry and current/global scopes must be empty.
 * Failure preserves all fields and the
 * original sticky state. Published arenas retain that same state after the
 * producing parser session is destroyed; no independent ledger is created. */
XR_FUNC XrCompileResourceStatus xa_analyzer_storage_open(
    XrCompileState *state, struct XaAnalyzer *analyzer);
/* Caller-owned analyzer storage is cleared before destruction. Cleanup
 * remains valid after resource failure and does not refund cumulative fees. */
XR_FUNC void xa_analyzer_storage_close(struct XaAnalyzer *analyzer);
/* The zero-initialized scope borrows the program arena for AST materialization.
 * Without a program arena, the caller's current scope remains selected.
 * Failure preserves bindings/output and the original CompileState failure.
 * A successful active scope must pop through xr_compile_session_pop_arena,
 * including when analysis subsequently fails. */
XR_FUNC XrCompileResourceStatus xa_analyzer_ast_scope_open(struct XaAnalyzer *analyzer,
    const struct AstNode *program, struct XrCompilerSessionScope *scope);

#endif // XANALYZER_STORAGE_H
