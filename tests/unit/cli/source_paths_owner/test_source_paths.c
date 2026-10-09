/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_paths.c - Real process queries and source-path ownership gates
 */
#include "app/cli/xcli_source_paths.h"
#include "os/os_proc.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s error=%lu\n", __LINE__, #c, GetLastError()); exit(1); } } while (0)
#include "source_paths_faults.inc.h"
static const XrCompileResourceLimits public_limits = {UINT64_C(1)<<30, UINT64_C(256)<<20, UINT64_C(8)<<30};
static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
static void reset(size_t failure) { CHECK(!live); attempts = total = peak = 0; fail_at = failure; }
static DWORD handles(void) { DWORD n; CHECK(GetProcessHandleCount(GetCurrentProcess(), &n)); return n; }
static void expected_path(const char *actual, const char *expected) {
    CHECK(actual && strlen(actual) == strlen(expected));
    for (size_t i = 0; actual[i]; ++i) CHECK(actual[i] == expected[i] ||
        ((actual[i] == '/' || actual[i] == '\\') && (expected[i] == '/' || expected[i] == '\\')));
}
static XrCliCompileSourceStatus run_paths(const char *entry, const XrCompileResourceLimits *limits,
    size_t failure, XrCompileResourceStats *stats, const char *stdlib, unsigned origin) {
    reset(failure); XrCompileResources *r = NULL;
    XrCompileResourceStatus opened = xr_compile_resources_new(limits, &r);
    if (opened != XR_COMPILE_RESOURCE_OK) {
        CHECK(!r && !live); return opened == XR_COMPILE_RESOURCE_BUDGET ? XR_CLI_COMPILE_SOURCE_BUDGET : XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY;
    }
    XrCliSourcePaths paths = {0}; XrCliSourcePathsDiagnostic diagnostic = {0};
    XrCliCompileSourceStatus status = xr_cli_compile_source_paths(r, entry, &paths, &diagnostic);
    CHECK(status == diagnostic.status);
    CHECK(xr_compile_resources_stats(r, stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats->allocated_bytes == total && stats->live_bytes == live && stats->peak_bytes == peak);
    xr_compile_resources_release(r);
    if (status == XR_CLI_COMPILE_SOURCE_OK) {
        expected_path(paths.stdlib, stdlib); CHECK((unsigned)paths.stdlib_origin == origin);
        CHECK(paths.entry && paths.entry[0]);
    } else CHECK(!paths.entry && !paths.stdlib && !paths.stdlib_origin);
    xr_cli_compile_source_paths_free(&paths); CHECK(!live);
    return status;
}
static void path_matrix(const char *entry, const char *stdlib, unsigned origin) {
    XrCompileResourceStats baseline = {0}, stats = {0};
    record_edges = true; edge_count = 0;
    CHECK(run_paths(entry, &public_limits, SIZE_MAX, &baseline, stdlib, origin) == XR_CLI_COMPILE_SOURCE_OK);
    record_edges = false;
    size_t points = attempts, cuts = edge_count;
    for (size_t i = 0; i < points; ++i)
        CHECK(run_paths(entry, &public_limits, i, &stats, stdlib, origin) == XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY);
    for (size_t i = 0; i < cuts; ++i) {
        XrCompileResourceLimits limits = public_limits; limits.work = edges[i] - 1;
        CHECK(run_paths(entry, &limits, SIZE_MAX, &stats, stdlib, origin) == XR_CLI_COMPILE_SOURCE_BUDGET);
    }
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = public_limits;
        if (axis == 0) limits.allocated_bytes = baseline.allocated_bytes - minus;
        if (axis == 1) limits.live_bytes = baseline.peak_bytes - minus;
        if (axis == 2) limits.work = baseline.work - minus;
        CHECK(run_paths(entry, &limits, SIZE_MAX, &stats, stdlib, origin) ==
            (minus ? XR_CLI_COMPILE_SOURCE_BUDGET : XR_CLI_COMPILE_SOURCE_OK));
    }
    printf("paths public defaults; %zu malloc %zu work cuts; total=%llu peak=%llu work=%llu exact/minus1 physical zero PASS\n",
        points, cuts, (unsigned long long)baseline.allocated_bytes, (unsigned long long)baseline.peak_bytes, (unsigned long long)baseline.work);
}
#include "stdlib_selection_cases.h"
static void query_once(bool executable, const char *expected, XrOsIoStatus expected_status) {
    reset(SIZE_MAX); XrCompileResources *r = NULL;
    CHECK(xr_compile_resources_new(&public_limits, &r) == XR_COMPILE_RESOURCE_OK);
    XrOsIoPolicy policy = xr_compile_io_policy(r); char *value = NULL;
    XrOsIoStatus s = executable ? xr_os_io_self_exe_path(&policy, &value) :
        xr_os_io_environment_get(&policy, "XRAY_TEST_VALUE", &value);
    CHECK(s == expected_status);
    xr_compile_resources_release(r);
    if (s == XR_OS_IO_OK) { CHECK(value); if (expected) expected_path(value, expected); }
    else CHECK(!value);
    if (value) xr_compile_resources_free(value);
    CHECK(!live);
}
static void queries(const char *exe, const char *value, bool absent) {
    query_once(true, exe, XR_OS_IO_OK);
    query_once(false, value, absent ? XR_OS_IO_NOT_FOUND : XR_OS_IO_OK);
    reset(SIZE_MAX); XrCompileResources *r = NULL;
    CHECK(xr_compile_resources_new(&unlimited, &r) == XR_COMPILE_RESOURCE_OK);
    XrOsIoPolicy p = xr_compile_io_policy(r); char *canary = (char *)(uintptr_t)1;
    CHECK(xr_os_io_self_exe_path(&p, &canary) == XR_OS_IO_BAD_ARGUMENT && canary == (char *)(uintptr_t)1);
    CHECK(xr_os_io_environment_get(&p, "XRAY_TEST_VALUE", &canary) == XR_OS_IO_BAD_ARGUMENT && canary == (char *)(uintptr_t)1);
    char *out = NULL;
    CHECK(xr_os_io_environment_get(&p, "", &out) == XR_OS_IO_BAD_ARGUMENT && !out);
    CHECK(xr_os_io_environment_get(&p, "=reserved", &out) == XR_OS_IO_BAD_ARGUMENT && !out);
    CHECK(xr_os_io_environment_get(&p, "\xc0\x80", &out) == XR_OS_IO_BAD_ARGUMENT && !out);
    CHECK(xr_os_io_environment_get(&p, "测试 key", &out) == XR_OS_IO_OK);
    CHECK(!strcmp(out, "fixed 中文")); xr_compile_resources_free(out); out = NULL;
    p.context = NULL; CHECK(xr_os_io_self_exe_path(&p, &out) == XR_OS_IO_BAD_ARGUMENT && !out);
    xr_compile_resources_release(r); CHECK(!live);
#ifndef SOURCE_PATHS_PRODUCTION
    for (unsigned operation = 0; operation < 2; ++operation) {
        io_attempts = 0; query_once(operation == 0, operation == 0 ? exe : value, operation == 1 && absent ? XR_OS_IO_NOT_FOUND : XR_OS_IO_OK);
        size_t calls = io_attempts;
        static const DWORD errors[] = {ERROR_ACCESS_DENIED, ERROR_NOT_ENOUGH_MEMORY, ERROR_OUTOFMEMORY};
        for (size_t i = 0; i < calls; ++i) for (unsigned e = 0; e < 3; ++e) {
            io_attempts = 0; io_fail_at = i; io_error = errors[e];
            query_once(operation == 0, NULL, e ? XR_OS_IO_OUT_OF_MEMORY : XR_OS_IO_IO);
        }
        io_fail_at = SIZE_MAX;
        printf("query %u %zu actual Win32 points x IO/OOM/OOM PASS\n", operation, calls);
    }
    if (!absent) {
        environment_grows = true; query_once(false, NULL, XR_OS_IO_IO); environment_grows = false;
    }
    module_truncates = true; query_once(true, NULL, XR_OS_IO_BUDGET); module_truncates = false;
    puts("injected size-change and repeated truncation preserve output PASS");
#endif
}
static void independent_query_work(void) {
    /* A 17-byte ASCII key and three-byte ASCII value require 166 submitted
     * work units: key validation/conversion 126, query/read 13, value conversion
     * 27. The already-existing ledger contributes one allocation work unit. */
    reset(SIZE_MAX); XrCompileResources *r = NULL;
    XrCompileResourceLimits limits = public_limits; limits.work = 167;
    CHECK(xr_compile_resources_new(&limits, &r) == XR_COMPILE_RESOURCE_OK);
    XrOsIoPolicy p = xr_compile_io_policy(r); char *first = NULL, *second = NULL;
    CHECK(xr_os_io_environment_get(&p, "XRAY_TEST_FORMULA", &first) == XR_OS_IO_OK);
    CHECK(!strcmp(first, "abc"));
    XrCompileResourceStats stats; CHECK(xr_compile_resources_stats(r, &stats) == XR_COMPILE_RESOURCE_OK && stats.work == 167);
    size_t count = attempts;
    CHECK(xr_os_io_environment_get(&p, "XRAY_TEST_FORMULA", &second) == XR_OS_IO_BUDGET && !second && attempts == count);
    xr_compile_resources_release(r); CHECK(!strcmp(first, "abc")); xr_compile_resources_free(first); CHECK(!live);
    reset(SIZE_MAX); limits.work = 166; r = NULL;
    CHECK(xr_compile_resources_new(&limits, &r) == XR_COMPILE_RESOURCE_OK);
    p = xr_compile_io_policy(r);
    CHECK(xr_os_io_environment_get(&p, "XRAY_TEST_FORMULA", &second) == XR_OS_IO_BUDGET && !second);
    xr_compile_resources_release(r); CHECK(!live);
    puts("independent environment formula work167/exact,166/rejected; second query same ledger exhausted PASS");
}
static void rejected_outputs(const char *entry) {
    reset(SIZE_MAX); XrCompileResources *r = NULL;
    CHECK(xr_compile_resources_new(&public_limits, &r) == XR_COMPILE_RESOURCE_OK);
    XrCliSourcePaths canary = {(char *)(uintptr_t)1, (char *)(uintptr_t)2, XR_CLI_STDLIB_WORKING_DIRECTORY};
    XrCliSourcePaths saved = canary; XrCliSourcePathsDiagnostic diagnostic = {0};
    size_t before = attempts;
    CHECK(xr_cli_compile_source_paths(r, entry, &canary, &diagnostic) == XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT);
    CHECK(!memcmp(&canary, &saved, sizeof(saved)) && attempts == before && diagnostic.stage == XR_CLI_SOURCE_PATH_INPUT);
    XrCliStdlibPath selected={(char *)(uintptr_t)3,XR_CLI_STDLIB_WORKING_DIRECTORY};
    CHECK(xr_cli_compile_stdlib_path(r,&selected,&diagnostic)==XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT);
    CHECK(selected.path==(char *)(uintptr_t)3&&selected.origin==XR_CLI_STDLIB_WORKING_DIRECTORY&&attempts==before);
    XrCliSourcePaths empty = {0};
    CHECK(xr_cli_compile_source_paths(r, "", &empty, &diagnostic) == XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT);
    CHECK(!empty.entry && !empty.stdlib && diagnostic.stage == XR_CLI_SOURCE_PATH_ENTRY);
    xr_compile_resources_release(r); CHECK(!live);
}
int wmain(int argc, wchar_t **wide) {
    CHECK(argc == 10); char *argv[10];
    for (int i = 0; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, NULL, 0, NULL, NULL); CHECK(n > 0);
        argv[i] = physical_malloc((size_t)n); CHECK(argv[i]);
        CHECK(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, argv[i], n, NULL, NULL) == n);
    }
    DWORD baseline = handles();
    queries(argv[7], argv[6], !strcmp(argv[1], "absent"));
    independent_query_work();
    rejected_outputs(argv[2]);
    unsigned origin = (unsigned)strtoul(argv[4], NULL, 10);
    XrCliCompileSourceStatus expected = (XrCliCompileSourceStatus)strtoul(argv[5], NULL, 10);
    XrCompileResourceStats stats = {0};
    CHECK(run_paths(argv[2], &public_limits, SIZE_MAX, &stats, argv[3], origin) == expected);
    XrCliCompileSourceStatus selected_expected=(XrCliCompileSourceStatus)strtoul(argv[9],NULL,10);
    CHECK(run_stdlib(&public_limits,SIZE_MAX,&stats,argv[8],origin)==selected_expected);
    if (!strcmp(argv[1], "matrix")) { path_matrix(argv[2], argv[3], origin); stdlib_matrix(argv[8],origin); }
    CHECK(handles() == baseline && !live);
    puts("owned queries, output preservation, producer death and exact handle delta zero PASS");
    for (int i = 0; i < argc; ++i) physical_free(argv[i]);
    return 0;
}
