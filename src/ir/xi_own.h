/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_own.h - Backward ownership / borrow inference for Xi IR
 *
 * KEY CONCEPT:
 *   Perceus-style ownership inference. Each reference-counted SSA value
 *   has a unique owner; all other uses are borrows. The pass walks the
 *   IR in reverse (backward liveness) to compute, for every RC value:
 *     - which use site is the last (consuming) use,
 *     - which uses are borrows (no refcount change),
 *     - where a drop must be inserted (last use, branch balance, exit).
 *
 *   This is a PURE ANALYSIS pass: it annotates values via a side table
 *   (XiOwnInfo) keyed by SSA value id. It does NOT mutate the IR and does
 *   NOT insert dup/drop. The later xi_arc rewrite consumes these
 *   annotations to emit XI_RETAIN / XI_RELEASE / XI_MOVE.
 *
 * REFERENCES (see docs/design/705_memory_model_refactor_plan.md §6):
 *   - Roc  crates/compiler/mono/src/inc_dec.rs (backward + owned/borrow env)
 *   - Koka src/Backend/C/Parc.hs (reverse traversal, owned/borrow multiset)
 *   - Nim  compiler/dfa.nim (CFG last-use reachability)
 *
 * USAGE:
 *   XiOwnResult own;
 *   xi_own_analyze(f, &own);
 *   // ... query own.values[vid] ...
 *   xi_own_free(&own);
 */

#ifndef XI_OWN_H
#define XI_OWN_H

#include "xi.h"
#include "../base/xdefs.h"
#include <stdbool.h>

/* ========== Ownership State ========== */

/* Ownership of an RC value at a given point. Mirrors Roc's Ownership enum. */
typedef enum {
    XI_OWN_NONE = 0, /* value is not reference-counted (scalar / unit / never) */
    XI_OWN_OWNED,    /* this value holds an owning reference (needs drop unless moved) */
    XI_OWN_BORROWED, /* borrowed reference (no refcount change) */
} XiOwnership;

/* Per-value ownership annotation, indexed by XiValue.id.
 *
 * `last_use_*` identifies the single consuming use site after which the
 * value's owning reference is dead (so a drop or move applies there).
 * When `is_dead` is set the value is never used and must be dropped right
 * after its definition (garbage-free). */
typedef struct XiOwnInfo {
    uint8_t ownership;     /* XiOwnership */
    bool rc_managed;       /* type is heap/RC-managed (string, array, instance, ...) */
    bool is_dead;          /* defined but never used → drop at definition */
    bool consumed;         /* has a consuming (move/last) use somewhere */
    bool needs_drop_flag;  /* conditional consume: cannot statically balance → runtime flag */
    uint32_t last_use_blk; /* block id of the last (consuming) use (UINT32_MAX = none) */
    uint32_t last_use_val; /* value id of the last user (UINT32_MAX = control/none) */
} XiOwnInfo;

/* ========== Per-Function Borrow Signature ========== */

/* Maximum parameters tracked precisely; beyond this, params are owned. */
#define XI_OWN_MAX_PARAMS 32

/* Borrow signature: for each parameter, whether the function only borrows
 * it (no consume) or takes ownership (stores/returns/forwards it).
 * Computed to a fixpoint across (mutually) recursive functions.
 * Mirrors Roc crate::borrow::infer_borrow_signatures. */
typedef struct XiBorrowSig {
    uint8_t param_own[XI_OWN_MAX_PARAMS]; /* XiOwnership per param (OWNED/BORROWED) */
    uint8_t nparams;                      /* number of valid entries */
    bool valid;                           /* computed successfully */
} XiBorrowSig;

/* ========== Analysis Result ========== */

typedef struct XiOwnResult {
    XiOwnInfo *values; /* indexed by value id, size = max_id */
    uint32_t max_id;   /* size of values[] */
    XiBorrowSig sig;   /* borrow signature of the analyzed function */
    uint32_t n_owned;  /* stats: number of owned values */
    uint32_t n_borrow; /* stats: number of borrowed values */
    uint32_t n_drop;   /* stats: number of drop points identified */
} XiOwnResult;

/* ========== API ========== */

/* Run backward ownership analysis on f (single function, not children).
 * Populates `out` (caller-owned). Pure analysis: f is not mutated.
 * Returns true on success, false on allocation failure. */
XR_FUNC bool xi_own_analyze(XiFunc *f, XiOwnResult *out);

/* Release memory held by an XiOwnResult. */
XR_FUNC void xi_own_free(XiOwnResult *out);

/* Whether a type is reference-counted (needs dup/drop).
 * Scalars (int/float/bool/null/unit/never) are not. */
XR_FUNC bool xi_own_type_is_rc(const struct XrType *type);

/* Dump ownership annotations to stderr for debugging (see --expandArc in Nim).
 * Prints, per value: id, op, ownership, rc_managed, drop point. */
XR_FUNC void xi_own_dump(const XiFunc *f, const XiOwnResult *out);

#endif  // XI_OWN_H
