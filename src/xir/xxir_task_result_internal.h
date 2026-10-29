/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task_result_internal.h - Owned task observations on authenticated frames
 */
#ifndef XXIR_TASK_RESULT_INTERNAL_H
#define XXIR_TASK_RESULT_INTERNAL_H
#include "xxir_call.h"
typedef struct XrXirTaskExecutor XrXirTaskExecutor;
/* A successful observation owns a terminal copy, or reports pending without
 * dispatching any activation. Rejections preserve both outputs. */
XR_FUNC XrXirCallStatus xir_task_executor_observe(XrXirTaskExecutor *executor,
    XrXirCallView *view,const XrXirValue *task,bool *terminal,XrXirCallResult *output);
XR_FUNC XrXirCallStatus xir_task_executor_result_member(XrXirTaskExecutor *executor,
    XrXirCallView *view,const XrXirValue *task);
/* Only the four language outcomes produce a value. Engine failures and fatal
 * cleanup remain independent; failure leaves the caller's Unit output intact. */
XR_FUNC XrXirCallStatus xir_task_result_wrap(const XrXirCallResult *outcome,
    XrXirType type,XrXirValueAdmission *admission,XrXirValue *output);
/* The current Instance authenticates the real instruction before staging this
 * private wait mode. The next action must name that exact owned Task. */
XR_FUNC XrXirCallStatus xir_call_task_result_stage(XrXirCallView *view,
    const XrXirValue *task,XrXirType result);
XR_FUNC XrXirCallStatus xr_xir_instance_task_poll_at(XrXirCallView *view,
    uint32_t instruction,const XrXirValue *task,XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_instance_task_await_result_at(XrXirCallView *view,
    uint32_t instruction,const XrXirValue *task);
#endif
