/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_lower_internal.h - Shared declarations between xi_lower.c and xi_lower_stmt.c
 *
 * Internal header: do not include from outside the xi_lower_* translation units.
 */

#ifndef XI_LOWER_INTERNAL_H
#define XI_LOWER_INTERNAL_H

#include "xi_lower.h"
#include "xi.h"
#include "../base/xdefs.h"

struct AstNode;
struct XrType;

/* ========== Braun SSA Primitives ========== */

XR_FUNC int xi_lower_var_create(XiLower *l, uint32_t symbol_id,
                                 const char *name, struct XrType *type);
XR_FUNC void xi_lower_braun_write(XiLower *l, int var_id, XiBlock *blk, XiValue *val);
XR_FUNC XiValue *xi_lower_braun_read(XiLower *l, int var_id, XiBlock *blk);
XR_FUNC void xi_lower_braun_seal(XiLower *l, XiBlock *blk);

/* ========== Variable / Scope Lookup (xi_lower.c) ========== */

XR_FUNC int xi_lower_var_find(XiLower *l, uint32_t symbol_id, const char *name);
XR_FUNC int xi_lower_find_shared(XiLower *l, uint32_t symbol_id,
                                  const char *name, struct XrType **out_type);
XR_FUNC int xi_lower_resolve_upvalue(XiLower *l, uint32_t symbol_id,
                                      const char *name, struct XrType **out_type);

/* ========== Context Init / Cleanup (xi_lower.c) ========== */

XR_FUNC void xi_lower_init(XiLower *l, struct XaAnalyzer *analyzer,
                            struct XrayIsolate *isolate);
XR_FUNC void xi_lower_cleanup(XiLower *l);

/* ========== Function Lowering (xi_lower.c) ========== */

XR_FUNC XiFunc *xi_lower_func_impl(struct AstNode *func_node,
                                     struct XaAnalyzer *analyzer,
                                     struct XrayIsolate *isolate,
                                     XiLower *parent_ctx);

/* ========== AST Lowering Primitives ========== */

XR_FUNC XiValue *xi_lower_expr(XiLower *l, struct AstNode *node);
XR_FUNC void xi_lower_stmt(XiLower *l, struct AstNode *node);
XR_FUNC struct XrType *xi_lower_node_type(XiLower *l, struct AstNode *node);

/* ========== Cross-boundary helpers (xi_lower_expr.c, called from stmt) ========== */

XR_FUNC XiValue *xi_lower_function_decl(XiLower *l, struct AstNode *node);
XR_FUNC void xi_lower_enum_decl(XiLower *l, struct AstNode *node);
XR_FUNC void xi_lower_class_decl(XiLower *l, struct AstNode *node);

/* ========== Method Symbol Resolution ========== */

/* Resolve a method name to a global SymbolId through the isolate's symbol
 * table.  Runs during lowering (main thread), so the isolate is always
 * available.  Returns 0 on failure. */
XR_FUNC int32_t xi_lower_method_symbol(XiLower *l, const char *method_name);

/* ========== Compound Statement Lowering (xi_lower_stmt.c) ========== */

XR_FUNC void xi_lower_select(XiLower *l, struct AstNode *node);
XR_FUNC void xi_lower_scope_block(XiLower *l, struct AstNode *node);
XR_FUNC void xi_lower_for_in(XiLower *l, struct AstNode *node);
XR_FUNC void xi_lower_try_catch(XiLower *l, struct AstNode *node);
XR_FUNC XiValue *xi_lower_match(XiLower *l, struct AstNode *node);
XR_FUNC XiValue *xi_lower_pattern_test(XiLower *l, XiValue *subject, struct AstNode *pattern);

#endif  // XI_LOWER_INTERNAL_H
