/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_host_execution.h - Owned host consumption of a typed program instance
 *
 * KEY CONCEPT:
 *   VM and native programs use the same instance and typed outcome owner.
 */
#ifndef XR_XIR_HOST_EXECUTION_H
#define XR_XIR_HOST_EXECUTION_H
#include "../xir/xxir_program.h"
typedef struct XrXirHostExecutionRequest {
    XrXirProgram *program;
    const XrXirInstanceConfig *config;
    uint32_t entry;
    const XrXirValue *arguments;
    uint32_t argument_count;
} XrXirHostExecutionRequest;
typedef struct XrXirHostCall XrXirHostCall;
/* Calls are serialized by the host. Reentry from execution/cleanup returns BUSY.
 * Begin owns a new instance and retains arguments/code; provider contexts remain
 * host-owned until drop. Output must be empty and is unchanged on failure. */
XR_FUNC XrXirCallStatus xr_xir_host_call_begin(const XrXirHostExecutionRequest *request,
    XrXirHostCall **output);
/* One poll, without automatically resuming suspension. The outcome borrows until
 * the next state-changing operation or drop; only take publishes an owned result. */
XR_FUNC XrXirInstanceResult xr_xir_host_call_step(XrXirHostCall *call);
XR_FUNC XrXirCallStatus xr_xir_host_call_resume(XrXirHostCall *call, uint64_t epoch, uint64_t wake);
/* Cancels a ready/suspended call and completes its existing cleanup protocol.
 * Returns the terminal status, including any cleanup failure. */
XR_FUNC XrXirCallStatus xr_xir_host_call_cancel(XrXirHostCall *call);
/* Takes a terminal result once. Copy failure preserves both owner and output. */
XR_FUNC XrXirCallStatus xr_xir_host_call_take(XrXirHostCall *call, XrXirCallResult *output);
/* Consumes the owner except on BUSY, reporting cleanup failure. NULL is a no-op. */
XR_FUNC XrXirCallStatus xr_xir_host_call_drop(XrXirHostCall *call);
/* Admission or cleanup failure preserves output. A published result owns values. */
XR_FUNC XrXirCallStatus xr_xir_host_execute(const XrXirHostExecutionRequest *request,
    XrXirCallResult *output);
#endif // XR_XIR_HOST_EXECUTION_H
