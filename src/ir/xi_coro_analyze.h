/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_coro_analyze.h - Shared coroutine suspension analysis for Xi IR
 *
 * KEY CONCEPT:
 *   Produces a backend-neutral XiCoroPlan describing a function's coroutine
 *   shape: the suspend points (with dense, stable state ids), the set of
 *   values live across a suspend (the logical frame slots), and closure /
 *   reduction metadata.  Both the AOT compiler and the VM consume the same
 *   plan, so the stackless state machine is identical by construction rather
 *   than only by differential testing.
 *
 *   The logical frame (which values are members) is identical across
 *   backends; each backend chooses its own physical slot representation and
 *   layout, which is unobservable.  The two genuinely context-dependent
 *   queries -- interprocedural callee resolution and stdlib module-import
 *   recognition -- are routed through XiCoroResolver so the analysis never
 *   depends on AOT or VM bundle types (keeps the src/ir DAG intact).
 */

#ifndef XI_CORO_ANALYZE_H
#define XI_CORO_ANALYZE_H

#include "xi.h"
#include "xi_analysis.h"
#include "xi_value_query.h"

/* Kind of a coroutine suspension site (diagnostic + lowering hint). */
typedef enum {
    XI_CORO_SUSP_YIELD,
    XI_CORO_SUSP_GO,
    XI_CORO_SUSP_AWAIT,
    XI_CORO_SUSP_CHAN_SEND,
    XI_CORO_SUSP_CHAN_RECV,
    XI_CORO_SUSP_SELECT,
    XI_CORO_SUSP_SCOPE_EXIT,
    XI_CORO_SUSP_SLEEP,
    XI_CORO_SUSP_CALL,
} XiCoroSuspendKind;

/* One suspension site and the values that stay live across it. */
typedef struct XiCoroSuspendPoint {
    uint32_t state_id; /* dense, 1-based, stable across backends */
    XiValue *op;       /* the suspending instruction */
    XiCoroSuspendKind kind;
    XiValue **live; /* live-across-suspend set (arena array) */
    uint32_t nlive;
} XiCoroSuspendPoint;

/* Where a logical slot originates: parameters and phis are always part of the
 * frame, whereas a block value additionally has to pass the backend's physical
 * storage test before it occupies a slot. */
typedef enum {
    XI_CORO_SLOT_PARAM,
    XI_CORO_SLOT_PHI,
    XI_CORO_SLOT_VALUE,
} XiCoroSlotKind;

/* One logical coroutine frame slot.  Membership is backend-identical;
 * logical_rep is an advisory representation each backend may refine.
 *
 * is_root / needs_release / needs_boundary_clone are pure value attributes,
 * while frame_root / frame_release are the *logical frame roles* (attribute
 * intersected with cross-suspend liveness / root reachability).  A backend
 * obtains its physical root/release set by further intersecting frame_root /
 * frame_release with its own storage test (e.g. AOT's has_storage). */
typedef struct XiCoroSlot {
    XiValue *value;
    struct XrType *type;
    uint8_t logical_rep; /* XrRep value (xtype.h), via xr_type_rep() */
    uint8_t kind;        /* XiCoroSlotKind */
    bool is_root;
    bool needs_release;
    bool needs_runtime_slot;
    bool needs_boundary_clone;
    bool live_across;   /* live across at least one suspend point */
    bool frame_root;    /* logical frame GC root */
    bool frame_release; /* logical frame ARC release on drop */
} XiCoroSlot;

/* Backend-neutral coroutine shape, attached to XiFunc by xi_coro_analyze. */
typedef struct XiCoroPlan {
    bool is_coroutine;
    uint32_t nstates; /* number of suspend states == entries in points */
    XiCoroSuspendPoint *points;
    XiCoroSlot *slots;
    uint32_t nslots;
    uint32_t root_count;
    uint32_t release_count;
    bool needs_cl;     /* frame carries a closure environment pointer */
    uint8_t ctx_depth; /* interprocedural resolution depth bound */
} XiCoroPlan;

/* Backend seam: keeps the analysis free of AOT/VM bundle types by routing
 * the two context-dependent queries through callbacks.
 *   - resolve_callee maps a call's callee value to its target XiFunc.
 *   - value_is_module_import decides whether 'v' (as used in 'f') refers to
 *     an import of the named stdlib module (e.g. "time" for time.sleep).
 * Either callback may be NULL; the analysis then answers only from the
 * intraprocedural / direct-import information available in the IR. */
typedef struct XiCoroResolver {
    const XiFunc *(*resolve_callee)(void *ud, const XiFunc *current, const XiValue *callee);
    bool (*value_is_module_import)(void *ud, const XiFunc *f, const XiValue *v, const char *module);
    void *ud;
} XiCoroResolver;

/* ========== Concurrency value-shape recognizers ========== */

/* True if op belongs to the coroutine op class (yield/await/chan/... ). */
XR_FUNC bool xi_op_is_coroutine(uint16_t op);

/* True if 'v' is a method call `recv.<method>(...)` on the matching builtin
 * type with exactly 'nargs' explicit arguments (nargs < 0 = any arity).
 * Channel calls additionally accept an unknown-typed receiver for the
 * send/recv shapes that pre-date precise typing. */
XR_FUNC bool xi_value_is_channel_method_call(const XiValue *v, const char *method, int nargs);
XR_FUNC bool xi_value_is_task_method_call(const XiValue *v, const char *method, int nargs);
XR_FUNC bool xi_value_is_work_queue_method_call(const XiValue *v, const char *method, int nargs);

/* True if 'v' is a Task/WorkQueue method call that may block (and therefore
 * needs a runtime result slot across the suspend). */
XR_FUNC bool xi_value_is_blocking_task_method_call(const XiValue *v);
XR_FUNC bool xi_value_is_blocking_work_queue_method_call(const XiValue *v);

/* True if 'v' is a coroutine suspension site.  'resolver' supplies the two
 * context-dependent queries (stdlib module-import for time.sleep, and
 * interprocedural callee resolution for a direct call into a suspendable
 * function); pass a resolver wired to the current backend's bundle. */
XR_FUNC bool xi_coro_is_suspend_point(const XiFunc *f, const XiValue *v,
                                      const XiCoroResolver *resolver);

/* ========== Frame-membership analysis (logical, backend-neutral) ========== */

/* Typed recv/await slot reuse: a channel recv (or await result) typed as a
 * tagged value whose sole consumer is one scalar XI_UNBOX reuses the recv/await
 * frame slot directly.  The *_user queries return that consumer (or NULL); the
 * unbox_from_* queries answer it from the unbox's perspective.  Used by AOT
 * physical storage and slot emission. */
XR_FUNC const XiValue *xi_coro_typed_recv_unbox_user(const XiFunc *f, const XiValue *recv);
XR_FUNC const XiValue *xi_coro_typed_await_unbox_user(const XiFunc *f, const XiValue *await_value);
XR_FUNC bool xi_coro_unbox_from_typed_recv(const XiFunc *f, const XiValue *v);
XR_FUNC bool xi_coro_unbox_from_typed_await(const XiFunc *f, const XiValue *v);

/* A channel recv's unique recv-status consumer (the paired `recv()/status`
 * shape); recv_status_user returns it, is_paired_recv_status tests it. */
XR_FUNC const XiValue *xi_coro_recv_status_user(const XiFunc *f, const XiValue *recv);
XR_FUNC bool xi_coro_is_paired_recv_status(const XiFunc *f, const XiValue *v);

/* True if 'v' is a blocking channel/task/work-queue method whose result is
 * delivered through a runtime-managed slot that must persist across suspend. */
XR_FUNC bool xi_coro_value_needs_runtime_slot(const XiValue *v);

/* True if 'target' is awaited as one element of an await-all/await-any group. */
XR_FUNC bool xi_coro_value_is_aggregate_await_tasks(const XiFunc *f, const XiValue *target);

/* True if 'target' (a value, phi result, or parameter) is live across at least
 * one suspend point of 'f'.  'live' is the function liveness; 'resolver'
 * supplies suspend-point classification (see xi_coro_is_suspend_point). */
XR_FUNC bool xi_coro_value_live_across_suspend(const XiFunc *f, const XiLiveness *live,
                                               const XiValue *target,
                                               const XiCoroResolver *resolver);

/* True if 'v' is a logical coroutine frame member: it survives a suspend because
 * it is live across one, needs a runtime result slot, is an await aggregate, is
 * a go handle, or reuses a typed recv/await slot.  This is the backend-neutral
 * membership; each backend intersects it with its own physical storage test. */
XR_FUNC bool xi_coro_value_is_logical_member(const XiFunc *f, const XiValue *v,
                                             const XiLiveness *live,
                                             const XiCoroResolver *resolver);

/* ========== Slot attributes (backend-neutral) ========== */

/* Unwrap COPY/MOVE/BOX/UNBOX to the value that determines a slot's ARC / clone
 * behavior; the _builtin_ variant further narrows to an underlying GET_BUILTIN. */
XR_FUNC const XiValue *xi_coro_release_origin(const XiValue *v);
XR_FUNC const XiValue *xi_coro_builtin_origin(const XiValue *v);

/* is_root: the slot holds a GC-traceable root (tagged value, or rc pointer). */
XR_FUNC bool xi_coro_value_rep_can_trace_root(const XiValue *v);

/* needs_release: the slot owns an ARC reference the frame must release on drop. */
XR_FUNC bool xi_coro_value_needs_arc_release(const XiValue *v);

/* needs_boundary_clone: the value must be cloned as it crosses the coroutine
 * boundary (mutable aggregates / StringBuilder / json). */
XR_FUNC bool xi_coro_type_needs_boundary_clone(const struct XrType *type);
XR_FUNC bool xi_coro_value_needs_boundary_clone(const XiValue *v);

/* True if the slot value carries a json type (selects the clone helper). */
XR_FUNC bool xi_coro_value_has_json_type(const XiValue *v);

/* Membership query against a materialized plan (built by xi_coro_analyze).
 * True if 'v' is one of the plan's logical frame slots. */
XR_FUNC bool xi_coro_plan_is_logical_member(const XiCoroPlan *plan, const XiValue *v);

/* The plan slot describing 'v', or NULL if 'v' is not a logical frame member.
 * Lets a backend read the shared slot attributes (live_across, frame_root,
 * frame_release, ...) instead of recomputing liveness. */
XR_FUNC const XiCoroSlot *xi_coro_plan_find_slot(const XiCoroPlan *plan, const XiValue *v);

/* ========== Plan + suspendability ========== */

/* Compute (or return the cached) coroutine plan for 'f'.  Idempotent: the
 * result is stored on f->coro_plan and reused on later calls.  Returns NULL
 * only for a NULL function or on allocation failure. */
XR_FUNC XiCoroPlan *xi_coro_analyze(XiFunc *f, const XiCoroResolver *resolver);

/* True if 'f' itself, or (when the resolver supplies callee resolution) any
 * function it transitively calls within the interprocedural depth bound,
 * contains a coroutine suspension point.  With a NULL resolve_callee the
 * answer is intraprocedural only. */
XR_FUNC bool xi_coro_func_is_suspendable(const XiFunc *f, const XiCoroResolver *resolver);

#endif  // XI_CORO_ANALYZE_H
