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
/* Admission or cleanup failure preserves output. A published result owns values. */
XR_FUNC XrXirCallStatus xr_xir_host_execute(const XrXirHostExecutionRequest *request,
    XrXirCallResult *output);
#endif // XR_XIR_HOST_EXECUTION_H
