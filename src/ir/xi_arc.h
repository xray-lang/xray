/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_arc.h - Automatic Reference Counting insertion pass
 *
 * Inserts XI_RETAIN and XI_RELEASE ops from precise owned/borrowed use
 * information. Runs after escape analysis and before backend lowering, while
 * semantic ops still expose stores, calls, returns, and projections directly.
 */

#ifndef XI_ARC_H
#define XI_ARC_H

#include "xi.h"

/* Rewrite NO_ESCAPE heap allocs to XI_STACK_ALLOC.
 * Must be called after xi_escape_analyze() and before xi_arc_insert().
 * Stores the original op in aux_int for codegen dispatch. */
XR_FUNC void xi_stack_alloc_rewrite(XiFunc *f);

/* Normalize owner forwarding and seal target-neutral parameter/return
 * ownership contracts on the pre-ARC graph. Callers that skip physical
 * retain/release insertion must still run this semantic stage before
 * constructing SemanticPlan. */
XR_FUNC void xi_arc_analyze_contracts(XiFunc *f);

/* Insert ARC retain/release ops into f.
 * Must be called after xi_escape_analyze() and before xi_backend_lower().
 * Modifies the IR in place. */
XR_FUNC void xi_arc_insert(XiFunc *f);

/* Exact post-analysis ownership action for a semantic operand. This includes
 * call-site borrow signatures and ARC-only exceptions that the generated
 * operation-wide ownership class cannot express. */
XR_FUNC bool xi_arc_operand_consumes(const XiFunc *function, const XiValue *operation,
                                     uint16_t operand);

/* Effective ownership convention of a semantic parameter after receiver,
 * operator, variadic-rest, and inferred borrow rules are combined. */
XR_FUNC uint8_t xi_arc_parameter_ownership(const XiFunc *function, const XiValue *parameter);

/* Exact post-analysis provenance of a semantic value. This is the same
 * recursive call/alias/freshness proof ARC uses for function returns. */
XR_FUNC XiReturnOwnership xi_arc_value_return_ownership(const XiFunc *function,
                                                        const XiValue *value);

/* Effective result-ownership class after following representation-only
 * adapters and resolving call freshness. The returned value is one of the
 * XI_GEN_RESULT_OWNERSHIP_* constants consumed by SemanticPlan. */
XR_FUNC uint8_t xi_arc_value_result_ownership(const XiFunc *function, const XiValue *value);

/* Immediate operand aliased by a call result, or -1 when the result is fresh,
 * static, unresolved, or not a call. The index is normalized to args[]. */
XR_FUNC int16_t xi_arc_value_alias_operand(const XiFunc *function, const XiValue *value);

/* Eliminate redundant retain/release pairs (copy→move optimization).
 * Must run AFTER xi_arc_insert. Removes RETAIN(v)+RELEASE(v) pairs where
 * the retain merely extends lifetime to a single forwarding consumer and
 * the release immediately follows (the pair is semantically a no-op move).
 * Returns the number of pairs eliminated. */
XR_FUNC int xi_arc_elim(XiFunc *f);

/* Finalize implicit error exits after ordinary ARC elimination.  Unit-typed
 * XI_ERR_CHECK values receive args[]=owned values that must be dropped only
 * when propagation returns from the function.  An associated may-throw
 * producer remains implicit so an unused result is not materialized; checks
 * whose producer was folded to proven-nothrow stay operand-free.  For AOT this
 * runs after all value-rewriting passes. */
XR_FUNC void xi_arc_attach_error_cleanups(XiFunc *f);

#endif  // XI_ARC_H
