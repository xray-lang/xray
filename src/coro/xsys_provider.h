/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsys_provider.h - Native host and scheduler provider for sys.xr
 */

#ifndef XR_CORO_XSYS_PROVIDER_H
#define XR_CORO_XSYS_PROVIDER_H

#include "../base/xdefs.h"
#include "../runtime/value/xvalue.h"
#include "../../include/xray_yieldable_abi.h"

struct XrVMRuntime;

XR_FUNC XrValue xr_sys_provider_cpu_count(struct XrVMRuntime *isolate, XrValue *args, int argc);
XR_FUNC XrValue xr_sys_provider_thread_yield(struct XrVMRuntime *isolate, XrValue *args, int argc);
XR_FUNC XrValue xr_sys_provider_pin_to_cpu(struct XrVMRuntime *isolate, XrValue *args, int argc);
XR_FUNC XrValue xr_sys_provider_thread_local_id(struct XrVMRuntime *isolate, XrValue *args,
                                                int argc);
XR_FUNC XrValue xr_sys_provider_thread_local_alive(struct XrVMRuntime *isolate, XrValue *args,
                                                   int argc);
XR_FUNC XrValue xr_sys_provider_on_signal(struct XrVMRuntime *isolate, XrValue *args, int argc);
XR_FUNC XrValue xr_sys_provider_dylib_open(struct XrVMRuntime *isolate, XrValue *args, int argc);
XR_FUNC XrValue xr_sys_provider_dylib_symbol(struct XrVMRuntime *isolate, XrValue *args, int argc);
XR_FUNC XrValue xr_sys_provider_dylib_close(struct XrVMRuntime *isolate, XrValue *args, int argc);
XR_FUNC XrValue xr_sys_provider_dylib_last_error(struct XrVMRuntime *isolate, XrValue *args,
                                                 int argc);
XR_FUNC XrValue xr_sys_provider_process_spawn(struct XrVMRuntime *isolate, XrValue *args, int argc);
XR_FUNC XrValue xr_sys_provider_process_try_wait(struct XrVMRuntime *isolate, XrValue *args,
                                                 int argc);
XR_FUNC XrValue xr_sys_provider_process_kill(struct XrVMRuntime *isolate, XrValue *args, int argc);
XR_FUNC XrValue xr_sys_provider_pipe_open(struct XrVMRuntime *isolate, XrValue *args, int argc);
XR_FUNC XrCFuncResult xr_sys_provider_pipe_read(struct XrVMRuntime *isolate, XrValue *args,
                                                int argc, XrValue *result);
XR_FUNC XrCFuncResult xr_sys_provider_pipe_write(struct XrVMRuntime *isolate, XrValue *args,
                                                 int argc, XrValue *result);
XR_FUNC XrValue xr_sys_provider_pipe_close(struct XrVMRuntime *isolate, XrValue *args, int argc);

#endif  // XR_CORO_XSYS_PROVIDER_H
