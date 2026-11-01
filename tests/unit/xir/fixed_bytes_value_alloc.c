/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * fixed_bytes_value_alloc.c - Inject the real fixed payload allocation failure
 *
 * KEY CONCEPT:
 *   The production value implementation owns allocation and rollback unchanged.
 */
#include "base/xmalloc.h"
#include <stdbool.h>
static bool fixed_bytes_fail_allocation;
static void *fixed_bytes_malloc(size_t bytes) {
    if(fixed_bytes_fail_allocation){fixed_bytes_fail_allocation=false;return NULL;}
    return xr_malloc(bytes);
}
#undef xr_malloc
#define xr_malloc(bytes) fixed_bytes_malloc(bytes)
#include "xir/xxir_value.c"
#undef xr_malloc
XR_FUNC void fixed_bytes_fail_next(void) {fixed_bytes_fail_allocation=true;}
XR_FUNC bool fixed_bytes_failure_observed(void) {return !fixed_bytes_fail_allocation;}
