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

/* Insert ARC retain/release ops into f.
 * Must be called after xi_escape_analyze() and before xi_backend_lower().
 * Modifies the IR in place. */
XR_FUNC void xi_arc_insert(XiFunc *f);

/* Eliminate redundant retain/release pairs (copy→move optimization).
 * Must run AFTER xi_arc_insert. Removes RETAIN(v)+RELEASE(v) pairs where
 * the retain merely extends lifetime to a single forwarding consumer and
 * the release immediately follows (the pair is semantically a no-op move).
 * Returns the number of pairs eliminated. */
XR_FUNC int xi_arc_elim(XiFunc *f);

#endif  // XI_ARC_H
