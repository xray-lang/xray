/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_compile_context.h - Shared compiler ownership and structural limits
 *
 * KEY CONCEPT:
 *   Context copies preserve one ledger; only structural limits are values.
 */
#ifndef XXIR_COMPILE_CONTEXT_H
#define XXIR_COMPILE_CONTEXT_H
#include "../base/xcompile_resources.h"

typedef struct XrXirCompileLimits {
    uint32_t functions, parameters, blocks, instructions;
    uint64_t frame_bytes;
} XrXirCompileLimits;

/* Calls borrow a valid context. Every owned allocation pins its ledger until
 * physical release, independently of the lifetime of this context value. */
typedef struct XrXirCompileContext {
    XrCompileResources *resources;
    XrXirCompileLimits limits;
} XrXirCompileContext;

XR_FUNC XrXirCompileLimits xr_xir_compile_default_limits(void);
#endif // XXIR_COMPILE_CONTEXT_H
