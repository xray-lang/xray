/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * mcp_compile_memory.h - Physical compiler allocation and fault observation
 */
#ifndef MCP_COMPILE_MEMORY_H
#define MCP_COMPILE_MEMORY_H
#include "base/xmalloc.h"
#include "xir/xxir_compile_context.h"
#include <string.h>
typedef struct SourceFixtureCompileAllocation { void *pointer; size_t bytes; } SourceFixtureCompileAllocation;
static SourceFixtureCompileAllocation source_fixture_compile_allocations[65536];
static size_t source_fixture_compile_live, source_fixture_compile_bytes, source_fixture_compile_attempts;
static size_t source_fixture_compile_peak_blocks,source_fixture_compile_peak_bytes;
static const char *source_fixture_compile_phase="ledger",*source_fixture_compile_peak_phase="ledger";
static size_t source_fixture_compile_fail_at=SIZE_MAX;
static bool source_fixture_compile_injected;
static size_t source_fixture_compile_slot(const void *pointer) {
    uint64_t key = (uint64_t)(uintptr_t)pointer;
    key ^= key >> 33; key *= UINT64_C(0xff51afd7ed558ccd); key ^= key >> 33;
    return (size_t)key & 65535;
}
static void *source_fixture_compile_malloc(size_t bytes) {
    if (source_fixture_compile_attempts++==source_fixture_compile_fail_at) {
        source_fixture_compile_injected=true; return NULL;
    }
    void *pointer = xr_malloc(bytes);
    if (!pointer) return NULL;
    if(source_fixture_compile_live>=32768)fprintf(stderr,"Source compiler observer full: blocks=%zu bytes=%zu attempts=%zu\n",source_fixture_compile_live,source_fixture_compile_bytes,source_fixture_compile_attempts);
    CHECK(source_fixture_compile_live < 32768 && bytes <= SIZE_MAX - source_fixture_compile_bytes);
    size_t slot = source_fixture_compile_slot(pointer);
    while (source_fixture_compile_allocations[slot].pointer) slot = (slot + 1) & 65535;
    source_fixture_compile_allocations[slot] = (SourceFixtureCompileAllocation){pointer, bytes};
    ++source_fixture_compile_live; source_fixture_compile_bytes += bytes;
    if(source_fixture_compile_live>source_fixture_compile_peak_blocks){source_fixture_compile_peak_blocks=source_fixture_compile_live;source_fixture_compile_peak_phase=source_fixture_compile_phase;}
    if(source_fixture_compile_bytes>source_fixture_compile_peak_bytes)source_fixture_compile_peak_bytes=source_fixture_compile_bytes;
    return pointer;
}
static void source_fixture_compile_free(void *pointer) {
    if (!pointer) return;
    size_t hole = source_fixture_compile_slot(pointer);
    while (source_fixture_compile_allocations[hole].pointer != pointer) {
        CHECK(source_fixture_compile_allocations[hole].pointer); hole = (hole + 1) & 65535;
    }
    CHECK(source_fixture_compile_live && source_fixture_compile_bytes >= source_fixture_compile_allocations[hole].bytes);
    --source_fixture_compile_live; source_fixture_compile_bytes -= source_fixture_compile_allocations[hole].bytes;
    for (size_t next = (hole + 1) & 65535; source_fixture_compile_allocations[next].pointer; next = (next + 1) & 65535) {
        size_t home = source_fixture_compile_slot(source_fixture_compile_allocations[next].pointer);
        bool stays = hole <= next ? hole < home && home <= next : hole < home || home <= next;
        if (!stays) { source_fixture_compile_allocations[hole] = source_fixture_compile_allocations[next]; hole = next; }
    }
    source_fixture_compile_allocations[hole] = (SourceFixtureCompileAllocation){0}; xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) source_fixture_compile_malloc(bytes)
#define xr_free(pointer) source_fixture_compile_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")

#endif // MCP_COMPILE_MEMORY_H
