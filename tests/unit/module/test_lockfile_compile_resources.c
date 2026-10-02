/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lockfile_compile_resources.c - Mandatory shared resources for package metadata
 */
#include "base/xmalloc.h"
#include "base/xcompile_resources.h"
#include "base/xio_policy.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[256];
static size_t calls, live, physical, fail_at = SIZE_MAX;
static void *counted_malloc(size_t bytes) {
    if (calls++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes); if (!memory) return NULL;
    for (size_t i = 0; i < 256; ++i) if (!allocations[i].pointer) {
        allocations[i] = (Allocation){memory, bytes}; ++live; physical += bytes; return memory;
    }
    CHECK(false); return NULL;
}
static void counted_free(void *memory) {
    if (!memory) return;
    for (size_t i = 0; i < 256; ++i) if (allocations[i].pointer == memory) {
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

/* Independent layout arithmetic for the two-pointer allocation prefix and
 * aligned ledger root; never derive expected work from verifier statistics. */
typedef union OracleAlignment {
    long double floating; uint64_t integer; void *pointer;
} OracleAlignment;
typedef union OracleHeader {
    struct { void *owner; size_t bytes; } fields;
#ifdef XR_COMPILER_MSVC
    OracleAlignment alignment;
#else
    max_align_t alignment;
#endif
} OracleHeader;
typedef struct OracleLedger {
    uint64_t limits[3], stats[5], references;
    atomic_bool locked;
} OracleLedger;
static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
static XrCompileResources *ledger(XrCompileResourceLimits limits) {
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    return resources;
}
static void release(XrCompileResources *resources) {
    xr_compile_resources_release(resources); CHECK(!live && !physical);
}
#include "module/xlockfile.h"

#include "os/os_dir.h"
#ifdef XR_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
static size_t handles, collision_files;
static bool force_collision, fail_rename, fail_read, fail_write;
static HANDLE WINAPI tracked_open(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
    DWORD creation, DWORD flags, HANDLE template_file) {
    if (force_collision && creation == CREATE_NEW) {
        HANDLE collision = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(collision != INVALID_HANDLE_VALUE); DWORD written = 0;
        CHECK(WriteFile(collision, "collision", 9, &written, NULL) && written == 9); CHECK(CloseHandle(collision)); ++collision_files;
        SetLastError(ERROR_FILE_EXISTS); return INVALID_HANDLE_VALUE;
    }
    HANDLE result = CreateFileW(path, access, share, security, creation, flags, template_file);
    if (result != INVALID_HANDLE_VALUE) ++handles;
    return result;
}
static BOOL WINAPI tracked_close(HANDLE handle) { BOOL ok = CloseHandle(handle); CHECK(ok && handles); --handles; return ok; }
static BOOL WINAPI checked_rename(LPCWSTR from, LPCWSTR to, DWORD flags) {
    if (fail_rename) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return MoveFileExW(from, to, flags);
}
static BOOL WINAPI checked_read(HANDLE handle, LPVOID buffer, DWORD count, LPDWORD read, LPOVERLAPPED overlapped) {
    if (fail_read) { SetLastError(ERROR_READ_FAULT); return FALSE; }
    return ReadFile(handle, buffer, count, read, overlapped);
}
static BOOL WINAPI checked_write(HANDLE handle, LPCVOID buffer, DWORD count, LPDWORD written, LPOVERLAPPED overlapped) {
    if (fail_write) { SetLastError(ERROR_WRITE_FAULT); return FALSE; }
    return WriteFile(handle, buffer, count, written, overlapped);
}
#define CreateFileW tracked_open
#define CloseHandle tracked_close
#define MoveFileExW checked_rename
#define ReadFile checked_read
#define WriteFile checked_write
#include "os/win/fs_win.c"
#undef CreateFileW
#undef CloseHandle
#undef MoveFileExW
#undef ReadFile
#undef WriteFile
#else
#include <unistd.h>
static size_t handles;
#include "os/unix/fs_unix.c"
#endif
#include "os/os_proc.h"
static bool fail_pid;
static int64_t checked_pid(void) { return fail_pid ? 0 : xr_proc_self_pid(); }
#define xr_proc_self_pid checked_pid
#include "module/xlockfile.c"
#undef xr_proc_self_pid
static XrOsIoPolicy system_policy;
static char directory[96], archive[128], path[128];
static size_t formulas, oom_points, work_cutoffs, residual_cases;
static const char expected_checksum[] = "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
static const char expected_text[] =
    "# xray.lock - Auto-generated, do not edit manually\n# Format version: 1\n\n"
    "[package.a/b]\nversion = \"1.2.3\"\nresolved = \"url\"\nchecksum = \"sum\"\n"
    "dependencies = [\"c/d@^1\"]\n\n";
static XrCompileResourceStats statistics(XrCompileResources *resources) {
    XrCompileResourceStats stats; CHECK(xr_compile_resources_stats(resources, &stats) == XR_COMPILE_RESOURCE_OK); return stats;
}
static void released(XrCompileResources *resources) { release(resources); CHECK(!handles); }
static XrLockfile *fixture_lock(const XrOsIoPolicy *policy) {
    XrLockfile *lock = NULL; CHECK(xr_lockfile_new_owned(policy, &lock) == XR_OS_IO_OK);
    CHECK(xr_lockfile_add_package_owned(lock, "a/b", "1.2.3", "url", "sum") == XR_OS_IO_OK);
    CHECK(xr_lockfile_add_dependency_owned(lock, "a/b", "c/d@^1") == XR_OS_IO_OK); return lock;
}
static void write_fixture(const char *target, const char *text) {
    XrOsIoStatus removed = xr_os_io_remove(&system_policy, target);
    CHECK(removed == XR_OS_IO_OK || removed == XR_OS_IO_NOT_FOUND);
    CHECK(xr_os_io_write_new_file_sync(&system_policy, target, (const uint8_t *)text, strlen(text)) == XR_OS_IO_OK);
}
static void assert_text(const char *target, const char *expected) {
    uint8_t *bytes = NULL; size_t size = 0;
    CHECK(xr_os_io_read_regular_file(&system_policy, target, SIZE_MAX, &bytes, &size) == XR_OS_IO_OK);
    CHECK(size == strlen(expected) && !memcmp(bytes, expected, size)); system_policy.free(system_policy.context, bytes);
}
static size_t cleanup_temporaries(bool collisions) {
    XrDirIter *iterator = NULL; CHECK(xr_os_io_dir_open(&system_policy, directory, &iterator) == XR_OS_IO_OK);
    XrDirEntry entry; XrOsIoStatus status; size_t count = 0;
    while ((status = xr_os_io_dir_next(iterator, &entry)) == XR_OS_IO_OK) {
        if (strncmp(entry.name, "xray.lock.tmp-lock-", 19)) continue;
        char full[400]; CHECK(snprintf(full, sizeof(full), "%s/%s", directory, entry.name) > 0);
        assert_text(full, collisions ? "collision" : expected_text);
        CHECK(xr_os_io_remove(&system_policy, full) == XR_OS_IO_OK); ++count;
    }
    CHECK(status == XR_OS_IO_END); xr_os_io_dir_close(iterator); return count;
}
static void setup(void) {
    system_policy = xr_os_io_system_policy();
#ifdef XR_OS_WINDOWS
    unsigned long id = GetCurrentProcessId();
#else
    unsigned long id = (unsigned long)getpid();
#endif
    CHECK(snprintf(directory, sizeof(directory), "package-resources-%lu", id) > 0);
    CHECK(snprintf(archive, sizeof(archive), "%s/archive", directory) > 0);
    CHECK(snprintf(path, sizeof(path), "%s/xray.lock", directory) > 0);
    CHECK(xr_os_io_mkdir(&system_policy, directory, 0700) == XR_OS_IO_OK);
    write_fixture(archive, "abc"); write_fixture(path, expected_text);
}
static void constructors(void) {
    XrCompileResources *resources = ledger(unlimited); XrOsIoPolicy policy = xr_compile_io_policy(resources); XrLockfile *lock = fixture_lock(&policy);
    CHECK(xr_lockfile_save_owned(&policy, lock, path) == XR_OS_IO_OK); assert_text(path, expected_text);
    XrLockfile *loaded = NULL; CHECK(xr_lockfile_load_owned(&policy, path, &loaded) == XR_OS_IO_OK);
    CHECK(loaded->package_count == 1 && loaded->packages[0].dep_count == 1);
    CHECK(!strcmp(loaded->packages[0].dependencies[0], "c/d@^1"));
    const XrLockedPackage *out = (const XrLockedPackage *)(uintptr_t)1;
    CHECK(xr_lockfile_find_owned(&policy, loaded, "missing", &out) == XR_OS_IO_NOT_FOUND && out == (const XrLockedPackage *)(uintptr_t)1);
    bool has = true; CHECK(xr_lockfile_has_owned(&policy, loaded, "missing", &has) == XR_OS_IO_OK && !has);
    CHECK(xr_lockfile_add_package_owned(loaded, "a/b", "2", NULL, NULL) == XR_OS_IO_OK);
    CHECK(loaded->packages[0].dep_count == 1 && !strcmp(loaded->packages[0].version, "2"));
    CHECK(xr_lockfile_remove_owned(loaded, "a/b") == XR_OS_IO_OK && loaded->package_count == 0);
    xr_lockfile_free_owned(loaded); xr_lockfile_free_owned(lock); released(resources);
}
static void owner_formulas(void) {
    uint64_t bytes = sizeof(OracleLedger) + 2 * sizeof(OracleHeader) + sizeof(XrLockfile) + 16 * sizeof(XrLockedPackage);
    uint64_t work = 3 + sizeof(XrLockfile) + 16 * sizeof(XrLockedPackage) + sizeof(XrLockfile *);
    for (unsigned dimension = 0; dimension < 3; ++dimension) for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = unlimited;
        if (!dimension) limits.allocated_bytes = bytes - minus;
        if (dimension == 1) limits.live_bytes = bytes - minus;
        if (dimension == 2) limits.work = work - minus;
        XrCompileResources *resources = ledger(limits); XrOsIoPolicy policy = xr_compile_io_policy(resources); XrLockfile *output = (XrLockfile *)(uintptr_t)1;
        CHECK(xr_lockfile_new_owned(&policy, &output) == (minus ? XR_OS_IO_BUDGET : XR_OS_IO_OK));
        if (minus) CHECK(output == (XrLockfile *)(uintptr_t)1);
        else { XrCompileResourceStats stats = statistics(resources); CHECK(stats.work == work && stats.allocated_bytes == bytes && stats.peak_bytes == bytes); xr_lockfile_free_owned(output); }
        released(resources); ++formulas;
    }
    XrCompileResources *resources = ledger(unlimited); XrOsIoPolicy policy = xr_compile_io_policy(resources); XrLockfile *lock = fixture_lock(&policy);
    uint64_t prefix = statistics(resources).work; xr_lockfile_free_owned(lock); released(resources);
    for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = unlimited; limits.work = prefix + 13 + sizeof(void *) - minus;
        resources = ledger(limits); policy = xr_compile_io_policy(resources); lock = fixture_lock(&policy);
        const XrLockedPackage *output = (const XrLockedPackage *)(uintptr_t)1;
        CHECK(xr_lockfile_find_owned(&policy, lock, "a/b", &output) == (minus ? XR_OS_IO_BUDGET : XR_OS_IO_OK));
        if (minus) CHECK(output == (const XrLockedPackage *)(uintptr_t)1); else CHECK(output == &lock->packages[0]);
        xr_lockfile_free_owned(lock); released(resources); ++formulas;
    }
}
static void checksum_formula(bool verify) {
#ifdef XR_OS_WINDOWS
    uint64_t n = strlen(archive);
    uint64_t bytes = sizeof(OracleLedger) + 2 * sizeof(OracleHeader) + 2 * (n + 1) + 3;
    uint64_t peak = sizeof(OracleLedger) + sizeof(OracleHeader) + 2 * (n + 1);
    uint64_t work = 5 * n + (verify ? 445 : 228);
    for (unsigned dimension = 0; dimension < 3; ++dimension) for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = unlimited;
        if (!dimension) limits.allocated_bytes = bytes - minus;
        if (dimension == 1) limits.live_bytes = peak - minus;
        if (dimension == 2) limits.work = work - minus;
        XrCompileResources *resources = ledger(limits); XrOsIoPolicy policy = xr_compile_io_policy(resources);
        char checksum[72], saved[72]; memset(checksum, 0xA5, sizeof(checksum)); memcpy(saved, checksum, sizeof(saved)); bool same = false;
        XrOsIoStatus status = verify ? xr_lockfile_verify_checksum_owned(&policy, archive, expected_checksum, &same) :
            xr_lockfile_checksum_file_owned(&policy, archive, checksum, sizeof(checksum));
        CHECK(status == (minus ? XR_OS_IO_BUDGET : XR_OS_IO_OK));
        if (minus) CHECK(!same && !memcmp(checksum, saved, sizeof(checksum)));
        else {
            CHECK(verify ? same : !strcmp(checksum, expected_checksum)); XrCompileResourceStats stats = statistics(resources);
            CHECK(stats.work == work && stats.allocated_bytes == bytes && stats.peak_bytes == peak);
        }
        released(resources); ++formulas;
    }
#else
    (void)verify;
#endif
}
static XrOsIoStatus operation(unsigned op, const XrOsIoPolicy *policy, XrLockfile **lock) {
    if (!op) return xr_lockfile_new_owned(policy, lock);
    if (op == 1) return xr_lockfile_load_owned(policy, path, lock);
    if (op == 2) return xr_lockfile_save_owned(policy, *lock, path);
    if (op == 3) {
        char output[72], saved[72]; memset(output, 0xA5, sizeof(output)); memcpy(saved, output, sizeof(output));
        XrOsIoStatus status = xr_lockfile_checksum_file_owned(policy, archive, output, sizeof(output));
        if (status == XR_OS_IO_OK) CHECK(!strcmp(output, expected_checksum)); else CHECK(!memcmp(output, saved, sizeof(output)));
        return status;
    }
    if (op == 4) {
        bool output = false; XrOsIoStatus status = xr_lockfile_verify_checksum_owned(policy, archive, expected_checksum, &output);
        CHECK(output == (status == XR_OS_IO_OK)); return status;
    }
    if (op == 5 || op == 8) {
        int before = (*lock)->package_count;
        XrOsIoStatus status = xr_lockfile_add_package_owned(*lock, "new/pkg", "2.3.4", "url2", "sum2");
        CHECK((*lock)->package_count == before + (status == XR_OS_IO_OK)); return status;
    }
    if (op == 6) {
        XrOsIoStatus status = xr_lockfile_add_package_owned(*lock, "a/b", "2.3.4", "url2", "sum2");
        CHECK(!strcmp((*lock)->packages[0].version, status == XR_OS_IO_OK ? "2.3.4" : "1.2.3")); return status;
    }
    int before = (*lock)->packages[0].dep_count;
    XrOsIoStatus status = xr_lockfile_add_dependency_owned(*lock, "a/b", "e/f@1");
    CHECK((*lock)->packages[0].dep_count == before + (status == XR_OS_IO_OK)); return status;
}
static XrLockfile *operation_input(unsigned op, const XrOsIoPolicy *policy) {
    XrLockfile *lock = op == 2 || op >= 5 ? fixture_lock(policy) : NULL;
    if (op == 8) for (unsigned i = 0; i < 15; ++i) {
        char name[32]; CHECK(snprintf(name, sizeof(name), "pkg/%u", i) > 0);
        CHECK(xr_lockfile_add_package_owned(lock, name, "1", NULL, NULL) == XR_OS_IO_OK);
    }
    if (op == 9) for (unsigned i = 0; i < 3; ++i)
        CHECK(xr_lockfile_add_dependency_owned(lock, "a/b", "e/f@1") == XR_OS_IO_OK);
    return lock;
}
static void faults(void) {
    for (unsigned op = 0; op < 10; ++op) {
        write_fixture(path, op == 2 ? "original\n" : expected_text);
        XrCompileResources *resources = ledger(unlimited); XrOsIoPolicy policy = xr_compile_io_policy(resources); XrLockfile *lock = operation_input(op, &policy);
        size_t begin = calls; uint64_t prefix = statistics(resources).work;
        CHECK(operation(op, &policy, &lock) == XR_OS_IO_OK); size_t points = calls - begin; uint64_t work = statistics(resources).work - prefix;
        xr_lockfile_free_owned(lock); released(resources);
        for (size_t i = 0; i < points; ++i) {
            write_fixture(path, op == 2 ? "original\n" : expected_text);
            resources = ledger(unlimited); policy = xr_compile_io_policy(resources); lock = operation_input(op, &policy);
            if (op < 2) lock = (XrLockfile *)(uintptr_t)1;
            fail_at = calls + i; CHECK(operation(op, &policy, &lock) == XR_OS_IO_OUT_OF_MEMORY); fail_at = SIZE_MAX;
            if (op < 2) CHECK(lock == (XrLockfile *)(uintptr_t)1); else xr_lockfile_free_owned(lock);
            released(resources);
            if (op == 2) { assert_text(path, "original\n"); if (cleanup_temporaries(false)) ++residual_cases; }
            ++oom_points;
        }
        for (uint64_t offset = 0; offset < work; ++offset) {
            write_fixture(path, op == 2 ? "original\n" : expected_text);
            XrCompileResourceLimits limits = unlimited; limits.work = prefix + offset;
            resources = ledger(limits); policy = xr_compile_io_policy(resources); lock = operation_input(op, &policy);
            if (op < 2) lock = (XrLockfile *)(uintptr_t)1;
            CHECK(operation(op, &policy, &lock) == XR_OS_IO_BUDGET);
            if (op < 2) CHECK(lock == (XrLockfile *)(uintptr_t)1); else xr_lockfile_free_owned(lock);
            released(resources);
            if (op == 2) { assert_text(path, "original\n"); if (cleanup_temporaries(false)) ++residual_cases; }
            ++work_cutoffs;
        }
        printf("lock operation %u: %zu malloc points, %llu complete work cutoffs\n", op, points, (unsigned long long)work);
    }
    write_fixture(path, expected_text);
}
static void error_states(void) {
    XrCompileResources *resources = ledger(unlimited); XrOsIoPolicy policy = xr_compile_io_policy(resources); XrLockfile *lock = fixture_lock(&policy);
    bool same = true; CHECK(xr_lockfile_verify_checksum_owned(&policy, archive, "sha256:wrong", &same) == XR_OS_IO_OK && !same);
    same = true; CHECK(xr_lockfile_verify_checksum_owned(&policy, "missing-lockfile-archive", expected_checksum, &same) == XR_OS_IO_NOT_FOUND && same);
    char short_buffer[65], saved[65]; memset(short_buffer, 0xA5, sizeof(short_buffer)); memcpy(saved, short_buffer, sizeof(saved));
    CHECK(xr_lockfile_checksum_file_owned(&policy, archive, short_buffer, sizeof(short_buffer)) == XR_OS_IO_BUDGET && !memcmp(short_buffer, saved, sizeof(saved)));
#ifdef XR_OS_WINDOWS
    fail_read = true; same = true;
    CHECK(xr_lockfile_verify_checksum_owned(&policy, archive, expected_checksum, &same) == XR_OS_IO_IO && same); fail_read = false;
    write_fixture(path, "original\n"); fail_write = true; CHECK(xr_lockfile_save_owned(&policy, lock, path) == XR_OS_IO_IO); fail_write = false;
    assert_text(path, "original\n"); CHECK(cleanup_temporaries(false) == 0);
    fail_rename = true; CHECK(xr_lockfile_save_owned(&policy, lock, path) == XR_OS_IO_IO); fail_rename = false;
    assert_text(path, "original\n"); CHECK(cleanup_temporaries(false) == 0);
    fail_pid = true; CHECK(xr_lockfile_save_owned(&policy, lock, path) == XR_OS_IO_IO); fail_pid = false;
    assert_text(path, "original\n"); CHECK(cleanup_temporaries(false) == 0);
    uint_fast64_t sequence = atomic_load(&temporary_sequence); atomic_store(&temporary_sequence, UINT64_MAX);
    CHECK(xr_lockfile_save_owned(&policy, lock, path) == XR_OS_IO_BUDGET); atomic_store(&temporary_sequence, sequence);
    assert_text(path, "original\n"); CHECK(cleanup_temporaries(false) == 0);
    force_collision = true; CHECK(xr_lockfile_save_owned(&policy, lock, path) == XR_OS_IO_EXISTS); force_collision = false;
    CHECK(collision_files == 16); assert_text(path, "original\n"); CHECK(cleanup_temporaries(true) == 16);
#endif
    xr_lockfile_free_owned(lock); released(resources);
    static const char *const malformed[] = {"[package.a/b]\ndependencies = [???]\n", "[package.a/b]\nversion = \"unterminated\n", "[package.a/b]\ndependencies = [\"x\"\n"};
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
        write_fixture(path, malformed[i]); resources = ledger(unlimited); policy = xr_compile_io_policy(resources); lock = (XrLockfile *)(uintptr_t)1;
        CHECK(xr_lockfile_load_owned(&policy, path, &lock) == XR_OS_IO_BAD_ARGUMENT && lock == (XrLockfile *)(uintptr_t)1); released(resources);
    }
    write_fixture(path, expected_text);
}
static void lifetime_and_growth(void) {
    XrCompileResources *resources = ledger(unlimited); XrOsIoPolicy policy = xr_compile_io_policy(resources); XrLockfile *lock = fixture_lock(&policy);
    for (unsigned i = 0; i < 20; ++i) {
        char name[32]; CHECK(snprintf(name, sizeof(name), "pkg/%u", i) > 0);
        CHECK(xr_lockfile_add_package_owned(lock, name, "1", NULL, NULL) == XR_OS_IO_OK);
        CHECK(xr_lockfile_add_dependency_owned(lock, "a/b", name) == XR_OS_IO_OK);
    }
    CHECK(lock->package_count == 21 && lock->packages[0].dep_count == 21);
    xr_compile_resources_release(resources); CHECK(!strcmp(lock->packages[0].version, "1.2.3"));
    CHECK(!strcmp(lock->packages[20].name, "pkg/19")); xr_lockfile_free_owned(lock); CHECK(!live && !physical && !handles);
}
static XrOsIoStatus wrong_work(void *context, uint64_t units) { (void)context; (void)units; CHECK(false); return XR_OS_IO_IO; }
static XrOsIoStatus wrong_alloc(void *context, size_t bytes, void **out) { (void)context; (void)bytes; (void)out; CHECK(false); return XR_OS_IO_IO; }
static void wrong_free(void *context, void *memory) { (void)context; (void)memory; CHECK(false); }
static void policy_identity(void) {
    XrCompileResources *a = ledger(unlimited), *b = ledger(unlimited);
    XrOsIoPolicy ap = xr_compile_io_policy(a), bp = xr_compile_io_policy(b); XrLockfile *lock = fixture_lock(&ap);
    CHECK(xr_lockfile_uses_policy(lock, &ap) && !xr_lockfile_uses_policy(lock, &bp));
    for (unsigned i = 0; i < 4; ++i) {
        XrOsIoPolicy impostor = i ? ap : bp;
        if (i == 1) impostor.work = wrong_work;
        if (i == 2) impostor.alloc = wrong_alloc;
        if (i == 3) impostor.free = wrong_free;
        CHECK(!xr_lockfile_uses_policy(lock, &impostor));
        const XrLockedPackage *output = (const XrLockedPackage *)(uintptr_t)1; bool exists = true;
        CHECK(xr_lockfile_find_owned(&impostor, lock, "a/b", &output) == XR_OS_IO_BAD_ARGUMENT && output == (const XrLockedPackage *)(uintptr_t)1);
        CHECK(xr_lockfile_has_owned(&impostor, lock, "a/b", &exists) == XR_OS_IO_BAD_ARGUMENT && exists);
        CHECK(xr_lockfile_save_owned(&impostor, lock, path) == XR_OS_IO_BAD_ARGUMENT); assert_text(path, expected_text);
    }
    xr_lockfile_free_owned(lock); xr_compile_resources_release(a); released(b);
}
int main(void) {
    setup(); constructors(); policy_identity(); owner_formulas(); checksum_formula(false); checksum_formula(true); error_states(); faults(); lifetime_and_growth();
    CHECK(xr_os_io_remove(&system_policy, archive) == XR_OS_IO_OK); CHECK(xr_os_io_remove(&system_policy, path) == XR_OS_IO_OK);
#ifdef XR_OS_WINDOWS
    CHECK(RemoveDirectoryA(directory));
#else
    CHECK(!rmdir(directory));
#endif
    printf("lockfile resources passed: %zu formula boundaries, %zu malloc OOM points, %zu work cutoffs, %zu owned temporary residual cases; physical zero\n",
        formulas, oom_points, work_cutoffs, residual_cases); return 0;
}
