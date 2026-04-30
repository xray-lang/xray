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

XR_FUNC int xi_lower_var_create(XiLower *l, const char *name, struct XrType *type);
XR_FUNC void xi_lower_braun_write(XiLower *l, int var_id, XiBlock *blk, XiValue *val);
XR_FUNC XiValue *xi_lower_braun_read(XiLower *l, int var_id, XiBlock *blk);
XR_FUNC void xi_lower_braun_seal(XiLower *l, XiBlock *blk);

/* ========== AST Lowering Primitives ========== */

XR_FUNC XiValue *xi_lower_expr(XiLower *l, struct AstNode *node);
XR_FUNC void xi_lower_stmt(XiLower *l, struct AstNode *node);
XR_FUNC struct XrType *xi_lower_node_type(XiLower *l, struct AstNode *node);

/* ========== Compound Statement Lowering (xi_lower_stmt.c) ========== */

XR_FUNC void xi_lower_select(XiLower *l, struct AstNode *node);
XR_FUNC void xi_lower_scope_block(XiLower *l, struct AstNode *node);
XR_FUNC void xi_lower_for_in(XiLower *l, struct AstNode *node);
XR_FUNC void xi_lower_try_catch(XiLower *l, struct AstNode *node);
XR_FUNC XiValue *xi_lower_match(XiLower *l, struct AstNode *node);
XR_FUNC XiValue *xi_lower_pattern_test(XiLower *l, XiValue *subject, struct AstNode *pattern);

#endif  // XI_LOWER_INTERNAL_H
