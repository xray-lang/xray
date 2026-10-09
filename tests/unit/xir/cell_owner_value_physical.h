/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cell_owner_value_physical.h - Observe actual value allocator blocks and bytes
 */
#ifndef CELL_OWNER_VALUE_PHYSICAL_H
#define CELL_OWNER_VALUE_PHYSICAL_H
#include "base/xmalloc.h"
typedef struct CellOwnerPhysicalBlock { void *pointer; size_t bytes; } CellOwnerPhysicalBlock;
static CellOwnerPhysicalBlock cell_owner_blocks[1024];
static size_t cell_owner_live, cell_owner_bytes;
static void cell_owner_record(void *pointer, size_t bytes) {
    if (!pointer) return;
    CHECK(cell_owner_live < 1024 && bytes <= SIZE_MAX-cell_owner_bytes);
    cell_owner_blocks[cell_owner_live++] = (CellOwnerPhysicalBlock){pointer,bytes};
    cell_owner_bytes += bytes;
}
static size_t cell_owner_find(void *pointer) {
    size_t i = 0;
    while (i < cell_owner_live && cell_owner_blocks[i].pointer != pointer) ++i;
    CHECK(i < cell_owner_live);
    return i;
}
static void *cell_owner_malloc(size_t bytes) {
    void *pointer = xr_malloc(bytes); cell_owner_record(pointer,bytes); return pointer;
}
static void *cell_owner_calloc(size_t count, size_t bytes) {
    CHECK(!count || bytes <= SIZE_MAX/count);
    void *pointer = xr_calloc(count,bytes); cell_owner_record(pointer,count*bytes); return pointer;
}
static void cell_owner_free(void *pointer) {
    if (!pointer) return;
    size_t i = cell_owner_find(pointer);
    CHECK(cell_owner_blocks[i].bytes <= cell_owner_bytes);
    cell_owner_bytes -= cell_owner_blocks[i].bytes;
    cell_owner_blocks[i] = cell_owner_blocks[--cell_owner_live];
    xr_free(pointer);
}
static void *cell_owner_realloc(void *pointer, size_t bytes) {
    if (!pointer) { void *result = xr_realloc(NULL,bytes); cell_owner_record(result,bytes); return result; }
    size_t i = cell_owner_find(pointer);
    void *result = xr_realloc(pointer,bytes);
    if (result) {
        CHECK(cell_owner_blocks[i].bytes <= cell_owner_bytes);
        cell_owner_bytes -= cell_owner_blocks[i].bytes;
        CHECK(bytes <= SIZE_MAX-cell_owner_bytes);
        cell_owner_bytes += bytes; cell_owner_blocks[i] = (CellOwnerPhysicalBlock){result,bytes};
    }
    return result;
}
/* Include the real value owner once, under the physical allocator observer. */
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_calloc")
#pragma push_macro("xr_free")
#pragma push_macro("xr_realloc")
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#undef xr_realloc
#define xr_malloc(bytes) cell_owner_malloc(bytes)
#define xr_calloc(count, bytes) cell_owner_calloc(count,bytes)
#define xr_free(pointer) cell_owner_free(pointer)
#define xr_realloc(pointer, bytes) cell_owner_realloc(pointer,bytes)
#include "xir/xxir_value.c"
#pragma pop_macro("xr_realloc")
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_calloc")
#pragma pop_macro("xr_malloc")
#endif // CELL_OWNER_VALUE_PHYSICAL_H
