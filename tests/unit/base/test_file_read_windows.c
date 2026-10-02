/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_file_read_windows.c - Physical-root and owned file-input regression checks
 */
#include "base/xmalloc.h"
#include "base/xcompile_resources.h"
#include "base/xwindows_utf8.h"
#include "os/os_file_read.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "failed at %d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct Allocation { void *memory; size_t size; } Allocation;
static Allocation blocks[64];
static size_t attempts, fail_at, live, allocated, peak;
static uint64_t boundaries[8192];
static size_t boundary_count;
static void *probe_malloc(size_t bytes) {
    if (++attempts == fail_at) return NULL;
    void *memory = xr_malloc(bytes); REQUIRE(memory);
    size_t i = 0;
    while (i < 64 && blocks[i].memory) ++i;
    REQUIRE(i < 64); blocks[i] = (Allocation){memory, bytes};
    live += bytes; allocated += bytes; if (live > peak) peak = live;
    return memory;
}
static void probe_free(void *memory) {
    if (!memory) return;
    size_t i = 0;
    while (i < 64 && blocks[i].memory != memory) ++i;
    REQUIRE(i < 64); live -= blocks[i].size; blocks[i] = (Allocation){0}; xr_free(memory);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc probe_malloc
#define xr_free probe_free
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free

static XrOsIoStatus tracked_work(void *context, uint64_t units) {
    XrCompileResourceStats s;
    REQUIRE(xr_compile_resources_stats(context, &s) == XR_COMPILE_RESOURCE_OK);
    REQUIRE(boundary_count < 8192);
    boundaries[boundary_count++] = s.work + units;
    XrCompileResourceStatus status = xr_compile_resources_work(context, units);
    return status == XR_COMPILE_RESOURCE_OK ? XR_OS_IO_OK : XR_OS_IO_BUDGET;
}
static XrCompileResourceStats check_limits(const char *root, const char *path, size_t limit,
    XrFileReadStatus expected, XrCompileResourceLimits limits, bool twice) {
    REQUIRE(!live); allocated = peak = boundary_count = 0;
    XrCompileResources *owner = NULL;
    XrCompileResourceStatus created = xr_compile_resources_new(&limits, &owner);
    if (created != XR_COMPILE_RESOURCE_OK) {
        REQUIRE(created == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && expected == XR_FILE_READ_OUT_OF_MEMORY);
        REQUIRE(!live && !owner); return (XrCompileResourceStats){0};
    }
    XrOsIoPolicy policy = xr_compile_io_policy(owner); policy.work = tracked_work;
    XrFileBytes output = {(char *)1, 999};
    XrFileReadStatus status = xr_os_io_read_under_root(&policy, root, path, limit, &output);
    if (status != expected) fprintf(stderr, "path %s: status %d expected %d\n", path, status, expected);
    REQUIRE(status == expected);
    if (status == XR_FILE_READ_OK) {
        REQUIRE(output.data && output.data[output.size] == 0);
        if (!strcmp(path, "empty")) REQUIRE(!output.size);
        else if (!strcmp(path, "binary")) REQUIRE(output.size == 3 && !memcmp(output.data, "a\0b", 3));
        else if (!strcmp(path, "large")) {
            REQUIRE(output.size == 140000);
            for (size_t i = 0; i < output.size; ++i) REQUIRE(output.data[i] == 'Q');
        } else REQUIRE(output.size == 3 && !memcmp(output.data, "abc", 3));
    } else REQUIRE(output.data == (char *)1 && output.size == 999);
    XrCompileResourceStats s;
    REQUIRE(xr_compile_resources_stats(owner, &s) == XR_COMPILE_RESOURCE_OK);
    REQUIRE(s.live_bytes == live && s.allocated_bytes == allocated && s.peak_bytes == peak);
    if (twice) {
        XrFileBytes second = {(char *)2, 333};
        REQUIRE(xr_os_io_read_under_root(&policy, root, path, limit, &second) == XR_FILE_READ_LIMIT);
        REQUIRE(second.data == (char *)2 && second.size == 333);
    }
    xr_compile_resources_release(owner);
    if (status == XR_FILE_READ_OK) {
        REQUIRE(output.data[0] == (output.size ? (path[0] == 'l' && !strcmp(path,"large") ? 'Q' : 'a') : 0));
        policy.free(policy.context, output.data);
    }
    REQUIRE(!live);
    return s;
}
static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
static void check(const char *root, const char *path, size_t limit, XrFileReadStatus expected) {
    (void)check_limits(root, path, limit, expected, unlimited, false);
}
static size_t fault_sweep(const char *root, const char *path, XrFileReadStatus expected) {
    attempts = 0; fail_at = 0; check(root, path, 10, expected);
    size_t count = attempts;
    for (size_t failure = 1; failure <= count; ++failure) {
        attempts = 0; fail_at = failure;
        check(root, path, 10, XR_FILE_READ_OUT_OF_MEMORY);
    }
    fail_at = 0; return count;
}
static void resource_sweep(const char *root, const char *path, XrFileReadStatus expected) {
    attempts = 0; fail_at = 0;
    XrCompileResourceStats s = check_limits(root, path, 10, expected, unlimited, false);
    size_t count = boundary_count;
    uint64_t points[8192]; memcpy(points, boundaries, count * sizeof(*points));
    for (size_t i = 0; i < count; ++i) {
        XrCompileResourceLimits limits = unlimited; limits.work = points[i] - 1;
        check_limits(root, path, 10, XR_FILE_READ_LIMIT, limits, false);
    }
    XrCompileResourceLimits exact = {s.allocated_bytes, s.peak_bytes, s.work};
    check_limits(root, path, 10, expected, exact, expected == XR_FILE_READ_OK);
    for (unsigned axis = 0; axis < 3; ++axis) {
        XrCompileResourceLimits limits = unlimited;
        if (axis == 0) limits.allocated_bytes = s.allocated_bytes - 1;
        if (axis == 1) limits.live_bytes = s.peak_bytes - 1;
        if (axis == 2) limits.work = s.work - 1;
        check_limits(root, path, 10, XR_FILE_READ_LIMIT, limits, false);
    }
    printf("resource boundaries %s: %zu, allocations=%llu work=%llu\n", path, count,
        (unsigned long long)s.allocation_count, (unsigned long long)s.work);
}
static void argument_and_guard_checks(void) {
    XrFileBytes output = {(char *)3, 77};
    XrOsIoPolicy policy = xr_os_io_system_policy();
    REQUIRE(xr_os_io_read_under_root(NULL, (char *)1, (char *)1, 1, &output) == XR_FILE_READ_BAD_ARGUMENT);
    REQUIRE(xr_os_io_read_under_root(&policy, NULL, (char *)1, 1, &output) == XR_FILE_READ_BAD_ARGUMENT);
    XrCompileResources *owner = NULL;
    XrCompileResourceLimits limits = unlimited; limits.work = 1;
    REQUIRE(xr_compile_resources_new(&limits, &owner) == XR_COMPILE_RESOURCE_OK);
    policy = xr_compile_io_policy(owner);
    REQUIRE(xr_os_io_read_under_root(&policy, (char *)1, (char *)1, 1, &output) == XR_FILE_READ_LIMIT);
    REQUIRE(output.data == (char *)3 && output.size == 77);
    xr_compile_resources_release(owner); REQUIRE(!live);
    /* Bootstrap is one work unit; the first invalid path byte is one more. */
    XrCompileResourceStats s = check_limits((char *)1, ":", 0, XR_FILE_READ_FORBIDDEN, unlimited, false);
    REQUIRE(s.work == 2 && s.allocation_count == 1);
    s = check_limits((char *)1, "a/", 0, XR_FILE_READ_FORBIDDEN, unlimited, false);
    REQUIRE(s.work == 5 && s.allocation_count == 1);
}

static void root_and_sharing(const char *root) {
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/inside/data", root);
    REQUIRE(length > 0 && (size_t) length < sizeof(path));
    check(path, "x", 10, XR_FILE_READ_FORBIDDEN);
    XrOsIoPolicy system = xr_os_io_system_policy();
    wchar_t *wide = NULL;
    REQUIRE(xr_win_utf8_path_owned(&system, path, &wide) == XR_OS_IO_OK);
    HANDLE writer = CreateFileW(wide, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    system.free(system.context, wide);
    REQUIRE(writer != INVALID_HANDLE_VALUE);
    check(root, "inside/data", 10, XR_FILE_READ_IO);
    REQUIRE(CloseHandle(writer));
    length = snprintf(path, sizeof(path), "%s/absent", root);
    REQUIRE(length > 0 && (size_t) length < sizeof(path));
    check(path, "x", 10, XR_FILE_READ_IO);
    length = snprintf(path, sizeof(path), "%s/\xE4\xB8\xAD\xE6\x96\x87\xE6\xA0\xB9", root);
    REQUIRE(length > 0 && (size_t) length < sizeof(path));
    check(path, "inside/data", 10, XR_FILE_READ_OK);
    check("relative", "x", 10, XR_FILE_READ_FORBIDDEN);
}

int main(int argc, char **argv) {
    REQUIRE(argc == 3);
    check(argv[1], "inside/data", 3, XR_FILE_READ_OK);
    DWORD handles_before = 0, handles_after = 0;
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &handles_before));
    check(argv[1], "inside/data", 2, XR_FILE_READ_LIMIT);
    check(argv[1], "empty", 0, XR_FILE_READ_OK);
    check(argv[1], "binary", 3, XR_FILE_READ_OK);
    check(argv[1], "large", 140000, XR_FILE_READ_OK);
    check(argv[1], "large", 139999, XR_FILE_READ_LIMIT);
    root_and_sharing(argv[1]);
    check(argv[1], "missing", 10, XR_FILE_READ_MISSING);
    check(argv[1], "absent/child", 10, XR_FILE_READ_MISSING);
    check(argv[1], "escape/missing", 10, XR_FILE_READ_FORBIDDEN);
    check(argv[1], "escape/absent/child", 10, XR_FILE_READ_FORBIDDEN);
    check(argv[1], "inside", 10, XR_FILE_READ_FORBIDDEN);
    check(argv[1], "escape/data", 10, XR_FILE_READ_FORBIDDEN);
    check(argv[1], "linked/data", 10, XR_FILE_READ_OK);
    check(argv[2], "inside/data", 10, XR_FILE_READ_OK);
    check(argv[1], "\xE4\xB8\xAD\xE6\x96\x87", 10, XR_FILE_READ_OK);
    const char *const invalid[] = {"", "../outside/data", "/inside/data", "inside//data",
        "inside/./data", "inside/../data", "inside/data.", "inside/data ", "inside\\data",
        "inside/data:stream", "inside/", "inside/\xFF"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        check(argv[1], invalid[i], 10, XR_FILE_READ_FORBIDDEN);
    size_t allocations = fault_sweep(argv[1], "inside/data", XR_FILE_READ_OK);
    allocations += fault_sweep(argv[1], "escape/data", XR_FILE_READ_FORBIDDEN);
    allocations += fault_sweep(argv[1], "escape/missing", XR_FILE_READ_FORBIDDEN);
    allocations += fault_sweep(argv[1], "absent/child", XR_FILE_READ_MISSING);
    resource_sweep(argv[1], "inside/data", XR_FILE_READ_OK);
    resource_sweep(argv[1], "escape/missing", XR_FILE_READ_FORBIDDEN);
    resource_sweep(argv[1], "absent/child", XR_FILE_READ_MISSING);
    argument_and_guard_checks();
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &handles_after));
    REQUIRE(handles_before == handles_after);
    printf("Physical file reads: %zu OOM points, zero allocations and handle residuals\n", allocations);
    return 0;
}
