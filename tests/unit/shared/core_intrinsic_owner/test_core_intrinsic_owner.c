/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_core_intrinsic_owner.c - Exact registry lookup work and failure boundaries
 */
#include "base/xchecks.h"
#include "base/xcompile_resources.h"
#include "shared/xr_core_intrinsic.h"

#include <stdio.h>
#include <string.h>
#if defined(XR_OS_WINDOWS)
#include <windows.h>
#endif

#define CHECK(condition) XR_CHECK(condition, #condition)

typedef struct QueryWork {
    XrCompileResources *resources;
    uint64_t calls;
    bool stopped;
} QueryWork;

static const XrCoreIntrinsicDesc sentinel = {0};

static bool query_work(void *context, uint64_t units) {
    QueryWork *work = context;
    CHECK(!work->stopped);
    CHECK(units == 1);
    work->calls++;
    XrCompileResourceStatus status = xr_compile_resources_work(work->resources, units);
    CHECK(status == XR_COMPILE_RESOURCE_OK || status == XR_COMPILE_RESOURCE_BUDGET);
    work->stopped = status != XR_COMPILE_RESOURCE_OK;
    return !work->stopped;
}

static QueryWork new_work(uint64_t available) {
    XrCompileResourceLimits limits = {UINT64_MAX, UINT64_MAX, available + 1};
    QueryWork work = {0};
    CHECK(xr_compile_resources_new(&limits, &work.resources) == XR_COMPILE_RESOURCE_OK);
    return work;
}

static XrCompileResourceStats stats(const QueryWork *work) {
    XrCompileResourceStats result;
    CHECK(xr_compile_resources_stats(work->resources, &result) == XR_COMPILE_RESOURCE_OK);
    return result;
}

static void check_case(const char *name, size_t length, XrCoreBuiltinId expected,
                       uint64_t reads) {
    const XrCoreIntrinsicDesc *descriptor = xr_core_intrinsic_by_id(expected);
    CHECK(xr_core_intrinsic_by_source_name(name, length) == descriptor);
    for (uint64_t available = 0; available <= reads + 1; available++) {
        QueryWork work = new_work(available);
        XrCompileResourceStats before = stats(&work);
        const XrCoreIntrinsicDesc *output = &sentinel;
        XrCoreIntrinsicQueryStatus status = xr_core_intrinsic_by_source_name_work(
            &work, query_work, name, length, &output);
        if (available < reads) {
            CHECK(status == XR_CORE_INTRINSIC_QUERY_WORK_LIMIT);
            CHECK(output == &sentinel && work.stopped);
            CHECK(work.calls == available + 1);
        } else {
            CHECK(status == XR_CORE_INTRINSIC_QUERY_OK);
            CHECK(output == descriptor && !work.stopped);
            CHECK(work.calls == reads);
        }
        XrCompileResourceStats after = stats(&work);
        CHECK(after.work == 1 + (available < reads ? available : reads));
        CHECK(after.allocation_count == before.allocation_count);
        CHECK(after.allocated_bytes == before.allocated_bytes);
        CHECK(after.live_bytes == before.live_bytes && after.peak_bytes == before.peak_bytes);
        xr_compile_resources_release(work.resources);
    }
}

static void fixed_read_counts(void) {
    /* Equal byte pairs cost two; a descriptor boundary costs one. Prior rows
     * cost two per equal pair and two for a mismatch, or one for their NUL. */
    check_case("assert", 6, XR_CORE_BUILTIN_ASSERT, 2 * 6 + 1);
    check_case("assertEqual", 11, XR_CORE_BUILTIN_ASSERT_EQUAL, 13 + 2 * 11 + 1);
    check_case("assertThrows", 12, XR_CORE_BUILTIN_ASSERT_THROWS, 13 + 14 + 25);
    check_case("assertPanics", 12, XR_CORE_BUILTIN_ASSERT_PANICS, 13 + 14 + 14 + 25);
    check_case("print", 5, XR_CORE_BUILTIN_PRINT, 4 * 2 + 11);
    check_case("len", 3, XR_CORE_BUILTIN_LEN, 5 * 2 + 7);
    check_case("", 0, XR_CORE_BUILTIN_NONE, 6);
    check_case("x", 1, XR_CORE_BUILTIN_NONE, 6 * 2);
    check_case("asser", 5, XR_CORE_BUILTIN_NONE, 4 * 11 + 2 * 2);
    check_case("assertP", 7, XR_CORE_BUILTIN_NONE, 13 + 14 + 14 + 15 + 4);
    check_case("assert\0", 7, XR_CORE_BUILTIN_NONE, 13 + 3 * 14 + 4);
    check_case("assertPanicsX", 13, XR_CORE_BUILTIN_NONE, 13 + 14 + 14 + 25 + 4);
    char long_name[4096];
    memset(long_name, 'x', sizeof(long_name));
    memcpy(long_name, "assertPanics", 12);
    check_case(long_name, sizeof(long_name), XR_CORE_BUILTIN_NONE, 70);
}

static void generated_registry(void) {
    char error[256];
    CHECK(xr_core_intrinsic_registry_validate(error, sizeof(error)));
    CHECK(xr_core_intrinsic_count() == XR_CORE_BUILTIN_COUNT);
    QueryWork work = new_work(1000);
    for (size_t i = 0; i < xr_core_intrinsic_count(); i++) {
        const XrCoreIntrinsicDesc *row = xr_core_intrinsic_at(i);
        const XrCoreIntrinsicDesc *output = &sentinel;
        CHECK(row && xr_core_intrinsic_descriptor_validate(row));
        CHECK(xr_core_intrinsic_by_source_name_work(&work, query_work, row->source_name,
            strlen(row->source_name), &output) == XR_CORE_INTRINSIC_QUERY_OK);
        CHECK(output == row && xr_core_intrinsic_by_id(row->id) == row);
    }
    CHECK(work.calls == 13 + 36 + 52 + 66 + 19 + 17);
    CHECK(xr_core_intrinsic_at(xr_core_intrinsic_count()) == NULL);
    CHECK(xr_core_intrinsic_by_source_name(NULL, SIZE_MAX) == NULL);
    xr_compile_resources_release(work.resources);
}

static void bad_arguments(void) {
    QueryWork work = new_work(100);
    const XrCoreIntrinsicDesc *output = &sentinel;
    CHECK(xr_core_intrinsic_by_source_name_work(NULL, query_work, "assert", 6, &output) ==
        XR_CORE_INTRINSIC_QUERY_BAD_ARGUMENT);
    CHECK(xr_core_intrinsic_by_source_name_work(&work, NULL, "assert", 6, &output) ==
        XR_CORE_INTRINSIC_QUERY_BAD_ARGUMENT);
    CHECK(xr_core_intrinsic_by_source_name_work(&work, query_work, NULL, 0, &output) ==
        XR_CORE_INTRINSIC_QUERY_BAD_ARGUMENT);
    CHECK(xr_core_intrinsic_by_source_name_work(&work, query_work, "assert", 6, NULL) ==
        XR_CORE_INTRINSIC_QUERY_BAD_ARGUMENT);
    CHECK(output == &sentinel && work.calls == 0 && stats(&work).work == 1);
    xr_compile_resources_release(work.resources);
}

static void cumulative_budget(void) {
    QueryWork work = new_work(13 + 17 - 1);
    const XrCoreIntrinsicDesc *output = NULL;
    CHECK(xr_core_intrinsic_by_source_name_work(&work, query_work, "assert", 6, &output) ==
        XR_CORE_INTRINSIC_QUERY_OK);
    const XrCoreIntrinsicDesc *first = output;
    CHECK(first && first->id == XR_CORE_BUILTIN_ASSERT);
    CHECK(xr_core_intrinsic_by_source_name_work(&work, query_work, "len", 3, &output) ==
        XR_CORE_INTRINSIC_QUERY_WORK_LIMIT);
    CHECK(output == first && work.calls == 30 && stats(&work).work == 30);
    /* The caller can retry; the ledger cannot regain consumed work. */
    work.stopped = false;
    CHECK(xr_core_intrinsic_by_source_name_work(&work, query_work, "assert", 6, &output) ==
        XR_CORE_INTRINSIC_QUERY_WORK_LIMIT);
    CHECK(output == first && work.calls == 31 && stats(&work).work == 30);
    xr_compile_resources_release(work.resources);
}

#if defined(XR_OS_WINDOWS)
static void guarded_input(void) {
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    size_t page_size = system.dwPageSize;
    unsigned char *pages = VirtualAlloc(NULL, 2 * page_size, MEM_RESERVE | MEM_COMMIT,
                                        PAGE_READWRITE);
    CHECK(pages != NULL);
    DWORD old_protect;
    CHECK(VirtualProtect(pages + page_size, page_size, PAGE_NOACCESS, &old_protect));
    char *name = (char *)pages + page_size - 12;
    memcpy(name, "assertPanics", 12);
    check_case(name, 12, XR_CORE_BUILTIN_ASSERT_PANICS, 66);
    /* No candidate is longer than twelve bytes, so a huge input length does
     * not authorize reading an input terminator or scanning the unused tail. */
    check_case(name, SIZE_MAX, XR_CORE_BUILTIN_NONE, 70);
    const char *inaccessible = (const char *)pages + page_size;
    check_case(inaccessible, 0, XR_CORE_BUILTIN_NONE, 6);
    for (uint64_t available = 0; available <= 1; available++) {
        QueryWork work = new_work(available);
        const XrCoreIntrinsicDesc *output = &sentinel;
        CHECK(xr_core_intrinsic_by_source_name_work(&work, query_work, inaccessible,
            SIZE_MAX, &output) == XR_CORE_INTRINSIC_QUERY_WORK_LIMIT);
        CHECK(output == &sentinel && work.calls == available + 1);
        CHECK(stats(&work).work == available + 1);
        xr_compile_resources_release(work.resources);
    }
    CHECK(VirtualFree(pages, 0, MEM_RELEASE));
}
#endif

int main(void) {
    generated_registry();
    fixed_read_counts();
    bad_arguments();
    cumulative_budget();
#if defined(XR_OS_WINDOWS)
    guarded_input();
#endif
    puts("core intrinsic owner: fixed counts, every work threshold, shared ledger, and guarded reads pass");
    return 0;
}
