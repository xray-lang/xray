/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_arc_verify.h - Independent RC / ownership contract verifier (task 219)
 *
 * Machine-checks the written RC contract (C1-C5, see docs/rules/architecture.md)
 * on ARC-inserted Xi IR. This is an N-VERSION checker: it shares NO closure or
 * alias code with xi_arc.c / xi_own.c and re-derives ownership facts from first
 * principles, depending only on xi_analysis (dominators / CFG) and the single
 * operand-ownership table generated from xisa/xi/ops.def. A checker that reused
 * the checked pass's logic could only reproduce that pass's bugs.
 *
 * Contracts:
 *   C1  no use after release        (use-after-free / early release)
 *   C2  path balance                (no double release)
 *   C3  borrow-closure completeness (owner live at every borrow-view use)
 *   C4  dominance boundary          (release dominated by owner; join归属)
 *   C5  metadata / SSA completeness (declared ownership; no stale users)
 *
 * The core (xi_arc_verify / xi_arc_verify_tree) is a PURE check: it never
 * mutates the IR (beyond refreshing the analysis caches) and never aborts.
 * Unit tests feed it hand-built violating IR and assert on report.contract.
 * The pipeline uses xi_arc_verify_or_ice, which turns a violation into a hard
 * ICE with a counterexample path and an IR dump.
 */

#ifndef XI_ARC_VERIFY_H
#define XI_ARC_VERIFY_H

#include "xi.h"

/* Which RC contract a verification run found violated. */
typedef enum XiArcContract {
    XI_ARC_CONTRACT_NONE = 0,
    XI_ARC_C1_USE_AFTER_RELEASE, /* C1: value used after its owner was released */
    XI_ARC_C2_DOUBLE_RELEASE,    /* C2: value released twice on a path */
    XI_ARC_C2_BALANCE,           /* C2: net RC imbalance at an exit (reserved) */
    XI_ARC_C3_BORROW_ESCAPE,     /* C3: borrow view used past its owner's lifetime */
    XI_ARC_C4_DOMINANCE,         /* C4: release not dominated by owner def */
    XI_ARC_C5_STALE_USE,         /* C5: operand refers to a value with no live def */
    XI_ARC_C5_METADATA,          /* C5: op ownership metadata inconsistency */
} XiArcContract;

/* Structured verification result. On success, ok = true and contract = NONE.
 * On the first violation found, ok = false and the remaining fields describe it. */
typedef struct XiArcVerifyReport {
    bool ok;
    XiArcContract contract;
    const XiFunc *func;    /* failing function (for IR dump) */
    const char *func_name; /* failing function name */
    uint32_t value_id;     /* offending RC value id (UINT32_MAX = n/a) */
    uint32_t release_blk;  /* block id of the release, if applicable (UINT32_MAX = n/a) */
    uint32_t use_blk;      /* block id of the offending use (UINT32_MAX = n/a) */
    char message[256];     /* human-readable diagnostic */
    char path[192];        /* shortest counterexample block path, e.g. "b0->b2->b5" */
} XiArcVerifyReport;

/* Human-readable contract label (e.g. "C1 (use-after-release)"). */
XR_FUNC const char *xi_arc_contract_name(XiArcContract c);

/* Verify a single function (not its children). Returns true if all contracts
 * hold. On the first violation, fills *report and returns false. Pure check:
 * never aborts. `report` may be NULL if the caller only wants the bool. */
XR_FUNC bool xi_arc_verify(XiFunc *f, XiArcVerifyReport *report);

/* Verify f and all nested children. Returns true if every function passes. */
XR_FUNC bool xi_arc_verify_tree(XiFunc *f, XiArcVerifyReport *report);

/* Pipeline entry point: verify the whole tree and, on violation, print the
 * diagnostic + counterexample path, dump the offending function's IR, and
 * abort() as an internal compiler error. `stage_label` names the pass/point
 * after which verification ran (e.g. "xi_arc_insert"). */
XR_FUNC void xi_arc_verify_or_ice(XiFunc *f, const char *stage_label);

#endif /* XI_ARC_VERIFY_H */
