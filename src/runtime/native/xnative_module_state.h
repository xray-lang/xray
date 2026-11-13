/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_module_state.h - Shared typed state runtime implementation boundary
 *
 * KEY CONCEPT:
 *   VM and native execution consume the same owned slot and type operations.
 */
#ifndef XNATIVE_MODULE_STATE_H
#define XNATIVE_MODULE_STATE_H
#include "xray_native_state.h"
#include "xir/xxir_value.h"
XR_FUNC bool xrt_module_callable_signature(const XrtModuleLayout *, uint32_t type,
    const XrXirTypeArena **, XrXirType *);
XR_FUNC XrXirTypeArena *xrt_module_callable_catalog(const XrtModuleLayout *);
XR_FUNC XrXirTypeArena *xrt_module_state_callables(const XrtModuleState *, const XrtCallableImage *);
#endif // XNATIVE_MODULE_STATE_H
