/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_reuse.h - Drop-Reuse analysis pass (Perceus FBIP)
 *
 * Scans for patterns where a XI_RELEASE is followed by a heap allocation
 * of the same size class in the same basic block. When found, converts:
 *   XI_RELEASE(v)           -> XI_DROP_REUSE(v) producing a reuse token
 *   XI_xxx_NEW(...)         -> XI_ALLOC_AT(token, ...) using reclaimed memory
 *
 * The reuse token is NULL at runtime if the object was shared (RC > 1),
 * in which case XI_ALLOC_AT falls back to normal allocation.
 *
 * Must run AFTER xi_arc_insert (needs RELEASE ops to be present).
 */

#ifndef XI_REUSE_H
#define XI_REUSE_H

#include "xi.h"

/* Run drop-reuse analysis on f and all its children.
 * Rewrites XI_RELEASE -> XI_DROP_REUSE + XI_ALLOC_AT pairs in place. */
XR_FUNC void xi_reuse_insert(XiFunc *f);

#endif  // XI_REUSE_H
