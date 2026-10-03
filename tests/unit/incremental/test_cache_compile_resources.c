/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cache_compile_resources.c - Cache and filesystem ownership admission
 */
#include "base/xmalloc.h"
#include "base/xio_policy.h"
#include "base/xcompile_resources.h"
#include "base/xfileio.h"
#include "os/os_fs.h"
#include "os/os_dir.h"
#include "incremental/xr_cache_store.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef XR_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)

typedef XrCompileCacheStatus (*OpenPrototype)(XrCompileResources *, const XrCacheStoreConfig *, XrCacheStore **);
typedef XrCacheVerifyStatus (*VerifyPrototype)(XrCompileResources *, XrCacheArtifactKind, XrCacheKey, const uint8_t *, size_t, void *);
_Static_assert(_Generic(&xr_compile_cache_store_open, OpenPrototype: 1, default: 0), "Cache construction requires caller resources");
_Static_assert(_Generic((XrCompileCacheArtifactVerifier)0, VerifyPrototype: 1, default: 0), "Verifier requires resources and typed status");
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[512];
static size_t calls, live, physical, fail_at = SIZE_MAX;
static void *counted_malloc(size_t bytes) {
    if (calls++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes); if (!memory) return NULL;
    for (size_t i = 0; i < 512; ++i) if (!allocations[i].pointer) {
        allocations[i] = (Allocation){memory, bytes}; ++live; physical += bytes; return memory;
    }
    CHECK(false); return NULL;
}
static void counted_free(void *memory) {
    if (!memory) return;
    for (size_t i = 0; i < 512; ++i) if (allocations[i].pointer == memory) {
        physical -= allocations[i].bytes; --live; allocations[i] = (Allocation){0}; xr_free(memory); return;
    }
    CHECK(false);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc counted_malloc
#define xr_free counted_free
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")

static size_t handles;
#ifdef XR_OS_WINDOWS
static HANDLE owned_handles[128];
static bool fail_read, fail_next;
static DWORD probe_attributes, probe_error;
static BOOL WINAPI checked_attributes(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID output) {
    if (probe_error) { SetLastError(probe_error); return FALSE; }
    BOOL ok = GetFileAttributesExW(path, level, output);
    if (ok && probe_attributes) ((WIN32_FILE_ATTRIBUTE_DATA *)output)->dwFileAttributes = probe_attributes;
    return ok;
}
static void handle_added(HANDLE handle) {
    if (handle == INVALID_HANDLE_VALUE) return;
    for (size_t i = 0; i < 128; ++i) if (!owned_handles[i]) { owned_handles[i] = handle; ++handles; return; }
    CHECK(false);
}
static void handle_removed(HANDLE handle) {
    for (size_t i = 0; i < 128; ++i) if (owned_handles[i] == handle) { owned_handles[i] = NULL; --handles; return; }
    CHECK(false);
}
static HANDLE WINAPI counted_create_file(LPCWSTR path, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD flags, HANDLE template_file) {
    HANDLE handle = CreateFileW(path, access, share, security, creation, flags, template_file);
    handle_added(handle); return handle;
}
static BOOL WINAPI counted_close_handle(HANDLE handle) {
    BOOL closed = CloseHandle(handle); CHECK(closed); handle_removed(handle); return closed;
}
static HANDLE WINAPI counted_find_first(LPCWSTR pattern, LPWIN32_FIND_DATAW data) {
    HANDLE handle = FindFirstFileW(pattern, data); handle_added(handle); return handle;
}
static BOOL WINAPI counted_find_close(HANDLE handle) {
    BOOL closed = FindClose(handle); CHECK(closed); handle_removed(handle); return closed;
}
static BOOL WINAPI checked_read(HANDLE handle, LPVOID buffer, DWORD size, LPDWORD read, LPOVERLAPPED overlapped) {
    if (fail_read) { SetLastError(ERROR_READ_FAULT); return FALSE; }
    return ReadFile(handle, buffer, size, read, overlapped);
}
static BOOL WINAPI checked_find_next(HANDLE handle, LPWIN32_FIND_DATAW data) {
    if (fail_next) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return FindNextFileW(handle, data);
}
#define GetFileAttributesExW checked_attributes
#define CreateFileW counted_create_file
#define CloseHandle counted_close_handle
#define FindFirstFileW counted_find_first
#define FindClose counted_find_close
#define ReadFile checked_read
#define FindNextFileW checked_find_next
#include "os/win/fs_win.c"
#include "os/win/dir_win.c"
#include "base/xfileio.c"
#undef GetFileAttributesExW
#undef CreateFileW
#undef CloseHandle
#undef FindFirstFileW
#undef FindClose
#undef ReadFile
#undef FindNextFileW
#else
#include "os/unix/fs_unix.c"
#include "os/unix/dir_unix.c"
#include "base/xfileio.c"
#endif

/* Independent size arithmetic for the ledger root and aligned allocation header. */
typedef union OracleAlignment { long double floating; uint64_t integer; void *pointer; } OracleAlignment;
typedef union OracleHeader {
    struct { void *owner; size_t bytes; } fields;
#ifdef XR_COMPILER_MSVC
    OracleAlignment alignment;
#else
    max_align_t alignment;
#endif
} OracleHeader;
typedef struct OracleLedger { uint64_t limits[3], stats[5], references; atomic_bool locked; } OracleLedger;
static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
static XrOsIoPolicy system_policy;
static char fixture[128], raw_path[180];
static XrCacheStoreConfig config;
static size_t oom_points, formula_cases, residual_cases;
static XrCacheKey key(unsigned id) { XrCacheKey value = {{0}}; value.bytes[0] = (uint8_t)id; return value; }
static XrCompileResources *ledger(XrCompileResourceLimits limits) {
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK); return resources;
}
static XrCompileResourceStats statistics(XrCompileResources *resources) {
    XrCompileResourceStats value; CHECK(xr_compile_resources_stats(resources, &value) == XR_COMPILE_RESOURCE_OK); return value;
}
static void released(XrCompileResources *resources) {
    xr_compile_resources_release(resources); CHECK(!live && !physical && !handles);
}
static XrCacheVerifyStatus accepted(XrCompileResources *resources, XrCacheArtifactKind kind,
    XrCacheKey id, const uint8_t *bytes, size_t size, void *context) {
    (void)kind; (void)id; CHECK(resources == context);
    return size == 3 && !memcmp(bytes, "abc", 3) ? XR_CACHE_VERIFY_OK : XR_CACHE_VERIFY_REJECTED;
}
static XrCacheStore *opened(XrCompileResources *resources) {
    XrCacheStore *store = NULL;
    CHECK(xr_compile_cache_store_open(resources, &config, &store) == XR_COMPILE_CACHE_OK);
    CHECK(xr_compile_cache_store_resources(store) == resources); return store;
}
static XrCacheBlob poison_blob(void) { XrCacheBlob blob; memset(&blob, 0xA5, sizeof(blob)); return blob; }
static char *system_join(const char *left, const char *right) {
    char *path = NULL; CHECK(xr_path_join_owned(&system_policy, left, right, &path) == XR_OS_IO_OK); return path;
}
static void system_free(void *memory) { system_policy.free(system_policy.context, memory); }
static char *disk_path(unsigned id) {
    char hex[XR_CACHE_KEY_HEX_SIZE]; xr_cache_key_hex(key(id), hex);
    char *directory = system_join(fixture, "xsm"), *path = system_join(directory, hex); system_free(directory); return path;
}
static bool disk_exists(unsigned id) {
    char *path = disk_path(id); bool exists = false;
    XrOsIoStatus status = xr_os_io_is_file(&system_policy, path, &exists); system_free(path);
    CHECK(status == XR_OS_IO_OK || status == XR_OS_IO_NOT_FOUND); return status == XR_OS_IO_OK && exists;
}
static size_t clean_directory(const char *directory, bool all, bool age_only) {
    XrDirIter *iterator = NULL; XrDirEntry entry; size_t count = 0;
    CHECK(xr_os_io_dir_open(&system_policy, directory, &iterator) == XR_OS_IO_OK);
    XrOsIoStatus status;
    while ((status = xr_os_io_dir_next(iterator, &entry)) == XR_OS_IO_OK) {
        if (entry.is_dir || (!all && strncmp(entry.name, ".tmp-", 5))) continue;
        char *path = system_join(directory, entry.name); ++count;
        if (age_only) {
#ifdef XR_OS_WINDOWS
            wchar_t *wide = NULL; CHECK(xr_win_utf8_path_owned(&system_policy, path, &wide) == XR_OS_IO_OK);
            HANDLE handle = CreateFileW(wide, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL); CHECK(handle != INVALID_HANDLE_VALUE);
            const uint64_t ticks = UINT64_C(116444736100000000);
            FILETIME old = {(DWORD)ticks, (DWORD)(ticks >> 32)};
            CHECK(SetFileTime(handle, NULL, NULL, &old)); CHECK(CloseHandle(handle)); system_free(wide);
#else
            struct timespec times[2] = {{1, 0}, {1, 0}};
            CHECK(!utimensat(AT_FDCWD, path, times, 0));
#endif
        } else CHECK(xr_os_io_remove(&system_policy, path) == XR_OS_IO_OK);
        system_free(path);
    }
    CHECK(status == XR_OS_IO_END); xr_os_io_dir_close(iterator); return count;
}
static void reset_publish(void) {
    char *path = disk_path(200); XrOsIoStatus status = xr_os_io_remove(&system_policy, path);
    CHECK(status == XR_OS_IO_OK || status == XR_OS_IO_NOT_FOUND); system_free(path);
    char *directory = system_join(fixture, "xsm"); (void)clean_directory(directory, false, false); system_free(directory);
}
static void paths(void) {
    static const struct { const char *dir, *expected; } joins[] = {
        {"/", "/x"}, {"///", "/x"}, {"a///", "a/x"}, {"", "x"},
#ifdef XR_OS_WINDOWS
        {"C:\\", "C:\\x"}, {"C:\\\\\\", "C:\\x"},
        {"\\\\server\\share", "\\\\server\\share/x"},
        {"\\\\server\\share\\\\", "\\\\server\\share/x"},
#endif
    };
    static const struct { const char *path, *expected; } parents[] = {
        {"a", "."}, {"/a", "/"}, {"/a///", "/"}, {"/a/b///", "/a"}, {"/", "/"},
#ifdef XR_OS_WINDOWS
        {"C:\\x", "C:\\"}, {"C:\\\\\\", "C:\\"},
        {"\\\\server\\share", "\\\\server\\share"},
        {"\\\\server\\share\\\\", "\\\\server\\share"},
        {"\\\\server\\share\\x", "\\\\server\\share"},
#endif
    };
    XrCompileResources *resources = ledger(unlimited); XrOsIoPolicy policy = xr_compile_io_policy(resources);
    for (size_t i = 0; i < sizeof(joins) / sizeof(joins[0]); ++i) {
        char *path = NULL; CHECK(xr_path_join_owned(&policy, joins[i].dir, "x", &path) == XR_OS_IO_OK);
        CHECK(!strcmp(path, joins[i].expected)); xr_compile_resources_free(path);
    }
    for (size_t i = 0; i < sizeof(parents) / sizeof(parents[0]); ++i) {
        char *path = NULL; CHECK(xr_path_dirname_owned(&policy, parents[i].path, &path) == XR_OS_IO_OK);
        CHECK(!strcmp(path, parents[i].expected)); xr_compile_resources_free(path);
    }
    char *owned = NULL; CHECK(xr_realpath_owned(&policy, ".", &owned) == XR_OS_IO_OK);
    xr_compile_resources_release(resources); CHECK(owned[0]); xr_compile_resources_free(owned); CHECK(!live && !physical);
    XrOsIoPolicy invalid = system_policy; invalid.context = NULL; owned = (char *)(uintptr_t)1;
    CHECK(xr_path_join_owned(&invalid, "/", "x", &owned) == XR_OS_IO_BAD_ARGUMENT && owned == (char *)(uintptr_t)1);
}
static void join_formula(void) {
    /* "abc" + "xy": two NUL scans (4+3), two tail checks, allocation,
     * three copied directory bytes, one separator and three copied name bytes. */
    const uint64_t bytes = sizeof(OracleLedger) + sizeof(OracleHeader) + 7, work = 18;
    for (unsigned dimension = 0; dimension < 3; ++dimension) for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = unlimited;
        if (!dimension) limits.allocated_bytes = bytes - minus;
        if (dimension == 1) limits.live_bytes = bytes - minus;
        if (dimension == 2) limits.work = work - minus;
        XrCompileResources *resources = ledger(limits); XrOsIoPolicy policy = xr_compile_io_policy(resources);
        char *out = (char *)(uintptr_t)1;
        CHECK(xr_path_join_owned(&policy, "abc", "xy", &out) == (minus ? XR_OS_IO_BUDGET : XR_OS_IO_OK));
        if (minus) CHECK(out == (char *)(uintptr_t)1);
        else {
            CHECK(!strcmp(out, "abc/xy")); XrCompileResourceStats stats = statistics(resources);
            CHECK(stats.allocated_bytes == bytes && stats.peak_bytes == bytes && stats.work == work);
            xr_compile_resources_free(out);
        }
        released(resources); ++formula_cases;
    }
}
static void read_formula(void) {
#ifdef XR_OS_WINDOWS
    const uint64_t n = strlen(raw_path), size = 3;
    const uint64_t allocation = sizeof(OracleLedger) + 2 * sizeof(OracleHeader) + 2 * (n + 1) + size;
    const uint64_t peak = sizeof(OracleLedger) + sizeof(OracleHeader) + 2 * (n + 1);
    /* UTF-8 conversion: 5n+6. Create/type/info: 3. Payload allocation: 1.
     * Main read: 1+size; EOF probe: 2; ledger construction: 1. */
    const uint64_t work = 5 * n + size + 14;
    for (unsigned dimension = 0; dimension < 3; ++dimension) for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = unlimited;
        if (!dimension) limits.allocated_bytes = allocation - minus;
        if (dimension == 1) limits.live_bytes = peak - minus;
        if (dimension == 2) limits.work = work - minus;
        XrCompileResources *resources = ledger(limits); XrOsIoPolicy policy = xr_compile_io_policy(resources);
        uint8_t *out = (uint8_t *)(uintptr_t)1; size_t length = 999;
        CHECK(xr_os_io_read_regular_file(&policy, raw_path, 3, &out, &length) == (minus ? XR_OS_IO_BUDGET : XR_OS_IO_OK));
        if (minus) CHECK(out == (uint8_t *)(uintptr_t)1 && length == 999);
        else {
            XrCompileResourceStats stats = statistics(resources);
            CHECK(length == 3 && !memcmp(out, "abc", 3));
            CHECK(stats.work == work && stats.allocated_bytes == allocation && stats.peak_bytes == peak);
            xr_compile_resources_free(out);
        }
        released(resources); ++formula_cases;
    }
#endif
}
static void entry_formula(void) {
    XrCompileResources *resources = ledger(unlimited); XrCacheStore *store = opened(resources);
    XrCompileResourceStats prefix = statistics(resources);
    xr_compile_cache_store_close(store); released(resources);
    const uint64_t d = strlen(fixture) + 4;
    const uint64_t work = 2 * d + 232, allocation = sizeof(OracleHeader) + d + 66;
    /* A paid retained block makes the final entry path determine peak live bytes. */
    const size_t ballast = (size_t)prefix.peak_bytes + 1024;
    for (unsigned dimension = 0; dimension < 3; ++dimension) for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = unlimited;
        if (!dimension) limits.allocated_bytes = prefix.allocated_bytes + sizeof(OracleHeader) + ballast + allocation - minus;
        if (dimension == 1) limits.live_bytes = prefix.live_bytes + sizeof(OracleHeader) + ballast + allocation - minus;
        if (dimension == 2) limits.work = prefix.work + 1 + work - minus;
        resources = ledger(limits); store = opened(resources); void *block = NULL;
        CHECK(xr_compile_resources_alloc(resources, ballast, &block) == XR_COMPILE_RESOURCE_OK);
        XrCompileResourceStats before = statistics(resources); char *out = (char *)(uintptr_t)1;
        CHECK(xr_compile_cache_store_entry_path(store, XR_CACHE_ARTIFACT_XSM, key(1), &out) ==
            (minus ? XR_COMPILE_CACHE_BUDGET : XR_COMPILE_CACHE_OK));
        if (minus) CHECK(out == (char *)(uintptr_t)1);
        else {
            XrCompileResourceStats after = statistics(resources); CHECK(after.work - before.work == work);
            CHECK(after.allocated_bytes - before.allocated_bytes == allocation);
            char *expected = disk_path(1); CHECK(!strcmp(out, expected)); system_free(expected); xr_compile_resources_free(out);
        }
        xr_compile_resources_free(block); xr_compile_cache_store_close(store); released(resources); ++formula_cases;
    }
}
static void fixture_create(void) {
#ifdef XR_OS_WINDOWS
    unsigned long id = GetCurrentProcessId();
#else
    unsigned long id = (unsigned long)getpid();
#endif
    CHECK(snprintf(fixture, sizeof(fixture), "cache-resources-%lu", id) > 0);
    config = (XrCacheStoreConfig){fixture, 1024 * 1024, 512, UINT64_MAX};
    XrCompileResources *resources = ledger(unlimited); XrCacheStore *store = opened(resources);
    for (unsigned i = 1; i <= 18; ++i) CHECK(xr_compile_cache_store_publish(store, XR_CACHE_ARTIFACT_XSM, key(i),
        (const uint8_t *)"abc", 3, accepted, resources) == XR_CACHE_PUBLISH_OK);
    xr_compile_cache_store_close(store); released(resources);
    CHECK(snprintf(raw_path, sizeof(raw_path), "%s/raw", fixture) > 0);
    CHECK(xr_os_io_write_new_file_sync(&system_policy, raw_path, (const uint8_t *)"abc", 3) == XR_OS_IO_OK);
}
static void owner_lifetime(void) {
    XrCompileResources *resources = ledger(unlimited); XrCacheStore *store = opened(resources); XrCacheBlob blob = {0};
    CHECK(xr_compile_cache_store_load(store, XR_CACHE_ARTIFACT_XSM, key(1), accepted, resources, &blob) == XR_CACHE_LOAD_HIT);
    xr_compile_cache_store_close(store); xr_compile_resources_release(resources);
    CHECK(live == 2 && !handles && blob.size == 3 && !memcmp(blob.bytes, "abc", 3));
    xr_compile_cache_blob_release(&blob); CHECK(!live && !physical);
}
static int operation(unsigned op, XrCompileResources *resources, XrCacheStore **store) {
    if (!op) return (int)xr_compile_cache_store_open(resources, &config, store);
    if (op == 1) {
        XrCacheBlob output = poison_blob(), saved = output;
        XrCacheLoadStatus status = xr_compile_cache_store_load(*store, XR_CACHE_ARTIFACT_XSM, key(1), accepted, resources, &output);
        if (status == XR_CACHE_LOAD_HIT) { CHECK(output.size == 3 && !memcmp(output.bytes, "abc", 3)); xr_compile_cache_blob_release(&output); }
        else CHECK(!memcmp(&output, &saved, sizeof(output)));
        return (int)status;
    }
    if (op == 2) return (int)xr_compile_cache_store_publish(*store, XR_CACHE_ARTIFACT_XSM, key(200),
        (const uint8_t *)"abc", 3, accepted, resources);
    XrCacheCollectStats stats, saved; memset(&stats, 0xA5, sizeof(stats)); saved = stats;
    XrCompileCacheStatus status = xr_compile_cache_store_collect(*store, &stats);
    if (status == XR_COMPILE_CACHE_OK) CHECK(stats.live_entries == 18 && stats.live_bytes == 18 * 99 && !stats.removed_entries);
    else CHECK(!memcmp(&stats, &saved, sizeof(stats)));
    return (int)status;
}
static void recover_temporaries(void) {
    char *directory = system_join(fixture, "xsm"); size_t residual = clean_directory(directory, false, true);
    if (residual) {
        ++residual_cases; uint64_t age = config.stale_temp_age_ns; config.stale_temp_age_ns = 1;
        XrCompileResources *resources = ledger(unlimited); XrCacheStore *store = opened(resources);
        xr_compile_cache_store_close(store); released(resources); config.stale_temp_age_ns = age;
        CHECK(clean_directory(directory, false, false) == 0); CHECK(disk_exists(1) && !disk_exists(200));
    }
    system_free(directory);
}
static void failures(void) {
    static const int oom[] = {XR_COMPILE_CACHE_OUT_OF_MEMORY, XR_CACHE_LOAD_OUT_OF_MEMORY,
        XR_CACHE_PUBLISH_OUT_OF_MEMORY, XR_COMPILE_CACHE_OUT_OF_MEMORY};
    for (unsigned op = 0; op < 4; ++op) {
        if (op == 2) reset_publish();
        XrCompileResources *resources = ledger(unlimited); XrCacheStore *store = op ? opened(resources) : NULL;
        size_t start = calls; CHECK(operation(op, resources, &store) == 0); size_t points = calls - start;
        xr_compile_cache_store_close(store); released(resources);
        for (size_t point = 0; point < points; ++point) {
            if (op == 2) reset_publish();
            resources = ledger(unlimited); store = op ? opened(resources) : (XrCacheStore *)(uintptr_t)1;
            fail_at = calls + point; CHECK(operation(op, resources, &store) == oom[op]); fail_at = SIZE_MAX;
            if (!op) CHECK(store == (XrCacheStore *)(uintptr_t)1);
            else {
                CHECK(xr_compile_cache_store_resource_status(store) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
                XrCacheCollectStats stats; memset(&stats, 0xA5, sizeof(stats));
                size_t unchanged = calls; CHECK(xr_compile_cache_store_collect(store, &stats) == XR_COMPILE_CACHE_OUT_OF_MEMORY);
                CHECK(calls == unchanged); xr_compile_cache_store_close(store);
            }
            released(resources); CHECK(disk_exists(1));
            if (op == 2) { CHECK(!disk_exists(200)); recover_temporaries(); }
            ++oom_points;
        }
        printf("actual malloc OOM operation %u: %zu points\n", op, points);
    }
}
static size_t budget_cases;
static void work_failures(void) {
    static const int budget[] = {XR_COMPILE_CACHE_BUDGET, XR_CACHE_LOAD_BUDGET,
        XR_CACHE_PUBLISH_BUDGET, XR_COMPILE_CACHE_BUDGET};
    for (unsigned op = 0; op < 4; ++op) {
        if (op == 2) reset_publish();
        XrCompileResources *resources = ledger(unlimited); XrCacheStore *store = op ? opened(resources) : NULL;
        uint64_t prefix = statistics(resources).work;
        CHECK(operation(op, resources, &store) == 0); uint64_t tail = statistics(resources).work - prefix;
        xr_compile_cache_store_close(store); released(resources);
        for (unsigned cut = 0; cut < 33; ++cut) {
            if (op == 2) reset_publish();
            XrCompileResourceLimits limits = unlimited;
            limits.work = prefix + (cut == 32 ? tail - 1 : tail * cut / 32);
            resources = ledger(limits); store = op ? opened(resources) : (XrCacheStore *)(uintptr_t)1;
            CHECK(operation(op, resources, &store) == budget[op]);
            if (!op) CHECK(store == (XrCacheStore *)(uintptr_t)1);
            else {
                CHECK(xr_compile_cache_store_resource_status(store) == XR_COMPILE_RESOURCE_BUDGET);
                size_t unchanged = calls;
                CHECK(xr_compile_cache_store_collect(store, NULL) == XR_COMPILE_CACHE_BUDGET && calls == unchanged);
                xr_compile_cache_store_close(store);
            }
            released(resources); CHECK(disk_exists(1));
            if (op == 2) { CHECK(!disk_exists(200)); recover_temporaries(); }
            ++budget_cases;
        }
        printf("work cutoffs operation %u: 33 selected from %llu submitted units\n", op, (unsigned long long)tail);
    }
}
static XrOsIoStatus os_operation(unsigned op, XrOsIoPolicy *policy) {
    if (op >= 5) return xr_file_probe_owned(policy, raw_path, op == 5);
    char *path = (char *)(uintptr_t)1;
    if (op < 3) {
        XrOsIoStatus status = op == 0 ? xr_realpath_owned(policy, ".", &path) :
            op == 1 ? xr_path_dirname_owned(policy, "/a/b///", &path) : xr_path_join_owned(policy, "/", "x", &path);
        if (status == XR_OS_IO_OK) policy->free(policy->context, path);
        else CHECK(path == (char *)(uintptr_t)1);
        return status;
    }
    if (op == 3) {
        uint8_t *out = (uint8_t *)(uintptr_t)1; size_t size = 999;
        XrOsIoStatus status = xr_os_io_read_regular_file(policy, raw_path, 3, &out, &size);
        if (status == XR_OS_IO_OK) { CHECK(size == 3 && !memcmp(out, "abc", 3)); policy->free(policy->context, out); }
        else CHECK(out == (uint8_t *)(uintptr_t)1 && size == 999);
        return status;
    }
    XrDirIter *iterator = (XrDirIter *)(uintptr_t)1;
    XrOsIoStatus status = xr_os_io_dir_open(policy, fixture, &iterator);
    if (status != XR_OS_IO_OK) { CHECK(iterator == (XrDirIter *)(uintptr_t)1); return status; }
    XrDirEntry entry, saved; memset(&entry, 0xA5, sizeof(entry));
    do { saved = entry; status = xr_os_io_dir_next(iterator, &entry); } while (status == XR_OS_IO_OK);
    CHECK(!memcmp(&entry, &saved, sizeof(entry))); xr_os_io_dir_close(iterator);
    return status == XR_OS_IO_END ? XR_OS_IO_OK : status;
}
static void os_failures(void) {
    for (unsigned op = 0; op < 7; ++op) {
        XrCompileResources *resources = ledger(unlimited); XrOsIoPolicy policy = xr_compile_io_policy(resources);
        size_t begin = calls; CHECK(os_operation(op, &policy) == XR_OS_IO_OK); size_t points = calls - begin;
        uint64_t work = statistics(resources).work; released(resources);
        for (size_t i = 0; i < points; ++i) {
            resources = ledger(unlimited); policy = xr_compile_io_policy(resources); fail_at = calls + i;
            CHECK(os_operation(op, &policy) == XR_OS_IO_OUT_OF_MEMORY); fail_at = SIZE_MAX; released(resources); ++oom_points;
        }
        /* Each finite prefix of these small OS/path walks must stop safely. */
        for (uint64_t cap = 1; cap < work; ++cap) {
            XrCompileResourceLimits limits = unlimited; limits.work = cap;
            resources = ledger(limits); policy = xr_compile_io_policy(resources);
            CHECK(os_operation(op, &policy) == XR_OS_IO_BUDGET); released(resources); ++budget_cases;
        }
        printf("OS operation %u: %zu malloc points, %llu complete work cutoffs\n", op, points, (unsigned long long)(work - 1));
    }
    XrCompileResources *resources = ledger(unlimited); XrOsIoPolicy policy = xr_compile_io_policy(resources); bool value = true;
    CHECK(xr_os_io_is_dir(&policy, raw_path, &value) == XR_OS_IO_OK && !value);
    value = true; CHECK(xr_os_io_exists(&policy, "missing-cache-resource-file", &value) == XR_OS_IO_NOT_FOUND && value);
    xr_compile_resources_release(resources); CHECK(!live && !physical && !handles);
}
static XrCacheVerifyStatus callback_status;
static XrCacheVerifyStatus rejected(XrCompileResources *resources, XrCacheArtifactKind kind,
    XrCacheKey id, const uint8_t *bytes, size_t size, void *context) {
    CHECK(accepted(resources, kind, id, bytes, size, context) == XR_CACHE_VERIFY_OK);
    if (callback_status == XR_CACHE_VERIFY_OUT_OF_MEMORY) {
        void *block = NULL; fail_at = calls;
        CHECK(xr_compile_resources_alloc(resources, 8, &block) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY); fail_at = SIZE_MAX;
    } else if (callback_status == XR_CACHE_VERIFY_BUDGET)
        CHECK(xr_compile_resources_work(resources, UINT64_MAX) == XR_COMPILE_RESOURCE_BUDGET);
    return callback_status;
}
static void callbacks(void) {
    static const XrCacheVerifyStatus inputs[] = {XR_CACHE_VERIFY_BAD_ARGUMENT, XR_CACHE_VERIFY_IO, XR_CACHE_VERIFY_OUT_OF_MEMORY, XR_CACHE_VERIFY_BUDGET};
    static const XrCacheLoadStatus expected[] = {XR_CACHE_LOAD_BAD_ARGUMENT, XR_CACHE_LOAD_IO_ERROR, XR_CACHE_LOAD_OUT_OF_MEMORY, XR_CACHE_LOAD_BUDGET};
    static const XrCachePublishStatus publish_expected[] = {XR_CACHE_PUBLISH_BAD_ARGUMENT, XR_CACHE_PUBLISH_IO_ERROR, XR_CACHE_PUBLISH_OUT_OF_MEMORY, XR_CACHE_PUBLISH_BUDGET};
    for (size_t i = 0; i < 4; ++i) {
        XrCompileResources *resources = ledger(unlimited); XrCacheStore *store = opened(resources);
        callback_status = inputs[i]; XrCacheBlob out = poison_blob(), saved = out;
        CHECK(xr_compile_cache_store_load(store, XR_CACHE_ARTIFACT_XSM, key(1), rejected, resources, &out) == expected[i]);
        CHECK(!memcmp(&out, &saved, sizeof(out))); xr_compile_cache_store_close(store); released(resources); CHECK(disk_exists(1));
        resources = ledger(unlimited); store = opened(resources);
        CHECK(xr_compile_cache_store_publish(store, XR_CACHE_ARTIFACT_XSM, key(200), (const uint8_t *)"abc", 3, rejected, resources) == publish_expected[i]);
        xr_compile_cache_store_close(store); released(resources); CHECK(!disk_exists(200));
    }
    XrCompileResources *resources = ledger(unlimited); XrCacheStore *store = opened(resources);
    callback_status = XR_CACHE_VERIFY_REJECTED; XrCacheBlob out = poison_blob(), saved = out;
    CHECK(xr_compile_cache_store_load(store, XR_CACHE_ARTIFACT_XSM, key(18), rejected, resources, &out) == XR_CACHE_LOAD_REJECTED);
    CHECK(!memcmp(&out, &saved, sizeof(out))); CHECK(!disk_exists(18) && disk_exists(1));
    CHECK(xr_compile_cache_store_publish(store, XR_CACHE_ARTIFACT_XSM, key(18), (const uint8_t *)"abc", 3, accepted, resources) == XR_CACHE_PUBLISH_OK);
    xr_compile_cache_store_close(store); released(resources);
}
static void os_errors(void) {
#ifdef XR_OS_WINDOWS
    XrCompileResources *resources = ledger(unlimited); XrOsIoPolicy policy = xr_compile_io_policy(resources);
    uint8_t *bytes = (uint8_t *)(uintptr_t)1; size_t size = 99; fail_read = true;
    CHECK(xr_os_io_read_regular_file(&policy, raw_path, 3, &bytes, &size) == XR_OS_IO_IO);
    CHECK(bytes == (uint8_t *)(uintptr_t)1 && size == 99); fail_read = false;
    XrDirIter *iterator = NULL; CHECK(xr_os_io_dir_open(&policy, fixture, &iterator) == XR_OS_IO_OK);
    XrDirEntry output, saved; memset(&output, 0xA5, sizeof(output)); saved = output; fail_next = true;
    XrOsIoStatus status;
    do { saved = output; status = xr_os_io_dir_next(iterator, &output); } while (status == XR_OS_IO_OK);
    CHECK(status == XR_OS_IO_IO && !memcmp(&output, &saved, sizeof(output))); fail_next = false;
    CHECK(xr_os_io_dir_next(iterator, &output) == XR_OS_IO_IO); xr_os_io_dir_close(iterator); released(resources);
    resources = ledger(unlimited); XrCacheStore *store = opened(resources); XrCacheBlob out = poison_blob(), copy = out;
    fail_read = true; CHECK(xr_compile_cache_store_load(store, XR_CACHE_ARTIFACT_XSM, key(1), accepted, resources, &out) == XR_CACHE_LOAD_IO_ERROR);
    fail_read = false; CHECK(!memcmp(&out, &copy, sizeof(out))); CHECK(disk_exists(1));
    XrCacheCollectStats stats, before; memset(&stats, 0xA5, sizeof(stats)); before = stats; fail_next = true;
    CHECK(xr_compile_cache_store_collect(store, &stats) == XR_COMPILE_CACHE_IO); fail_next = false;
    CHECK(!memcmp(&stats, &before, sizeof(stats))); xr_compile_cache_store_close(store); released(resources);
#endif
}
static void probe_cases(void) {
    XrCompileResources *resources = ledger(unlimited); XrOsIoPolicy policy = xr_compile_io_policy(resources);
    CHECK(xr_file_probe_owned(NULL, raw_path, true) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(xr_file_probe_owned(&policy, NULL, true) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(xr_file_probe_owned(&policy, "", true) == XR_OS_IO_BAD_ARGUMENT);
    for (unsigned links = 0; links < 2; ++links) {
        CHECK(xr_file_probe_owned(&policy, raw_path, links != 0) == XR_OS_IO_OK);
        CHECK(xr_file_probe_owned(&policy, fixture, links != 0) == XR_OS_IO_BAD_ARGUMENT);
        CHECK(xr_file_probe_owned(&policy, "missing-cache-resource-file", links != 0) == XR_OS_IO_NOT_FOUND);
#ifdef XR_OS_WINDOWS
        CHECK(xr_file_probe_owned(&policy, "\xC0\x80", links != 0) == XR_OS_IO_BAD_ARGUMENT);
        /* Native leaf metadata branch matrix; the injected attributes model
         * Win32 reparse/device results without requiring symlink privilege. */
        probe_attributes = FILE_ATTRIBUTE_REPARSE_POINT;
        CHECK(xr_file_probe_owned(&policy, raw_path, links != 0) == (links ? XR_OS_IO_OK : XR_OS_IO_BAD_ARGUMENT));
        probe_attributes |= FILE_ATTRIBUTE_DIRECTORY;
        CHECK(xr_file_probe_owned(&policy, raw_path, links != 0) == XR_OS_IO_BAD_ARGUMENT);
        probe_attributes = FILE_ATTRIBUTE_DEVICE;
        CHECK(xr_file_probe_owned(&policy, raw_path, links != 0) == XR_OS_IO_BAD_ARGUMENT);
        probe_attributes = 0;
        static const struct { DWORD error; XrOsIoStatus status; } errors[] = {
            {ERROR_ACCESS_DENIED, XR_OS_IO_IO}, {ERROR_FILE_NOT_FOUND, XR_OS_IO_NOT_FOUND},
            {ERROR_NOT_ENOUGH_MEMORY, XR_OS_IO_OUT_OF_MEMORY}, {ERROR_OUTOFMEMORY, XR_OS_IO_OUT_OF_MEMORY},
            {ERROR_FILENAME_EXCED_RANGE, XR_OS_IO_BUDGET}, {ERROR_INVALID_NAME, XR_OS_IO_BAD_ARGUMENT}
        };
        for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
            probe_error = errors[i].error;
            CHECK(xr_file_probe_owned(&policy, raw_path, links != 0) == errors[i].status);
        }
        probe_error = 0;
#endif
    }
    released(resources);
#ifdef XR_OS_WINDOWS
    const uint64_t n = strlen(raw_path), allocated = sizeof(OracleLedger) + sizeof(OracleHeader) + 2 * (n + 1);
    const uint64_t work = 5 * n + 8;
    for (unsigned links = 0; links < 2; ++links) for (unsigned dimension = 0; dimension < 3; ++dimension)
        for (unsigned minus = 0; minus < 2; ++minus) {
            XrCompileResourceLimits limits = unlimited;
            if (!dimension) limits.allocated_bytes = allocated - minus;
            if (dimension == 1) limits.live_bytes = allocated - minus;
            if (dimension == 2) limits.work = work - minus;
            resources = ledger(limits); policy = xr_compile_io_policy(resources);
            CHECK(xr_file_probe_owned(&policy, raw_path, links != 0) == (minus ? XR_OS_IO_BUDGET : XR_OS_IO_OK));
            if (!minus) {
                XrCompileResourceStats stats = statistics(resources);
                CHECK(stats.work == work && stats.allocated_bytes == allocated && stats.peak_bytes == allocated);
            }
            released(resources); ++formula_cases;
        }
#endif
}
static void unicode_path(void) {
    char *path = system_join(fixture, "\xE6\xB5\x8B\xE8\xAF\x95 space");
    CHECK(xr_os_io_write_new_file_sync(&system_policy, path, (const uint8_t *)"abc", 3) == XR_OS_IO_OK);
    XrCompileResources *resources = ledger(unlimited); XrOsIoPolicy policy = xr_compile_io_policy(resources);
    uint8_t *bytes = NULL; size_t size = 0;
    CHECK(xr_os_io_read_regular_file(&policy, path, 3, &bytes, &size) == XR_OS_IO_OK && size == 3 && !memcmp(bytes, "abc", 3));
    xr_compile_resources_free(bytes); released(resources);
    CHECK(xr_os_io_remove(&system_policy, path) == XR_OS_IO_OK); system_free(path);
}
static void bad_store_arguments(void) {
    XrCompileResources *resources = ledger(unlimited); XrCacheStore *out = (XrCacheStore *)(uintptr_t)1;
    CHECK(xr_compile_cache_store_open(NULL, &config, &out) == XR_COMPILE_CACHE_BAD_ARGUMENT);
    CHECK(out == (XrCacheStore *)(uintptr_t)1);
#ifdef XR_OS_WINDOWS
    XrCacheStoreConfig invalid = config; invalid.root = "\xC0\x80";
    CHECK(xr_compile_cache_store_open(resources, &invalid, &out) == XR_COMPILE_CACHE_BAD_ARGUMENT);
    CHECK(out == (XrCacheStore *)(uintptr_t)1);
#endif
    released(resources);
}
static void quota_order(void) {
    char *directory = system_join(fixture, "xsm"); CHECK(clean_directory(directory, true, true) == 18); system_free(directory);
    /* Equal mtime forces the lexical tie-breaker. Hex filenames 01..08 are
     * evicted first; the ten remaining independent 99-byte objects total 990. */
    uint64_t quota = config.quota_bytes; config.quota_bytes = 990;
    XrCompileResources *resources = ledger(unlimited); XrCacheStore *store = opened(resources); XrCacheCollectStats stats;
    CHECK(xr_compile_cache_store_collect(store, &stats) == XR_COMPILE_CACHE_OK);
    CHECK(stats.live_entries == 10 && stats.live_bytes == 990 && !stats.removed_entries);
    for (unsigned i = 1; i <= 18; ++i) CHECK(disk_exists(i) == (i >= 9));
    xr_compile_cache_store_close(store); released(resources); config.quota_bytes = quota;
}
static void fixture_remove(void) {
    const char *names[] = {"xsm", "xtp"};
    for (size_t i = 0; i < 2; ++i) {
        char *directory = system_join(fixture, names[i]); (void)clean_directory(directory, true, false);
#ifdef XR_OS_WINDOWS
        CHECK(RemoveDirectoryA(directory));
#else
        CHECK(!rmdir(directory));
#endif
        system_free(directory);
    }
    CHECK(xr_os_io_remove(&system_policy, raw_path) == XR_OS_IO_OK);
    char *lock = system_join(fixture, ".cache-root.lock"); CHECK(xr_os_io_remove(&system_policy, lock) == XR_OS_IO_OK); system_free(lock);
#ifdef XR_OS_WINDOWS
    CHECK(RemoveDirectoryA(fixture));
#else
    CHECK(!rmdir(fixture));
#endif
}
int main(void) {
    system_policy = xr_os_io_system_policy(); paths(); join_formula(); fixture_create();
    bad_store_arguments(); read_formula(); entry_formula(); probe_cases(); unicode_path(); owner_lifetime(); os_errors(); callbacks(); failures(); work_failures(); os_failures(); quota_order(); fixture_remove();
    CHECK(!live && !physical && !handles);
    printf("cache resources passed: %zu formula boundaries, %zu actual malloc OOM points, %zu work cutoffs, %zu temporary recovery cases; compiler heap/FS handles zero\n",
        formula_cases, oom_points, budget_cases, residual_cases); return 0;
}
