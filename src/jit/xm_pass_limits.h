/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_pass_limits.h - Centralised JIT optimisation pipeline budgets.
 *
 * KEY CONCEPT:
 *   All "magic" size caps used by the JIT's optimisation passes live
 *   here, in one clearly commented place.  Individual pass files must
 *   not redeclare their own #define <PASS>_MAX_* — import this header
 *   instead.  This prevents the silent drift the project has
 *   accumulated over time (same concept tuned differently in DSE vs
 *   LICM, etc.) and makes every budget trivially auditable.
 *
 * PHILOSOPHY:
 *   - Function-level hard caps (XM_MAX_FUNC_*): enforced via JIT
 *     eligibility so oversized inputs fall back to the interpreter
 *     rather than silently producing partial optimisation results.
 *   - Pass-level tuning knobs: reflect either real algorithmic limits
 *     (hash-table sizes) or engineering compromises (max inline
 *     depth).  When a budget is exceeded, the offending pass must
 *     skip gracefully, never corrupt IR.
 *
 * WHAT BELONGS HERE:
 *   - Budgets that reference "function size" directly (vregs, blocks,
 *     instructions).
 *   - Tunables for individual passes whose output depends on them
 *     (GVN/CSE hash table sizes, LICM iteration count, etc.).
 *
 * WHAT DOES NOT BELONG HERE:
 *   - Architectural limits tied to the target (XRA_MAX_GP_REGS,
 *     XM_MAX_SPILL_SLOTS) — those stay with the regalloc / target
 *     headers that own the invariant.
 *   - Runtime ABI limits (XM_SUSPEND_SPILL_MAX) — those belong with
 *     the struct definition they describe.
 *   - Eligibility checks — see xm_eligibility.h.
 */

#ifndef XM_PASS_LIMITS_H
#define XM_PASS_LIMITS_H

/* ========== Function-level Hard Caps ==========
 *
 * A function exceeding any of these bounds is considered too large for
 * the JIT pipeline and stays in the interpreter.  These are enforced
 * during eligibility, so passes can assume they operate within bounds.
 */

// Maximum virtual registers the JIT will attempt to allocate for.
#define XM_MAX_FUNC_VREGS 4096

// Maximum basic blocks per JIT-compiled function.
#define XM_MAX_FUNC_BLOCKS 4096

// Maximum total instructions summed over every block.
#define XM_MAX_FUNC_TOTAL_INS 65536

/* ========== CSE / GVN Hash Tables ==========
 *
 * Open-addressed tables; size grows as a power of two between these
 * bounds.  Exceeding MAX means the pass stops expanding and accepts
 * some collision-driven misses rather than unbounded memory use.
 */
#define XM_CSE_MIN_TABLE 64
#define XM_CSE_MAX_TABLE 1024

#define XM_GVN_MIN_TABLE 128

/* ========== LICM ==========
 *
 * LICM iterates per-loop until fixed-point; the iteration cap bounds
 * worst-case time on pathological chain-invariant IR.
 *
 * NOTE: there is intentionally no LICM_MAX_LOOPS / LICM_MAX_STORE_OBJS
 * cap here — LICM stores its loop set via XmLoopInfo's heap-allocated
 * representation, not a fixed-size LicmLoop[] array, so a static cap
 * would be redundant.
 */
#define XM_LICM_MAX_ITERATIONS 8  // previously 4

/* ========== If-Conversion ==========
 *
 * Triangles / diamonds small enough to be predicated into straight-line
 * code.  Raising these values enables more CMOV-style collapses at the
 * cost of speculatively executing both arms.
 */
#define XM_IFCONV_MAX_INS 6   // previously 2 — allow richer bodies
#define XM_IFCONV_MAX_PHIS 4  // previously 2

/* ========== Dead Store Elimination ==========
 *
 * Number of concurrently tracked (obj, offset) store sites in a single
 * block scan.  DSE falls back to a conservative "all stores live" view
 * once this is exceeded.
 */
#define XM_DSE_MAX_TRACKED 32

/* ========== Global Code Motion ==========
 *
 * GCM allocates bookkeeping dynamically from func->nblk / func->nvreg.
 * No separate caps needed — function-level XM_MAX_FUNC_{BLOCKS,VREGS}
 * bound the upper size via eligibility.
 */

/* ========== Range Analysis ==========
 *
 * Bounded fixed-point iterations for interval arithmetic over a
 * function's value graph.  Per-vreg arrays are heap-allocated from
 * func->nvreg; no separate cap — XM_MAX_FUNC_VREGS applies.
 */
#define XM_RA_MAX_ROUNDS 4

/* ========== Inlining (reserved for the inliner pass) ========== */
#define XM_INLINE_MAX_COUNT 16
#define XM_INLINE_MAX_SIZE 500

#endif  // XM_PASS_LIMITS_H
