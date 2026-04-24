/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjit_hooks.c - JIT hooks global storage (Phase 4: CORO-08)
 */

#include "xjit_hooks.h"

/* NULL until jit/ module calls xr_jit_hooks_register(). */
XrJitHooks *xr_jit_hooks = NULL;
