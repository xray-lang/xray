/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_native_observer_internal.h - Admitted native throws before unwind
 */
#ifndef XXIR_CALL_NATIVE_OBSERVER_INTERNAL_H
#define XXIR_CALL_NATIVE_OBSERVER_INTERNAL_H
#include "xxir_call_internal.h"

typedef struct XirCallNativeFrame {
    XrXirExecutorBinding binding;
    const void *instance;
    uint64_t frame,parent;
    uint32_t entry,depth;
    XrXirCallPhase phase;
    bool scope_exit;
} XirCallNativeFrame;

typedef struct XirCallNativeThrowEvent {
    XirCallNativeFrame frame;
    const XrXirValue *value;
} XirCallNativeThrowEvent;

/* The Program lease owns this observer through Instance disposal. Notification
 * borrows only the already admitted throw value for this synchronous call.
 * Only admitted normal-phase native THROW and LEAVE_ERROR boundaries are
 * offered. scope_exit describes the pending lexical exit, not access authority.
 * A propagated return and the internal Instance dispatcher are not
 * source-origin classifications.
 * No callback view or native state is lent; all executor reentry is excluded.
 * READY accepts the pause decision. OOM/LIMIT preserve execution-failure rules;
 * every other status is an observer protocol failure. */
typedef struct XirCallNativeObserver {
    void *owner;
    XrXirCallStatus (*throwing)(void *,const XirCallNativeThrowEvent *,bool *);
} XirCallNativeObserver;

typedef struct XirCallNativeThrow {
    XirCallNativeFrame frame;
    XrXirValue value;
} XirCallNativeThrow;

/* Install once from an actual callback on the exact executor Instance. The
 * identity-only observer remains separate. This enables no VM frame access. */
XR_FUNC XrXirCallStatus xir_call_native_observer_bind(const XrXirCallView *,const XirCallNativeObserver *);
/* Between driver quanta only, for the exact held frame and executor binding.
 * Copy returns an owned value, never a frame/state/view borrow. Failure keeps
 * output unchanged. Continue consumes the hold once; the next transition
 * starts the original exit with its lexical form intact. The producing action
 * is not repeated; a lexical exit may then execute its ordinary continuation. */
XR_FUNC XrXirCallStatus xir_call_native_throw_copy(XrXirCall *,const XrXirExecutorBinding *,uint64_t,XirCallNativeThrow *);
XR_FUNC XrXirCallStatus xir_call_native_throw_continue(XrXirCall *,const XrXirExecutorBinding *,uint64_t);
#endif // XXIR_CALL_NATIVE_OBSERVER_INTERNAL_H
