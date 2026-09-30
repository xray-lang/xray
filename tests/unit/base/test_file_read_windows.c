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
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "failed at %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static size_t attempts, fail_at, live;
static void *probe_malloc(size_t bytes) {
    if (++attempts == fail_at) return NULL;
    void *result = xr_malloc(bytes);
    REQUIRE(result); ++live; return result;
}
static void probe_free(void *pointer) {
    if (pointer) { REQUIRE(live); --live; xr_free(pointer); }
}
#undef xr_malloc
#undef xr_free
#define xr_malloc probe_malloc
#define xr_free probe_free
#include "../../../src/os/win/file_read_win.c"

static void check(const char *root, const char *path, size_t limit, XrFileReadStatus expected) {
    XrFileBytes output = {(char *) 1, 999};
    XrFileReadStatus status = xr_file_read_under_root(root, path, limit, &output);
    if (status != expected) fprintf(stderr, "path %s: status %d expected %d\n", path, status, expected);
    REQUIRE(status == expected);
    if (status == XR_FILE_READ_OK) {
        REQUIRE(output.data && output.data[output.size] == 0);
        if (!strcmp(path, "empty")) REQUIRE(!output.size);
        else if (!strcmp(path, "binary")) REQUIRE(output.size == 3 && !memcmp(output.data, "a\0b", 3));
        else if (!strcmp(path, "large")) {
            REQUIRE(output.size == 140000);
            for (size_t i = 0; i < output.size; ++i) REQUIRE(output.data[i] == 'Q');
        }
        else REQUIRE(output.size == 3 && !memcmp(output.data, "abc", 3));
    } else REQUIRE(!output.data && !output.size);
    xr_free(output.data);
    REQUIRE(!live);
}

static size_t fault_sweep(const char *root, const char *path, XrFileReadStatus expected) {
    attempts = 0; fail_at = 0;
    check(root, path, 10, expected);
    size_t allocations = attempts;
    for (size_t failure = 1; failure <= allocations; ++failure) {
        attempts = 0; fail_at = failure;
        check(root, path, 10, XR_FILE_READ_OUT_OF_MEMORY);
    }
    fail_at = 0;
    return allocations;
}

static void root_and_sharing(const char *root) {
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/inside/data", root);
    REQUIRE(length > 0 && (size_t) length < sizeof(path));
    check(path, "x", 10, XR_FILE_READ_FORBIDDEN);
    XrFileReadStatus status;
    wchar_t *wide = wide_path(path, &status); REQUIRE(wide);
    HANDLE writer = CreateFileW(wide, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    xr_free(wide);
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
        "inside/data:stream", "inside/\xFF"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        check(argv[1], invalid[i], 10, XR_FILE_READ_FORBIDDEN);
    size_t allocations = fault_sweep(argv[1], "inside/data", XR_FILE_READ_OK);
    allocations += fault_sweep(argv[1], "escape/data", XR_FILE_READ_FORBIDDEN);
    allocations += fault_sweep(argv[1], "escape/missing", XR_FILE_READ_FORBIDDEN);
    allocations += fault_sweep(argv[1], "absent/child", XR_FILE_READ_MISSING);
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &handles_after));
    REQUIRE(handles_before == handles_after);
    printf("Physical file reads: %zu OOM points, zero allocations and handle residuals\n", allocations);
    return 0;
}
