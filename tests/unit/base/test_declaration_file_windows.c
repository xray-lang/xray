/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_declaration_file_windows.c - Owned manifest file admission and failure cleanup
 */
#include "base/xmalloc.h"
#include <stdlib.h>

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "failed at %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static size_t attempts, fail_at, live_count, live_bytes;
static struct { void *pointer; size_t size; } slots[1024];

static void *track(void *pointer, size_t size) {
    REQUIRE(pointer);
    for (size_t i = 0; i < XR_COUNTOF(slots); ++i) {
        if (slots[i].pointer) continue;
        slots[i].pointer = pointer;
        slots[i].size = size;
        ++live_count;
        live_bytes += size;
        return pointer;
    }
    exit(1);
}
static void *probe_malloc(size_t size) {
    return ++attempts == fail_at ? NULL : track(xr_malloc(size), size);
}
static void *probe_calloc(size_t count, size_t size) {
    return ++attempts == fail_at ? NULL : track(xr_calloc(count, size), count * size);
}
static void probe_free(void *pointer) {
    if (!pointer) return;
    for (size_t i = 0; i < XR_COUNTOF(slots); ++i) {
        if (slots[i].pointer != pointer) continue;
        slots[i].pointer = NULL;
        --live_count;
        live_bytes -= slots[i].size;
        xr_free(pointer);
        return;
    }
    exit(1);
}
static void *probe_realloc(void *pointer, size_t size) {
    if (++attempts == fail_at) return NULL;
    if (!pointer) return track(xr_realloc(NULL, size), size);
    for (size_t i = 0; i < XR_COUNTOF(slots); ++i) {
        if (slots[i].pointer != pointer) continue;
        void *grown = xr_realloc(pointer, size);
        REQUIRE(grown);
        live_bytes = live_bytes - slots[i].size + size;
        slots[i].pointer = grown;
        slots[i].size = size;
        return grown;
    }
    exit(1);
}
static char *probe_strdup(const char *source) {
    size_t size = strlen(source) + 1;
    char *copy = probe_malloc(size);
    if (copy) memcpy(copy, source, size);
    return copy;
}
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc probe_malloc
#define xr_calloc probe_calloc
#define xr_realloc probe_realloc
#define xr_free probe_free
#define xr_strdup probe_strdup

#include "../../../src/base/xtoml.c"
#include "../../../src/os/win/file_read_win.c"
#include "../../../src/module/xdeclaration_manifest.c"
#include "../../../src/module/xdeclaration_load.c"

static size_t load_case(const char *root, XrDeclarationInputBudget budget,
    XrDeclarationStatus expected, size_t failure) {
    fail_at = failure; attempts = 0;
    XrDeclarationManifest *manifest = (XrDeclarationManifest *) 1;
    XrDeclarationStatus status = xr_declaration_manifest_load(root, budget, &manifest, NULL);
    REQUIRE(status == expected);
    if (status == XR_DECLARATION_OK) {
        REQUIRE(manifest && manifest->count == 1);
        REQUIRE(!strcmp(manifest->records[0].module, "src/main.xr"));
        REQUIRE(!strcmp(manifest->records[0].name, "apply"));
        REQUIRE(manifest->records[0].parameter_count == 1);
        REQUIRE(!strcmp(manifest->records[0].parameters[0], "callback"));
    } else REQUIRE(!manifest);
    size_t count = attempts;
    xr_declaration_manifest_free(manifest);
    REQUIRE(!live_count && !live_bytes);
    return count;
}
static size_t work_case(const char *root, XrDeclarationInputBudget budget,
    XrDeclarationStatus expected) {
    attempts = 0; fail_at = 0;
    size_t used = SIZE_MAX;
    XrDeclarationManifest *manifest = NULL;
    REQUIRE(xr_declaration_manifest_load(root, budget, &manifest, &used) == expected);
    REQUIRE(used <= budget.parsing.work);
    if (expected != XR_DECLARATION_OK) REQUIRE(!manifest);
    xr_declaration_manifest_free(manifest);
    REQUIRE(!live_count && !live_bytes);
    return used;
}
static void combined_work_cases(const char *root, XrDeclarationInputBudget budget) {
    size_t used = work_case(root, budget, XR_DECLARATION_OK);
    REQUIRE(used > 0 && used < SIZE_MAX / 2);
    budget.parsing.work = used;
    REQUIRE(work_case(root, budget, XR_DECLARATION_OK) == used);
    --budget.parsing.work;
    work_case(root, budget, XR_DECLARATION_LIMIT);
    budget.parsing.work = used * 2;
    budget.parsing.work -= work_case(root, budget, XR_DECLARATION_OK);
    budget.parsing.work -= work_case(root, budget, XR_DECLARATION_OK);
    REQUIRE(budget.parsing.work == 0);
    work_case(root, budget, XR_DECLARATION_LIMIT);
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/nosection", root);
    REQUIRE(length > 0 && (size_t)length < sizeof(path));
    budget.parsing.work = 1024 * 1024;
    REQUIRE(work_case(path, budget, XR_DECLARATION_ABSENT) > 0);
    length = snprintf(path, sizeof(path), "%s/absent", root);
    REQUIRE(length > 0 && (size_t)length < sizeof(path));
    REQUIRE(work_case(path, budget, XR_DECLARATION_ABSENT) == 0);
}
int main(int argc, char **argv) {
    REQUIRE(argc == 3);
    XrDeclarationInputBudget budget = {{65536, 1024 * 1024, 1024 * 1024, 128}, {65536, 32, 32, 10000}};
    combined_work_cases(argv[1], budget);
    size_t count = load_case(argv[1], budget, XR_DECLARATION_OK, 0);
    DWORD before, after;
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &before));
    for (size_t fail = 1; fail <= count; ++fail)
        load_case(argv[1], budget, XR_DECLARATION_OUT_OF_MEMORY, fail);
    load_case(argv[2], budget, XR_DECLARATION_OK, 0);
    const char *const names[] = {"absent", "nosection", "syntax", "schema", "directory"};
    const XrDeclarationStatus expected[] = {XR_DECLARATION_ABSENT, XR_DECLARATION_ABSENT,
        XR_DECLARATION_INVALID, XR_DECLARATION_INVALID, XR_DECLARATION_FORBIDDEN};
    for (size_t i = 0; i < XR_COUNTOF(names); ++i) {
        char path[4096];
        int length = snprintf(path, sizeof(path), "%s/%s", argv[1], names[i]);
        REQUIRE(length > 0 && (size_t) length < sizeof(path));
        load_case(path, budget, expected[i], 0);
    }
    XrDeclarationInputBudget limited = budget;
    limited.parsing.input_bytes = 1; load_case(argv[1], limited, XR_DECLARATION_LIMIT, 0);
    limited = budget; limited.parsing.allocation_bytes = 1;
    load_case(argv[1], limited, XR_DECLARATION_LIMIT, 0);
    limited = budget; limited.parsing.work = 1; load_case(argv[1], limited, XR_DECLARATION_LIMIT, 0);
    limited = budget; limited.parsing.depth = 1; load_case(argv[1], limited, XR_DECLARATION_LIMIT, 0);
    limited = budget; limited.records.bytes = 1; load_case(argv[1], limited, XR_DECLARATION_LIMIT, 0);
    limited = budget; limited.records.records = 0; load_case(argv[1], limited, XR_DECLARATION_LIMIT, 0);
    limited = budget; limited.records.parameters = 0; load_case(argv[1], limited, XR_DECLARATION_LIMIT, 0);
    limited = budget; limited.records.work = 0; load_case(argv[1], limited, XR_DECLARATION_LIMIT, 0);
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &after));
    REQUIRE(before == after);
    printf("Manifest file admission: %zu OOM points, all stage budgets, zero owners/handles\n", count);
    return 0;
}
