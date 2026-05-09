/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_verify.h - IR verification pass for Xi IR
 *
 * Checks structural and semantic invariants of the SSA IR:
 *
 * Structural (fast):
 *   1. Every value has a non-NULL type.
 *   2. Every value's op is within valid range.
 *   3. Every value's block pointer matches the block it's in.
 *   4. Phi node arg count matches predecessor count.
 *   5. CFG edges are consistent (succ/pred symmetry).
 *   6. Entry block has no predecessors.
 *   7. Block terminator is well-formed for its kind.
 *
 * Semantic (unconditional):
 *   8. SSA dominance: each use is dominated by its def.
 *      Phi arg[i] dominated by pred[i].
 *   9. Operand arity: nargs matches static expectation per XiOp.
 *  10. Type contracts: comparisons produce bool, XI_SELECT condition
 *      is bool, XI_EXTRACT source is call/multi_ret.
 *  11. Side-effect flags: store/throw/print/etc. must carry
 *      XI_FLAG_SIDE_EFFECT.
 *
 * On failure: returns false and writes a diagnostic message to the
 * caller-provided buffer.
 */

#ifndef XI_VERIFY_H
#define XI_VERIFY_H

#include "xi.h"

/* Verify structural invariants of an XiFunc.
 * Returns true if valid, false if an invariant is violated.
 * On failure, writes a human-readable diagnostic to 'errbuf'
 * (up to errbuf_size bytes, NUL-terminated). */
XR_FUNC bool xi_verify(const XiFunc *f, char *errbuf, int errbuf_size);

/* Stage-specific verifiers — run the base xi_verify() plus additional
 * checks appropriate for the given stage. Each succeeding stage
 * includes all checks from earlier stages.
 *
 * Return true if valid.  On failure, the diagnostic goes to errbuf. */
XR_FUNC bool xi_verify_stage(const XiFunc *f, XiStage stage, char *errbuf, int errbuf_size);

#endif  // XI_VERIFY_H
