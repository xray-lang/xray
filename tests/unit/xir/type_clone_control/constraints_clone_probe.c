/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * constraints_clone_probe.c - The canonical nested constraint/application copier is observed directly.
 */
#include "probe_internal.h"
#define xir_compile_alloc(c,b,s) tc_probe_alloc(c,b,s,__func__)
#define xir_compile_calloc(c,n,b,s) tc_probe_calloc(c,n,b,s,__func__)
#define xir_type_copy_zero tc_zero
#define xir_type_copy_bytes tc_bytes
#define xir_lower_copy_span tc_span
#include "xir/xxir_constraints.c"
