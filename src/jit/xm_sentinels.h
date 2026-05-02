/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_sentinels.h - AOT sentinel function declarations
 *
 * KEY CONCEPT:
 *   Sentinel functions are used as fn_ptr markers in Xm.
 *   AOT codegen recognizes these addresses and emits specialized C code
 *   instead of calling the sentinel at runtime.
 */

#ifndef XM_SENTINELS_H
#define XM_SENTINELS_H

#include <stdint.h>
#include "../base/xdefs.h"

struct XrCoroutine;

// Builtin method invocation sentinel
XR_FUNC int64_t xrt_invoke_method_sentinel(struct XrCoroutine *c, int64_t x);

// StringBuilder operation sentinels
XR_FUNC int64_t xrt_strbuf_new_sentinel(struct XrCoroutine *c, int64_t x);
XR_FUNC int64_t xrt_strbuf_append_sentinel(struct XrCoroutine *c, int64_t x);
XR_FUNC int64_t xrt_strbuf_finish_sentinel(struct XrCoroutine *c, int64_t x);

#endif
