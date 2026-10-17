/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_debug_calls_internal.h - Preallocated authenticated frame records
 */
#ifndef XXIR_VM_DEBUG_CALLS_INTERNAL_H
#define XXIR_VM_DEBUG_CALLS_INTERNAL_H
#include "xxir_call_observer_internal.h"
#include "xxir_value.h"

typedef struct XirDebugCallFrame {
    XrXirCallEvent event;
    uint64_t id, thread;
    bool active;
    uint32_t native_instruction,native_callee;
    bool native_waiting,native_invoked;
} XirDebugCallFrame;
typedef struct XirDebugCalls {
    XrXirCallObserver observer;
    XrXirDomain *domain;
    const void *target;
    const XrXirCall *installing;
    XirDebugCallFrame *frames;
    uint32_t capacity, count, current, functions;
    uint64_t next_id, next_thread, debt;
    XrXirValueStatus failure;
    bool installed, disabled;
} XirDebugCalls;
XR_FUNC XrXirValueStatus xir_debug_calls_new(XirDebugCalls *,XrXirDomain *,uint32_t);
XR_FUNC void xir_debug_calls_free(XirDebugCalls *);
XR_FUNC XrXirValueStatus xir_debug_calls_bind(XirDebugCalls *,const XrXirCallView *);
XR_FUNC XrXirValueStatus xir_debug_calls_pay(XirDebugCalls *);
XR_FUNC const XirDebugCallFrame *xir_debug_calls_current(const XirDebugCalls *);
XR_FUNC const XirDebugCallFrame *xir_debug_calls_parent(const XirDebugCalls *,const XirDebugCallFrame *);
XR_FUNC const XirDebugCallFrame *xir_debug_calls_id(const XirDebugCalls *,uint64_t);
XR_FUNC XrXirValueStatus xir_debug_calls_native_pending(XirDebugCalls *,const XrXirCallView *,uint32_t,uint32_t,bool);
#endif // XXIR_VM_DEBUG_CALLS_INTERNAL_H
