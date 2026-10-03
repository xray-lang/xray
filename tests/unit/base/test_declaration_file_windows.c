/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_declaration_file_windows.c - Owned manifest file admission and failure cleanup
 */
#include "module/xdeclaration_load.h"
#include "base/xmalloc.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct Allocation { void *memory; size_t size; } Allocation;
static Allocation blocks[4096];
static size_t attempts, fail_at, live, allocated, peak;
static void *probe_malloc(size_t bytes) {
    if (++attempts == fail_at) return NULL;
    void *memory = xr_malloc(bytes); REQUIRE(memory);
    size_t i = 0; while (i < 4096 && blocks[i].memory) ++i;
    REQUIRE(i < 4096); blocks[i] = (Allocation){memory, bytes};
    live += bytes; allocated += bytes; if (live > peak) peak = live; return memory;
}
static void probe_free(void *memory) {
    if (!memory) return;
    size_t i = 0; while (i < 4096 && blocks[i].memory != memory) ++i;
    REQUIRE(i < 4096); live -= blocks[i].size; blocks[i] = (Allocation){0}; xr_free(memory);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc probe_malloc
#define xr_free probe_free
#define xr_compile_resources_work actual_work
#include "base/xcompile_resources.c"
#undef xr_compile_resources_work

static bool record_work;
static uint64_t work_points[32768];
static size_t work_count;
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *owner, uint64_t units) {
    if (record_work) {
        XrCompileResourceStats s;
        REQUIRE(xr_compile_resources_stats(owner, &s) == XR_COMPILE_RESOURCE_OK);
        REQUIRE(work_count < 32768); work_points[work_count++] = s.work + units;
    }
    return actual_work(owner, units);
}

static const XrDeclarationInputLimits shape = {{65536, 128}, {32, 32}};
static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
static XrCompileResourceStats load_case(const char *root, XrDeclarationInputLimits input,
    XrCompileResourceLimits limits, XrDeclarationStatus expected, size_t failure, bool twice) {
    REQUIRE(!live); fail_at = failure; attempts = allocated = peak = 0;
    XrCompileResources *resources = NULL;
    XrCompileResourceStatus created = xr_compile_resources_new(&limits, &resources);
    if (created != XR_COMPILE_RESOURCE_OK) {
        REQUIRE(created == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && expected == XR_DECLARATION_OUT_OF_MEMORY);
        REQUIRE(!resources && !live); return (XrCompileResourceStats){0};
    }
    XrDeclarationManifest *manifest = (XrDeclarationManifest *)1;
    XrDeclarationStatus status = xr_compile_declaration_manifest_load(resources, root, &input, &manifest);
    if (status != expected) fprintf(stderr, "status %d expected %d\n", status, expected);
    REQUIRE(status == expected);
    if (status == XR_DECLARATION_OK) {
        REQUIRE(manifest && manifest->count == 1);
        REQUIRE(!strcmp(manifest->records[0].module, "src/main.xr"));
        REQUIRE(!strcmp(manifest->records[0].name, "apply"));
        REQUIRE(manifest->records[0].parameter_count == 1);
        REQUIRE(!strcmp(manifest->records[0].parameters[0], "callback"));
    } else REQUIRE(manifest == (XrDeclarationManifest *)1);
    XrCompileResourceStats stats;
    REQUIRE(xr_compile_resources_stats(resources, &stats) == XR_COMPILE_RESOURCE_OK);
    REQUIRE(stats.live_bytes == live && stats.allocated_bytes == allocated && stats.peak_bytes == peak);
    if (twice) {
        XrDeclarationManifest *second = (XrDeclarationManifest *)2;
        REQUIRE(xr_compile_declaration_manifest_load(resources, root, &input, &second) == XR_DECLARATION_LIMIT);
        REQUIRE(second == (XrDeclarationManifest *)2);
    }
    xr_compile_resources_release(resources);
    if (status == XR_DECLARATION_OK) {
        REQUIRE(!strcmp(manifest->records[0].module, "src/main.xr"));
        xr_compile_declaration_manifest_free(manifest);
    }
    REQUIRE(!live); return stats;
}
static void resource_cases(const char *root) {
    work_count = 0; record_work = true;
    XrCompileResourceStats stats = load_case(root, shape, unlimited, XR_DECLARATION_OK, 0, false);
    record_work = false; size_t count = attempts;
    for (size_t i = 1; i <= count; ++i)
        load_case(root, shape, unlimited, XR_DECLARATION_OUT_OF_MEMORY, i, false);
    for (size_t i = 0; i < work_count; ++i) {
        XrCompileResourceLimits limits = unlimited; limits.work = work_points[i] - 1;
        load_case(root, shape, limits, XR_DECLARATION_LIMIT, 0, false);
    }
    XrCompileResourceLimits exact = {stats.allocated_bytes, stats.peak_bytes, stats.work};
    load_case(root, shape, exact, XR_DECLARATION_OK, 0, true);
    for (unsigned axis = 0; axis < 3; ++axis) {
        XrCompileResourceLimits limits = unlimited;
        if (!axis) limits.allocated_bytes = stats.allocated_bytes - 1;
        if (axis == 1) limits.live_bytes = stats.peak_bytes - 1;
        if (axis == 2) limits.work = stats.work - 1;
        load_case(root, shape, limits, XR_DECLARATION_LIMIT, 0, false);
    }
    printf("File/TOML/schema: %zu OOM points, %zu submitted-work cutoffs, one ledger, zero physical owners\n", count, work_count);
}
int main(int argc, char **argv) {
    REQUIRE(argc == 3);
    DWORD before, after; REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &before));
    resource_cases(argv[1]);
    load_case(argv[2], shape, unlimited, XR_DECLARATION_OK, 0, false);
    const char *const names[] = {"absent", "nosection", "syntax", "schema", "directory"};
    const XrDeclarationStatus expected[] = {XR_DECLARATION_ABSENT, XR_DECLARATION_ABSENT,
        XR_DECLARATION_INVALID, XR_DECLARATION_INVALID, XR_DECLARATION_FORBIDDEN};
    for (size_t i = 0; i < XR_COUNTOF(names); ++i) {
        char path[4096]; int length = snprintf(path, sizeof(path), "%s/%s", argv[1], names[i]);
        REQUIRE(length > 0 && (size_t)length < sizeof(path));
        XrCompileResourceStats stats = load_case(path, shape, unlimited, expected[i], 0, false);
        REQUIRE(stats.work > 1);
    }
    XrDeclarationInputLimits limited = shape;
    limited.parsing.input_bytes = 1; load_case(argv[1], limited, unlimited, XR_DECLARATION_LIMIT, 0, false);
    limited = shape; limited.parsing.depth = 1; load_case(argv[1], limited, unlimited, XR_DECLARATION_LIMIT, 0, false);
    limited = shape; limited.records.records = 0; load_case(argv[1], limited, unlimited, XR_DECLARATION_LIMIT, 0, false);
    limited = shape; limited.records.parameters = 0; load_case(argv[1], limited, unlimited, XR_DECLARATION_LIMIT, 0, false);
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &after)); REQUIRE(before == after);
    return 0;
}
