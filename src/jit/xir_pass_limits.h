/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_pass_limits.h - Centralised JIT optimisation pipeline budgets.
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
 *   - Function-level hard caps (XIR_MAX_FUNC_*): enforced via JIT
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
 *     XIR_MAX_SPILL_SLOTS) — those stay with the regalloc / target
 *     headers that own the invariant.
 *   - Runtime ABI limits (XIR_SUSPEND_SPILL_MAX) — those belong with
 *     the struct definition they describe.
 *   - Opcode-support classifications — see xir_opcode_support.h.
 */

#ifndef XIR_PASS_LIMITS_H
#define XIR_PASS_LIMITS_H

/* ========== Function-level Hard Caps ==========
 *
 * A function exceeding any of these bounds is considered too large for
 * the JIT pipeline and stays in the interpreter.  These are enforced
 * during eligibility, so passes can assume they operate within bounds.
 */

// Maximum virtual registers the JIT will attempt to allocate for.
#define XIR_MAX_FUNC_VREGS        4096

// Maximum basic blocks per JIT-compiled function.
#define XIR_MAX_FUNC_BLOCKS       4096

// Maximum total instructions summed over every block.
#define XIR_MAX_FUNC_TOTAL_INS    65536

/* ========== CSE / GVN Hash Tables ==========
 *
 * Open-addressed tables; size grows as a power of two between these
 * bounds.  Exceeding MAX means the pass stops expanding and accepts
 * some collision-driven misses rather than unbounded memory use.
 */
#define XIR_CSE_MIN_TABLE         64
#define XIR_CSE_MAX_TABLE         1024

#define XIR_GVN_MIN_TABLE         128

/* ========== LICM ==========
 *
 * LICM iterates per-loop until fixed-point; the iteration cap bounds
 * worst-case time on pathological chain-invariant IR.
 *
 * NOTE: LICM_MAX_LOOPS / LICM_MAX_STORE_OBJS from the pre-phase-2
 * design are intentionally absent here — Phase 2.2 replaces the fixed
 * LicmLoop[] array with XirLoopInfo's heap-allocated representation.
 */
#define XIR_LICM_MAX_ITERATIONS   8      // previously 4

/* ========== If-Conversion ==========
 *
 * Triangles / diamonds small enough to be predicated into straight-line
 * code.  Raising these values enables more CMOV-style collapses at the
 * cost of speculatively executing both arms.
 */
#define XIR_IFCONV_MAX_INS        6      // previously 2 — allow richer bodies
#define XIR_IFCONV_MAX_PHIS       4      // previously 2

/* ========== Dead Store Elimination ==========
 *
 * Number of concurrently tracked (obj, offset) store sites in a single
 * block scan.  DSE falls back to a conservative "all stores live" view
 * once this is exceeded.
 */
#define XIR_DSE_MAX_TRACKED       32

/* ========== Global Code Motion ==========
 *
 * GCM sizes its per-vreg and per-block bookkeeping statically.  These
 * should eventually be switched to dynamic allocation tied to
 * XIR_MAX_FUNC_{VREGS,BLOCKS}; the values below keep current behaviour
 * for callers that inline them directly.
 */
#define XIR_GCM_MAX_BLOCKS        512
#define XIR_GCM_MAX_VREGS         4096

/* ========== Range Analysis ==========
 *
 * Bounded fixed-point iterations for interval arithmetic over a
 * function's value graph.
 */
#define XIR_RA_MAX_VALUES         512
#define XIR_RA_MAX_ROUNDS         4

/* ========== Inlining (reserved for Phase 4 pipeline) ========== */
#define XIR_INLINE_MAX_COUNT      16
#define XIR_INLINE_MAX_SIZE       500

#endif // XIR_PASS_LIMITS_H
