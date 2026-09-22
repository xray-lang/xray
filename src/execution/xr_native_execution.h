/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_native_execution.h - Instance-bound native frame lifecycle
 */

#ifndef XR_NATIVE_EXECUTION_H
#define XR_NATIVE_EXECUTION_H

#include "xr_execution.h"
#include "xr_native_descriptor.h"

typedef struct XrBackendExecution XrBackendExecution;

XR_FUNC bool xr_backend_execution_create(XrInstance *instance,
                                         const XrBackendNativeDescriptor *descriptor,
                                         XrBackendExecution **execution_out);
XR_FUNC XrBackendExecutionOutcome xr_backend_execution_step(XrBackendExecution *execution);
XR_FUNC XrBackendExecutionOutcome xr_backend_execution_cancel(XrBackendExecution *execution);
XR_FUNC void xr_backend_execution_free(XrBackendExecution *execution);

#endif  // XR_NATIVE_EXECUTION_H
