/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_root_callback_internal.h - Private owned Root callback activation
 */
#ifndef XXIR_ROOT_CALLBACK_INTERNAL_H
#define XXIR_ROOT_CALLBACK_INTERNAL_H
#include "xxir_program.h"

/* Prepares and publishes one sibling activation only from an actual same-Root
 * owned FunctionBinding: zero explicit parameters, Unit result, Never error.
 * The binding must belong to the active sealed generation; older unreceived
 * generations remain closed. Captures and the code lease stay owned until
 * that independent Call retires.
 * The live Parent Call, binding ticket, epoch and wait records are untouched.
 * A busy, initializing, borrowed, unknown or NoSuspend frontier defers with
 * BAD_STATE/BUSY and performs no callback. This grants no Source registration
 * or native call permission; those require their own sealed site admission. */
XR_FUNC XrXirCallStatus xir_instance_callback_begin(XrXirInstance *instance,
    const XrXirValue *function);
/* An observation of the actual executor-owned sibling, not an execution or
 * handler-registration capability. Invalid/foreign owners return false. */
XR_FUNC bool xir_instance_callback_active(const XrXirInstance *instance);
/* The host supplies its own monotonic sample. Guest time providers do not
 * establish or alter relative host wait deadlines. Each actual activation
 * keeps its first sample for the exact Call wake, including after reselection. */
typedef struct XirHostWaitDeadline { uint64_t start_ns, duration_ns; bool active; } XirHostWaitDeadline;
XR_FUNC XrXirCallStatus xir_instance_host_wait_deadline(XrXirInstance *instance,
    uint64_t epoch,uint64_t wake,uint64_t now_ns,XirHostWaitDeadline *output);

/* Executor-side identity checks use the actual pending Instance owner; raw
 * Calls, copied carriers and arbitrary internal pointers grant no role. */
struct XrXirTaskExecutor;
XR_FUNC bool xir_instance_callback_owns(const XrXirInstance *instance,
    const struct XrXirTaskExecutor *executor,const XrXirCall *call);
XR_FUNC XrXirCallStatus xir_instance_callback_retire(XrXirInstance *instance,XrXirCall *call);
#endif // XXIR_ROOT_CALLBACK_INTERNAL_H
