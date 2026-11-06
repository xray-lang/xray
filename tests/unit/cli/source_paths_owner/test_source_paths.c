/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_paths.c - Real process queries and source-path ownership gates
 */
#include "api/xsource_paths.h"
#include "os/os_proc.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s error=%lu\n", __LINE__, #c, GetLastError()); exit(1); } } while (0)
#include "source_paths_faults.inc.h"
static const XrCompileResourceLimits public_limits = {
    .memory_mode = XR_COMPILE_MEMORY_ORDINARY, .profile_allocations = true
};
static void reset(size_t failure) { CHECK(!live); attempts = total = peak = 0; fail_at = failure; }
static DWORD handles(void) { DWORD n; CHECK(GetProcessHandleCount(GetCurrentProcess(), &n)); return n; }
static void expected_path(const char *actual, const char *expected) {
    CHECK(actual && strlen(actual) == strlen(expected));
    for (size_t i = 0; actual[i]; ++i) CHECK(actual[i] == expected[i] ||
        ((actual[i] == '/' || actual[i] == '\\') && (expected[i] == '/' || expected[i] == '\\')));
}
/* Success owns no probe record; a failure owns a dense prefix of candidates.
 * A reported lookup prints them for the driver's independent expectation. */
static void candidates_check(XrSourceCompileStatus status, XrSourcePathsDiagnostic *diagnostic,
    const char *report) {
    CHECK(diagnostic->candidate_count <= XR_SOURCE_STDLIB_CANDIDATE_LIMIT);
    if (status == XR_SOURCE_COMPILE_OK) CHECK(!diagnostic->candidate_count);
    for (uint32_t i = 0; i < XR_SOURCE_STDLIB_CANDIDATE_LIMIT; ++i)
        CHECK((i < diagnostic->candidate_count) == (diagnostic->candidates[i] != NULL));
    for (uint32_t i = 0; report && i < diagnostic->candidate_count; ++i)
        printf("%s=%s\n", report, diagnostic->candidates[i]);
    xr_source_paths_diagnostic_free(diagnostic);
    CHECK(!diagnostic->candidate_count && !diagnostic->candidates[0]);
}
static XrSourceCompileStatus run_paths(const char *entry, const XrCompileResourceLimits *limits,
    size_t failure, XrCompileResourceStats *stats, const char *stdlib, unsigned origin, const char *report) {
    reset(failure); XrCompileResources *r = NULL;
    XrCompileResourceStatus opened = xr_compile_resources_new(limits, &r);
    if (opened != XR_COMPILE_RESOURCE_OK) {
        CHECK(!r && !live);
        CHECK(opened == XR_COMPILE_RESOURCE_MEMORY_LIMIT || opened == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
        return opened == XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_SOURCE_COMPILE_MEMORY_LIMIT : XR_SOURCE_COMPILE_OUT_OF_MEMORY;
    }
    XrSourcePaths paths = {0}; XrSourcePathsDiagnostic diagnostic = {0};
    XrSourceCompileStatus status = xr_source_paths(r, entry, &paths, &diagnostic);
    CHECK(status == diagnostic.status);
    CHECK(xr_compile_resources_stats(r, stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats->allocated_bytes == total && stats->held_bytes == live && stats->peak_bytes == peak);
    xr_compile_resources_release(r);
    if (status == XR_SOURCE_COMPILE_OK) {
        expected_path(paths.stdlib, stdlib); CHECK((unsigned)paths.stdlib_origin == origin);
        CHECK(paths.entry && paths.entry[0]);
    } else CHECK(!paths.entry && !paths.stdlib && !paths.stdlib_origin);
    candidates_check(status, &diagnostic, report);
    xr_source_paths_free(&paths); CHECK(!live);
    return status;
}
static void path_matrix(const char *entry, const char *stdlib, unsigned origin) {
    XrCompileResourceStats baseline = {0}, stats = {0};
    CHECK(run_paths(entry, &public_limits, SIZE_MAX, &baseline, stdlib, origin, NULL) == XR_SOURCE_COMPILE_OK);
    size_t points = attempts;
    for (size_t i = 0; i < points; ++i)
        CHECK(run_paths(entry, &public_limits, i, &stats, stdlib, origin, NULL) == XR_SOURCE_COMPILE_OUT_OF_MEMORY);
    XrCompileResourceLimits limits = public_limits;
    limits.memory_mode = XR_COMPILE_MEMORY_FINITE; limits.memory_limit = baseline.peak_bytes;
    CHECK(run_paths(entry, &limits, SIZE_MAX, &stats, stdlib, origin, NULL) == XR_SOURCE_COMPILE_OK);
    CHECK(limits.memory_limit > 0); --limits.memory_limit;
    CHECK(run_paths(entry, &limits, SIZE_MAX, &stats, stdlib, origin, NULL) == XR_SOURCE_COMPILE_MEMORY_LIMIT);
    CHECK(run_paths(entry, &public_limits, SIZE_MAX, &stats, stdlib, origin, NULL) == XR_SOURCE_COMPILE_OK);
    printf("paths ordinary profile; %zu malloc points; total=%llu peak=%llu finite exact/minus1/recovery physical zero PASS\n",
        points, (unsigned long long)baseline.allocated_bytes, (unsigned long long)baseline.peak_bytes);
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
    CHECK(xr_compile_resources_new(&public_limits, &r) == XR_COMPILE_RESOURCE_OK);
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
    module_truncates = true; query_once(true, NULL, XR_OS_IO_OVERFLOW); module_truncates = false;
    puts("injected size-change and repeated truncation preserve output PASS");
#endif
}
static void independent_query_ownership(void) {
    reset(SIZE_MAX); XrCompileResources *r = NULL;
    CHECK(xr_compile_resources_new(&public_limits, &r) == XR_COMPILE_RESOURCE_OK);
    XrOsIoPolicy p = xr_compile_io_policy(r); char *first = NULL, *second = NULL;
    CHECK(xr_os_io_environment_get(&p, "XRAY_TEST_FORMULA", &first) == XR_OS_IO_OK);
    CHECK(xr_os_io_environment_get(&p, "XRAY_TEST_FORMULA", &second) == XR_OS_IO_OK);
    CHECK(first != second && !strcmp(first, "abc") && !strcmp(second, "abc"));
    xr_compile_resources_release(r);
    CHECK(!strcmp(first, "abc") && !strcmp(second, "abc"));
    xr_compile_resources_free(first); CHECK(!strcmp(second, "abc"));
    xr_compile_resources_free(second); CHECK(!live);
    puts("independent environment results survive producer and sibling retirement PASS");
}
static void rejected_outputs(const char *entry) {
    reset(SIZE_MAX); XrCompileResources *r = NULL;
    CHECK(xr_compile_resources_new(&public_limits, &r) == XR_COMPILE_RESOURCE_OK);
    XrSourcePaths canary = {(char *)(uintptr_t)1, (char *)(uintptr_t)2, XR_STDLIB_WORKING_DIRECTORY};
    XrSourcePaths saved = canary; XrSourcePathsDiagnostic diagnostic = {0};
    size_t before = attempts;
    CHECK(xr_source_paths(r, entry, &canary, &diagnostic) == XR_SOURCE_COMPILE_BAD_ARGUMENT);
    CHECK(!memcmp(&canary, &saved, sizeof(saved)) && attempts == before && diagnostic.stage == XR_SOURCE_PATH_INPUT);
    XrStdlibPath selected={(char *)(uintptr_t)3,XR_STDLIB_WORKING_DIRECTORY};
    CHECK(xr_source_stdlib_path(r,&selected,&diagnostic)==XR_SOURCE_COMPILE_BAD_ARGUMENT);
    CHECK(selected.path==(char *)(uintptr_t)3&&selected.origin==XR_STDLIB_WORKING_DIRECTORY&&attempts==before);
    XrSourcePaths empty = {0};
    CHECK(xr_source_paths(r, "", &empty, &diagnostic) == XR_SOURCE_COMPILE_BAD_ARGUMENT);
    CHECK(!empty.entry && !empty.stdlib && diagnostic.stage == XR_SOURCE_PATH_ENTRY);
    /* A diagnostic still owning a probe record would leak if overwritten. */
    XrSourcePathsDiagnostic owner = {0}; owner.candidate_count = 1; owner.candidates[0] = (char *)(uintptr_t)4;
    XrSourcePathsDiagnostic kept = owner;
    CHECK(xr_source_paths(r, entry, &empty, &owner) == XR_SOURCE_COMPILE_BAD_ARGUMENT);
    CHECK(!memcmp(&owner, &kept, sizeof(kept)) && !empty.entry && attempts == before);
    XrStdlibPath none = {0};
    CHECK(xr_source_stdlib_path(r, &none, &owner) == XR_SOURCE_COMPILE_BAD_ARGUMENT);
    CHECK(!memcmp(&owner, &kept, sizeof(kept)) && !none.path && attempts == before);
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
    independent_query_ownership();
    rejected_outputs(argv[2]);
    unsigned origin = (unsigned)strtoul(argv[4], NULL, 10);
    XrSourceCompileStatus expected = (XrSourceCompileStatus)strtoul(argv[5], NULL, 10);
    XrCompileResourceStats stats = {0};
    CHECK(run_paths(argv[2], &public_limits, SIZE_MAX, &stats, argv[3], origin, "candidate") == expected);
    XrSourceCompileStatus selected_expected=(XrSourceCompileStatus)strtoul(argv[9],NULL,10);
    CHECK(run_stdlib(&public_limits,SIZE_MAX,&stats,argv[8],origin,"stdlib-candidate")==selected_expected);
    if (!strcmp(argv[1], "matrix")) { path_matrix(argv[2], argv[3], origin); stdlib_matrix(argv[8],origin); }
    CHECK(handles() == baseline && !live);
    puts("owned queries, output preservation, producer death and exact handle delta zero PASS");
    for (int i = 0; i < argc; ++i) physical_free(argv[i]);
    return 0;
}
