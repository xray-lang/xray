/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcgen_bridge.h - AOT codegen forward declarations (void* signatures)
 *
 * KEY CONCEPT:
 *   AOT codegen only needs function addresses for pointer comparison
 *   and C code emission. It does not call these functions directly,
 *   so void* signatures are intentional to avoid pulling in
 *   coroutine/runtime headers.
 */

#ifndef XCGEN_BRIDGE_H
#define XCGEN_BRIDGE_H

#include <stdint.h>
#include "../base/xdefs.h"

struct XrJson;
struct XrShape;

/* ========== JIT Runtime Helpers (void* signature for AOT) ========== */

XR_FUNC int64_t xr_jit_index_get(void *, int64_t);
XR_FUNC int64_t xr_jit_index_set(void *, int64_t);
XR_FUNC int64_t xr_jit_getprop(void *, int64_t);
XR_FUNC int64_t xr_jit_throw(void *, int64_t);
XR_FUNC int64_t xr_jit_tarray_get(void *, int64_t);
XR_FUNC int64_t xr_jit_tarray_set(void *, int64_t);

/* ========== Json Creation (void* coro for AOT) ========== */

XR_FUNC struct XrJson *xr_json_new_with_shape(void *, struct XrShape *);

#endif
