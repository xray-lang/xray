/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_hosted_fragment_runtime.h - VM host side of the fragment ABI
 */

#ifndef XRAY_HOSTED_FRAGMENT_RUNTIME_H
#define XRAY_HOSTED_FRAGMENT_RUNTIME_H

#include "xray_export.h"
#include "xray_hosted_fragment_abi.h"

typedef struct XrVMRuntime XrVMRuntime;

XRAY_API const XrHostedFragmentHostOps xr_hosted_fragment_host_ops;
XRAY_API const void *xr_hosted_fragment_runtime_ops(void);
XRAY_API void *xr_hosted_fragment_current_coroutine(XrVMRuntime *isolate);
XRAY_API bool xr_hosted_fragment_context_init(XrVMRuntime *isolate, const char *module_name,
                                              XrHostedFragmentContext *context,
                                              XrHostedFragmentSignal *signal);
XRAY_API XrValue xr_hosted_fragment_int(int64_t value);
XRAY_API bool xr_hosted_fragment_as_int(XrValue value, int64_t *out);
XRAY_API XrValue xr_hosted_fragment_handle_signal(XrVMRuntime *isolate, const char *symbol,
                                                  const XrHostedFragmentSignal *signal);

#endif /* XRAY_HOSTED_FRAGMENT_RUNTIME_H */
