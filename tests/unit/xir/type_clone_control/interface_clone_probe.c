/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * interface_clone_probe.c - The canonical interface copier participates in the same borrowed request.
 */
#include "probe_internal.h"
#define xir_compile_alloc(c,b,s) tc_probe_alloc(c,b,s,__func__)
#define xir_compile_calloc(c,n,b,s) tc_probe_calloc(c,n,b,s,__func__)
#define xir_type_copy_zero tc_zero
#define xir_type_copy_bytes tc_bytes
#define xir_lower_copy_span tc_span
#include "xir/xxir_interface.c"
