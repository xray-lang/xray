/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_observer_internal.h - Bounded observation of authenticated Call frames
 *
 * KEY CONCEPT:
 *   Events identify execution without lending frame state or entry authority.
 */
#ifndef XXIR_CALL_OBSERVER_INTERNAL_H
#define XXIR_CALL_OBSERVER_INTERNAL_H
#include "xxir_call_internal.h"
/* Events borrow no frame state. Frame IDs are scoped by the exact executor binding. */
typedef enum XrXirCallEventKind {
    XR_XIR_CALL_FRAME_PRESENT, XR_XIR_CALL_FRAME_ENTER, XR_XIR_CALL_FRAME_CODE,
    XR_XIR_CALL_FRAME_LEAVE, XR_XIR_CALL_DISPATCH_BEGIN, XR_XIR_CALL_DISPATCH_END
} XrXirCallEventKind;
typedef enum XrXirCallCodeKind {
    XR_XIR_CALL_CODE_UNKNOWN, XR_XIR_CALL_CODE_NATIVE, XR_XIR_CALL_CODE_VM
} XrXirCallCodeKind;
typedef struct XrXirCallEvent {
    XrXirExecutorBinding binding;
    const void *instance;
    uint64_t frame, parent;
    uint32_t entry, depth;
    XrXirCallEventKind kind;
    XrXirCallCodeKind code;
    XrXirCallPhase phase;
    XrXirCallStatus status;
} XrXirCallEvent;
typedef void (*XrXirCallObserveEntry)(void *owner, const XrXirCallEvent *event);
struct XrXirCallObserver { void *owner; XrXirCallObserveEntry event; };
/* One Program-code-lease-owned observer is installed through an authenticated
 * active view. The shared executor budget observes all its activations. Existing
 * frames replay as PRESENT, newest first; parent IDs reconstruct the tree. The
 * receiver records within its own prepaid bound, never drives or borrows state.
 * Installation prepays replay work before publication. Notifications allocate
 * nothing, exclude Call/Instance reentry, and never replace failure/cleanup.
 * Frame depth is local to its activation; ENTER precedes the first body dispatch.
 * DISPATCH status is READY: the returned action has not yet been admitted.
 * UNKNOWN means no body has identified its backend yet. Real VM entry marks VM;
 * an unmarked C callback identifies NATIVE after its first returned action. */
XR_FUNC XrXirCallStatus xr_xir_call_observer_bind(const XrXirCallView *view,
    const XrXirCallObserver *observer);
/* Only the actual admitted VM callback invokes this before any VM operation. */
XR_FUNC XrXirCallStatus xr_xir_call_observer_vm(const XrXirCallView *view);
/* A synchronous fact about the exact active view, never a cached frame grant.
 * Copied/retired views and native or unmarked callbacks are not VM ingress. */
XR_FUNC bool xir_call_vm_view_current(const XrXirCallView *view);
/* Only at exact VM ingress. Resolve an entered VM frame on this activation's
 * actual live ancestor chain without lending a view, state or invocation.
 * A different metadata table/context is rejected before returning an ordinal. */
XR_FUNC XrXirCallStatus xir_call_vm_frame_entry(const XrXirCallView *,uint64_t,uint32_t *);
/* Metadata only at an authentic normal callback: resolve a live VM/native
 * ancestor identity in the current metadata coordinates without borrowing its
 * state or widening VM write grants. Cross-metadata ancestors are rejected. */
XR_FUNC XrXirCallStatus xir_call_debug_frame_entry(const XrXirCallView *,uint64_t,uint32_t *,XrXirCallCodeKind *);
/* The owning executor may visit only this exact already-entered VM frame.
 * This check lends no view and does not execute a callback. */
XR_FUNC XrXirCallStatus xir_call_vm_visit_ready(XrXirCall *,const XrXirExecutorBinding *,uint64_t);

#endif // XXIR_CALL_OBSERVER_INTERNAL_H
