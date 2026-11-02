/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * path_source_memory.h - Physical allocator refusal on the unchanged Source owner
 *
 * KEY CONCEPT:
 *   One real allocation fails; rollback and a fresh complete request remain observable.
 */
#ifndef PATH_SOURCE_MEMORY_H
#define PATH_SOURCE_MEMORY_H
static bool path_physical_fail;
static unsigned path_physical_failures;
static void *path_physical_malloc(size_t size) {
    if(path_physical_fail){path_physical_fail=false;++path_physical_failures;return NULL;}
    return xr_malloc(size);
}
#pragma push_macro("xr_malloc")
#undef xr_malloc
#define xr_malloc(size) path_physical_malloc(size)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_malloc")
#endif // PATH_SOURCE_MEMORY_H
