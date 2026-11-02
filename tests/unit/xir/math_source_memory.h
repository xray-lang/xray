/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * math_source_memory.h - One physical Source allocation refusal and request recovery
 *
 * KEY CONCEPT:
 *   A test-local physical allocator observes the unchanged compiler owner.
 */
#ifndef MATH_SOURCE_MEMORY_H
#define MATH_SOURCE_MEMORY_H
#include "base/xmalloc.h"
static bool math_fail_next;
static unsigned math_allocation_failures;
static void *math_physical_malloc(size_t size) {
    if(math_fail_next){math_fail_next=false;++math_allocation_failures;return NULL;}
    return xr_malloc(size);
}
#undef xr_malloc
#define xr_malloc(size) math_physical_malloc(size)
#include "base/xcompile_resources.c"
#undef xr_malloc
#endif // MATH_SOURCE_MEMORY_H
