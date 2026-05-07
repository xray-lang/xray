/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_arc.h - Automatic Reference Counting insertion pass
 *
 * Inserts XI_RETAIN and XI_RELEASE ops based on escape analysis.
 * Runs after escape analysis and backend lowering.
 *
 * Rules:
 *   - NO_ESCAPE values: no retain/release (stack lifetime)
 *   - ARG_ESCAPE values: retain at creation, release at function exit
 *     (caller takes ownership via return)
 *   - HEAP_ESCAPE values: retain at each new store-site,
 *     release when overwritten or at scope exit
 *   - GLOBAL_ESCAPE values: retain/release at all usage boundaries
 *
 * For the initial implementation (conservative), we insert:
 *   1. RETAIN after every heap-allocating op with escape > NO_ESCAPE
 *   2. RELEASE before every return (for all retained local values)
 *   3. Skip RETAIN/RELEASE entirely for NO_ESCAPE values
 */

#ifndef XI_ARC_H
#define XI_ARC_H

#include "xi.h"

/* Rewrite NO_ESCAPE heap allocs to XI_STACK_ALLOC.
 * Must be called after xi_escape_analyze() and before xi_arc_insert().
 * Stores the original op in aux_int for codegen dispatch. */
XR_FUNC void xi_stack_alloc_rewrite(XiFunc *f);

/* Insert ARC retain/release ops into f based on escape analysis.
 * Must be called after xi_escape_analyze() and xi_backend_lower().
 * Modifies the IR in place. */
XR_FUNC void xi_arc_insert(XiFunc *f);

#endif  // XI_ARC_H
