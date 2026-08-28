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
struct XrModuleGraph;
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

/* Publish the anonymous pointer-free tuple6 return family and every bounded
 * same-module direct-local caller. No declaration locator or class identity is
 * synthesized for the value product. */
XR_FUNC XaProgramSemanticClosurePublishStatus xa_program_semantic_closure_publish_leaf_product(
    struct XaAnalyzer *analyzer, const struct AstNode *syntax,
    const struct XrModuleSpec *module_spec, XrProgramSemanticClosure **out, char *error,
    size_t error_size);

/* Publish the bounded two-source-module scalar graph while the shared analyzer
 * and its exact import/export joins are live. A structurally matching graph is
 * fail-closed: missing dependency, target, type, effect, or source authority is
 * invalid rather than unsupported. */
XR_FUNC XaProgramSemanticClosurePublishStatus
xa_program_semantic_closure_publish_scalar_module_graph(struct XaAnalyzer *analyzer,
                                                        const struct XrModuleGraph *graph,
                                                        XrProgramSemanticClosure **out, char *error,
                                                        size_t error_size);

/* Publish one source-derived namespace-import call through an exported
 * nullary i64 wrapper to one generated private native target leaf. The
 * complete source graph remains topology authority; only the two selected
 * source functions and their two resolved calls become executable authority. */
XR_FUNC XaProgramSemanticClosurePublishStatus
xa_program_semantic_closure_publish_source_module_scalar_private_leaf_call(
    struct XaAnalyzer *analyzer, const struct XrModuleGraph *graph, XrProgramSemanticClosure **out,
    char *error, size_t error_size);

/* Freeze the complete bounded acyclic source-module graph reachable from the
 * graph entry. This publication carries modules and exact resolved source
 * edges only; downstream executable authority must be added by a later slice. */
XR_FUNC XaProgramSemanticClosurePublishStatus
xa_program_semantic_closure_publish_source_module_graph(struct XaAnalyzer *analyzer,
                                                        const struct XrModuleGraph *graph,
                                                        XrProgramSemanticClosure **out, char *error,
                                                        size_t error_size);

#endif  // XA_PROGRAM_SEMANTIC_CLOSURE_H
