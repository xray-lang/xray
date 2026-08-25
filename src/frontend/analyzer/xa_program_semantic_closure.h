/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_program_semantic_closure.h - Analyzer publication to PSC bridge
 */

#ifndef XA_PROGRAM_SEMANTIC_CLOSURE_H
#define XA_PROGRAM_SEMANTIC_CLOSURE_H

#include "../../plan/semantic/xr_program_semantic_closure.h"

struct XaTypedProgram;
struct XaAnalyzer;
struct AstNode;
struct XrModuleSpec;

typedef enum XaProgramSemanticClosurePublishStatus {
    XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED = 0,
    XA_PROGRAM_SEMANTIC_CLOSURE_READY,
    XA_PROGRAM_SEMANTIC_CLOSURE_INVALID,
    XA_PROGRAM_SEMANTIC_CLOSURE_RESOURCE_FAILURE,
} XaProgramSemanticClosurePublishStatus;

/* Project the verified bounded scalar snapshot. No AST, analyzer database,
 * Xi, TargetPlan, layout, representation, or ABI fact is consulted here. */
XR_FUNC bool xa_typed_program_build_scalar_closure(const struct XaTypedProgram *typed_program,
                                                   XrProgramSemanticClosure **out, char *error,
                                                   size_t error_size);

/* Publish the first structurally eligible leaf-value aggregate family while
 * analyzer facts are still live. The resulting PSC is frozen, verified, and
 * contains no analyzer or target pointer. */
XR_FUNC XaProgramSemanticClosurePublishStatus xa_program_semantic_closure_publish_leaf_aggregate(
    struct XaAnalyzer *analyzer, const struct AstNode *syntax,
    const struct XrModuleSpec *module_spec, XrProgramSemanticClosure **out, char *error,
    size_t error_size);

#endif  // XA_PROGRAM_SEMANTIC_CLOSURE_H
