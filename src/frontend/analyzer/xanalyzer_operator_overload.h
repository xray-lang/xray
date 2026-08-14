/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_operator_overload.h - Static selection of arithmetic operator methods
 *
 * KEY CONCEPT:
 *   `a + b` where `a` is a class instance is a call to that class's `operator+`
 *   (spec §5.3.5). Xray monomorphizes, so which method runs is a fact the front
 *   end can settle, and it must: resolving it at run time was a VM-only
 *   capability. The AOT arithmetic helper reinterprets an instance pointer as a
 *   double, and its target plan has no call authority for an arithmetic opcode
 *   that produces an object.
 *
 *   This resolves the method and reports every way the resolution can fail.
 *   The caller publishes the selection fact and the result type; the
 *   canonicalizer later rewrites the node into the method call it denotes.
 */

#ifndef XANALYZER_OPERATOR_OVERLOAD_H
#define XANALYZER_OPERATOR_OVERLOAD_H

#include "../../base/xdefs.h"
#include "xanalyzer_infer.h"

struct AstNode;
struct XrType;
struct XaSymbol;

typedef enum XaOperatorOverloadStatus {
    /* Neither operand is a class instance: this is not an overload site and
     * the caller keeps whatever it had concluded. */
    XA_OPERATOR_OVERLOAD_NOT_APPLICABLE = 0,
    /* Resolved. out_method and out_fn_type are filled in. */
    XA_OPERATOR_OVERLOAD_RESOLVED,
    /* An operand is a class instance but no usable operator method applies.
     * A diagnostic has been reported; the expression is an error type. */
    XA_OPERATOR_OVERLOAD_REJECTED,
} XaOperatorOverloadStatus;

/* Resolve `left <op> right` against the left operand class's operator method.
 * Only the five arithmetic operators are considered; any other node type is
 * NOT_APPLICABLE. */
XR_FUNC XaOperatorOverloadStatus xa_resolve_binary_operator_overload(
    XaInferContext *ctx, struct AstNode *node, struct XrType *left, struct XrType *right,
    struct XaSymbol **out_method, struct XrType **out_fn_type);

#endif  // XANALYZER_OPERATOR_OVERLOAD_H
