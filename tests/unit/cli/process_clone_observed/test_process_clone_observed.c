/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_process_clone_observed.c - Frozen provenance and independent observer ownership
 */
#include "base/xmalloc.h"
#include "app/toolchain/xtc_process.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s (win=%lu)\n", __LINE__, #c, GetLastError()); exit(1); } } while (0)

#if defined(CLONE_INJECTED)
typedef struct Allocation { void *pointer; size_t size; } Allocation;
static Allocation allocations[4096];
static size_t attempts, fail_at = SIZE_MAX, physical, blocks;
static void *clone_alloc(size_t size) {
    if (attempts++ == fail_at) return NULL;
    void *pointer = xr_malloc(size); CHECK(pointer);
    for (unsigned i = 0; i < 4096; ++i) if (!allocations[i].pointer) {
        allocations[i] = (Allocation){pointer, size}; physical += size; ++blocks; return pointer;
    }
    CHECK(false); return NULL;
}
static void clone_free(void *pointer) {
    if (!pointer) return;
    for (unsigned i = 0; i < 4096; ++i) if (allocations[i].pointer == pointer) {
        physical -= allocations[i].size; --blocks; allocations[i] = (Allocation){0}; xr_free(pointer); return;
    }
    CHECK(false);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc clone_alloc
#define xr_free clone_free
#include "base/xcompile_resources.c"
static unsigned clone_capture_calls, conversions, cwd_queries;
static LPWCH clone_environment(void) { ++clone_capture_calls; return GetEnvironmentStringsW(); }
static DWORD clone_cwd(DWORD size, LPWSTR path) { ++cwd_queries; return GetCurrentDirectoryW(size, path); }
static int clone_wide(UINT page, DWORD flags, LPCCH input, int length, LPWSTR output, int capacity) {
    ++conversions; return MultiByteToWideChar(page, flags, input, length, output, capacity);
}
#define GetEnvironmentStringsW clone_environment
#define GetCurrentDirectoryW clone_cwd
#define MultiByteToWideChar clone_wide
#include "app/toolchain/xtc_process.c"
#undef GetEnvironmentStringsW
#undef GetCurrentDirectoryW
#undef MultiByteToWideChar
#undef xr_malloc
#undef xr_free
#endif

static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
static char executable[32768], original_cwd[32768];
static wchar_t original_directory[32768], first_environment[65536], second_environment[65536];
static unsigned old_events, new_events;
static XrOsProcStatus old_observe(void *context, const XrProcImageEvent *event) {
    (void)context; (void)event; ++old_events; return XR_PROC_IO;
}
static XrOsProcStatus new_observe(void *context, const XrProcImageEvent *event) {
    CHECK(context == &new_events && event && event->file_handle);
    DWORD flags = 0; CHECK(GetHandleInformation((HANDLE)event->file_handle, &flags));
    ++new_events; return XR_PROC_OK;
}
static XrProcImageObserver observer = {&new_events, new_observe};
static XrCompileResourceStats stats(XrCompileResources *resources) {
    XrCompileResourceStats result; CHECK(xr_compile_resources_stats(resources, &result) == XR_COMPILE_RESOURCE_OK); return result;
}
static XrCompileResources *ledger(XrCompileResourceLimits limits) {
    XrCompileResources *resources = NULL; CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK); return resources;
}
static void physical_zero(void) {
#if defined(CLONE_INJECTED)
    CHECK(!physical && !blocks);
#endif
}
static void append_environment(wchar_t *block, size_t *used, const wchar_t *entry) {
    size_t length = wcslen(entry) + 1; CHECK(*used + length + 1 < 65536);
    memcpy(block + *used, entry, length * sizeof(wchar_t)); *used += length; block[*used] = 0;
}
static void make_environment(wchar_t *block, bool changed) {
    size_t used = 0; block[0] = 0;
    append_environment(block, &used, changed ? L"=C:=C:\\clone-new" : L"=C:=C:\\clone-old");
    append_environment(block, &used, changed ? L"=ExitCode=00000029" : L"=ExitCode=00000017");
    append_environment(block, &used, changed ? L"=XR_CLONE=hidden-new" : L"=XR_CLONE=hidden-old");
    append_environment(block, &used, changed ? L"CLONE_测试=after" : L"CLONE_测试=before");
    append_environment(block, &used, L"CLONE EMPTY=");
    const wchar_t *names[] = {L"PATH", L"SystemRoot", L"ASAN_OPTIONS", L"UBSAN_OPTIONS"};
    for (unsigned i = 0; i < 4; ++i) {
        wchar_t value[32768], entry[32832];
        DWORD length = GetEnvironmentVariableW(names[i], value, 32768);
        if (!length) continue;
        CHECK(length < 32768 && swprintf(entry, 32832, L"%ls=%ls", names[i], value) > 0);
        append_environment(block, &used, entry);
    }
}
static XrProcCompletionPolicy prepare_completion=XR_PROC_COMPLETE_TREE;
static XrToolchainProcess *prepare(XrCompileResources *resources, bool observed) {
    XrProcessSpec spec; xtc_process_spec_init(&spec, executable, 5000);
    spec.environment_source = XTC_PROCESS_ENV_SNAPSHOT;
    spec.completion_policy = prepare_completion;
    spec.argv[0] = "deliberately-not-the-executable"; spec.argv[1] = "--child";
    spec.argv[2] = original_cwd; spec.argv[3] = executable; spec.argv[4] = "text|中";
    spec.cwd = NULL; spec.output_limit = 4096;
    if (observed) { spec.image_mode = XR_PROC_IMAGES_WINDOWS_TREE; spec.image_observer = (XrProcImageObserver){NULL, old_observe}; }
    XrToolchainProcess *source = NULL; CHECK(xtc_process_prepare(resources, &spec, &source) == XTC_PROCESS_OK); return source;
}
static void same_text(const char *left, const char *right, bool distinct) {
    CHECK(left && right && !strcmp(left, right)); if (distinct) CHECK(left != right);
}
static void same_view(const XrToolchainProcess *left, const XrToolchainProcess *right, bool distinct) {
    XrProcessView a, b; CHECK(xtc_process_view(left, &a) == XTC_PROCESS_OK && xtc_process_view(right, &b) == XTC_PROCESS_OK);
    same_text(a.executable, b.executable, distinct); same_text(a.cwd, b.cwd, distinct);
    CHECK(a.argc == b.argc && a.env_count == b.env_count && a.timeout_ms == b.timeout_ms && a.output_limit == b.output_limit && a.completion_policy == b.completion_policy);
    for (size_t i = 0; i < a.argc; ++i) same_text(a.argv[i], b.argv[i], distinct);
    for (size_t i = 0; i < a.env_count; ++i) { same_text(a.env_keys[i], b.env_keys[i], distinct); same_text(a.env_values[i], b.env_values[i], distinct); }
#if defined(CLONE_INJECTED)
    for (size_t i = 0; i < a.env_count; ++i) {
        CHECK(left->wide_lengths[i] == right->wide_lengths[i]);
        CHECK(!memcmp(left->wide_keys[i], right->wide_keys[i], (size_t)left->wide_lengths[i] * sizeof(wchar_t)));
        if (distinct) CHECK(left->wide_keys[i] != right->wide_keys[i]);
    }
#endif
}
static void environment_value(const wchar_t *prefix, const wchar_t *value) {
    LPWCH block = GetEnvironmentStringsW(); CHECK(block); bool found = false;
    for (const wchar_t *p = block; *p; p += wcslen(p) + 1)
        if (!wcsncmp(p, prefix, wcslen(prefix))) { CHECK(!wcscmp(p + wcslen(prefix), value)); found = true; }
    CHECK(found && FreeEnvironmentStringsW(block));
}
static int child(int argc, char **argv) {
    CHECK(argc == 5 && !strcmp(argv[0], "deliberately-not-the-executable"));
    /* The narrow Windows CRT argv uses ACP; the process boundary uses UTF-16. */
    char argument[64]; CHECK(WideCharToMultiByte(CP_ACP, 0, L"text|中", -1, argument, sizeof(argument), NULL, NULL));
    CHECK(!strcmp(argv[4], argument) && wcsstr(GetCommandLineW(), L"text|中"));
    char path[32768], directory[32768]; CHECK(GetModuleFileNameA(NULL, path, 32768) && GetCurrentDirectoryA(32768, directory));
    CHECK(!strcmp(directory, argv[2]) && !strcmp(path, argv[3]));
    environment_value(L"=C:=", L"C:\\clone-old"); environment_value(L"=ExitCode=", L"00000017");
    environment_value(L"=XR_CLONE=", L"hidden-old"); environment_value(L"CLONE_测试=", L"before");
    environment_value(L"CLONE EMPTY=", L"");
    fputs("frozen-clone|hidden|unicode|cwd|argv0", stdout); return 0;
}
static void invalid_outputs(void) {
    XrCompileResources *resources = ledger(unlimited); XrToolchainProcess *source = prepare(resources, true);
    XrToolchainProcess *out = (XrToolchainProcess *)(uintptr_t)0x1234; XrCompileResourceStats before = stats(resources);
    CHECK(xtc_process_clone_observed(source, &observer, &out) == XTC_PROCESS_INVALID && out == (XrToolchainProcess *)(uintptr_t)0x1234);
    out = NULL; CHECK(xtc_process_clone_observed(NULL, &observer, &out) == XTC_PROCESS_INVALID && !out);
    CHECK(xtc_process_clone_observed(source, NULL, &out) == XTC_PROCESS_INVALID && !out);
    XrProcImageObserver empty = {0}; CHECK(xtc_process_clone_observed(source, &empty, &out) == XTC_PROCESS_INVALID && !out);
    CHECK(xtc_process_clone_observed(source, &observer, NULL) == XTC_PROCESS_INVALID);
    XrCompileResourceStats after = stats(resources); CHECK(!memcmp(&before, &after, sizeof(before)));
    xtc_process_free(source); xr_compile_resources_release(resources); physical_zero();
}
static void run_lifetime(bool observed) {
    const XrCompileResourceLimits finite={64 * 1024 * 1024,8 * 1024 * 1024,128000000};
    XrCompileResources *resources = ledger(prepare_completion==XR_PROC_COMPLETE_ROOT ? finite : unlimited);
    XrToolchainProcess *source = prepare(resources, observed), *copy = NULL;
    CHECK(SetEnvironmentStringsW(second_environment)); wchar_t changed_directory[32768];
    CHECK(GetWindowsDirectoryW(changed_directory, 32768) && SetCurrentDirectoryW(changed_directory));
#if defined(CLONE_INJECTED)
    unsigned before_clone_capture_calls = clone_capture_calls, before_conversions = conversions, before_cwd = cwd_queries;
#endif
    CHECK(xtc_process_clone_observed(source, &observer, &copy) == XTC_PROCESS_OK);
#if defined(CLONE_INJECTED)
    CHECK(clone_capture_calls == before_clone_capture_calls && conversions == before_conversions && cwd_queries == before_cwd);
#endif
    if (observed) { CHECK(xtc_process_resources(copy) == resources); same_view(source, copy, true); }
    xtc_process_free(source); xr_compile_resources_release(resources);
    CHECK(xtc_process_resources(copy) == resources);
    XrProcessView view; CHECK(xtc_process_view(copy, &view) == XTC_PROCESS_OK);
    CHECK(view.completion_policy == prepare_completion && view.image_mode == XR_PROC_IMAGES_WINDOWS_TREE && !strcmp(view.executable, executable) && strcmp(view.executable, view.argv[0]));
    XrProcessResult result = {0}; unsigned before_events = new_events;
    CHECK(xtc_process_run(copy, NULL, NULL, &result) == XTC_PROCESS_OK);
    if (result.exit_code || result.stderr_bytes.length) {
        fprintf(stderr, "child exit=%d stderr=%zu: ", result.exit_code, result.stderr_bytes.length);
        if (result.stderr_bytes.length) fwrite(result.stderr_bytes.data, 1, result.stderr_bytes.length, stderr);
    }
    CHECK(result.exit_code == 0 && !result.stderr_bytes.length && !result.stdout_bytes.truncated && !result.stderr_bytes.truncated);
    const char expected[] = "frozen-clone|hidden|unicode|cwd|argv0";
    CHECK(result.stdout_bytes.length == sizeof(expected) - 1 && !memcmp(result.stdout_bytes.data, expected, sizeof(expected) - 1));
    CHECK(new_events > before_events && !old_events);
    xtc_process_free(copy); CHECK(!memcmp(result.stdout_bytes.data, expected, sizeof(expected) - 1));
    xtc_process_result_free(&result); physical_zero();
    CHECK(SetCurrentDirectoryW(original_directory) && SetEnvironmentStringsW(first_environment));
    printf("real clone from %s: %u new image callbacks; source/ledger died before run PASS\n", observed ? "observed" : "plain", new_events - before_events);
}

#if defined(CLONE_INJECTED)
typedef struct Expected { uint64_t work, bytes, allocations; uint64_t ends[8192]; size_t count; } Expected;
static void operation(Expected *e, uint64_t cost) { e->work += cost; CHECK(e->count < 8192); e->ends[e->count++] = e->work; }
static void allocation(Expected *e, size_t size) { e->bytes += sizeof(CompileAllocation) + size; ++e->allocations; operation(e, 1); }
static void text_operations(Expected *e, const char *text) {
    size_t bytes = strlen(text) + 1; for (size_t i = 0; i < bytes; ++i) operation(e, 1);
    allocation(e, bytes); operation(e, bytes);
}
static Expected expected_clone(const XrToolchainProcess *source) {
    Expected e = {0}; e.bytes = sizeof(CompileAllocation) + sizeof(XrToolchainProcess); e.allocations = 1;
    operation(&e, 1 + sizeof(XrToolchainProcess));
    operation(&e, sizeof(XrCompileResources *) + 3 * sizeof(size_t) + sizeof(uint32_t) + sizeof(XrProcImageMode) + sizeof(XrProcImageObserver) + sizeof(XrProcCompletionPolicy));
    text_operations(&e, source->spec.executable); text_operations(&e, source->spec.cwd);
    for (size_t i = 0; i < source->argc; ++i) text_operations(&e, source->spec.argv[i]);
    for (size_t i = 0; i < source->spec.env_count; ++i) {
        text_operations(&e, source->spec.env_keys[i]); text_operations(&e, source->spec.env_values[i]);
        operation(&e, sizeof(int)); size_t bytes = (wcslen(source->wide_keys[i]) + 1) * sizeof(wchar_t);
        allocation(&e, bytes); operation(&e, bytes);
    }
    return e;
}
static void allocation_and_work(void) {
    XrCompileResources *resources = ledger(unlimited); XrToolchainProcess *source = prepare(resources, true), *copy = NULL;
    Expected e = expected_clone(source); XrCompileResourceStats base = stats(resources); size_t start = attempts;
    CHECK(xtc_process_clone_observed(source, &observer, &copy) == XTC_PROCESS_OK);
    XrCompileResourceStats after = stats(resources);
    CHECK(after.work - base.work == e.work && after.allocated_bytes - base.allocated_bytes == e.bytes);
    CHECK(after.live_bytes - base.live_bytes == e.bytes && after.allocation_count - base.allocation_count == e.allocations);
    CHECK(attempts - start == e.allocations); same_view(source, copy, true);
    xtc_process_free(copy); copy = NULL; CHECK(stats(resources).live_bytes == base.live_bytes);
    for (size_t point = 0; point < e.allocations; ++point) {
        XrToolchainProcess unchanged = *source; uint64_t work = stats(resources).work;
        fail_at = attempts + point;
        CHECK(xtc_process_clone_observed(source, &observer, &copy) == XTC_PROCESS_OUT_OF_MEMORY && !copy);
        CHECK(!memcmp(source, &unchanged, sizeof(unchanged)) && stats(resources).live_bytes == base.live_bytes);
        CHECK(stats(resources).work > work); fail_at = SIZE_MAX;
    }
    xtc_process_free(source); xr_compile_resources_release(resources); physical_zero();
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = unlimited;
        if (axis == 0) limits.allocated_bytes = base.allocated_bytes + e.bytes - minus;
        if (axis == 1) limits.live_bytes = base.live_bytes + e.bytes - minus;
        if (axis == 2) limits.work = base.work + e.work - minus;
        resources = ledger(limits); source = prepare(resources, true);
        CHECK(xtc_process_clone_observed(source, &observer, &copy) == (minus ? XTC_PROCESS_BUDGET : XTC_PROCESS_OK));
        if (minus) { CHECK(!copy); CHECK(xtc_process_clone_observed(source, &observer, &copy) == XTC_PROCESS_BUDGET && !copy); }
        else { CHECK(copy); xtc_process_free(copy); copy = NULL; }
        xtc_process_free(source); xr_compile_resources_release(resources); physical_zero();
    }
    for (size_t i = 0; i < e.count; ++i) {
        XrCompileResourceLimits limits = unlimited; limits.work = base.work + e.ends[i] - 1;
        resources = ledger(limits); source = prepare(resources, true);
        CHECK(xtc_process_clone_observed(source, &observer, &copy) == XTC_PROCESS_BUDGET && !copy);
        CHECK(stats(resources).live_bytes == base.live_bytes && stats(resources).work <= limits.work);
        xtc_process_free(source); xr_compile_resources_release(resources); physical_zero();
    }
    printf("clone independent formula: allocations=%llu bytes=%llu work=%llu; %zu charge boundaries; OOM/three-axis/physical-zero PASS\n",
        (unsigned long long)e.allocations, (unsigned long long)e.bytes, (unsigned long long)e.work, e.count);
}
static void read_boundary(void) {
    SYSTEM_INFO info; GetSystemInfo(&info); size_t page = info.dwPageSize;
    char *region = VirtualAlloc(NULL, 2 * page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); CHECK(region);
    DWORD previous = 0; CHECK(VirtualProtect(region + page, page, PAGE_NOACCESS, &previous)); region[page - 1] = 'x';
    XrCompileResourceLimits limits = unlimited; limits.work = 2; XrCompileResources *resources = ledger(limits);
    const char *out = NULL; CHECK(process_clone_text(resources, region + page - 1, &out) == XTC_PROCESS_BUDGET && !out);
    CHECK(stats(resources).work == 2); xr_compile_resources_release(resources); physical_zero();
    CHECK(VirtualFree(region, 0, MEM_RELEASE));
}
#endif

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--child")) return child(argc, argv);
    CHECK(GetModuleFileNameA(NULL, executable, sizeof(executable)) && GetCurrentDirectoryA(sizeof(original_cwd), original_cwd));
    CHECK(GetCurrentDirectoryW(32768, original_directory)); LPWCH previous_environment = GetEnvironmentStringsW(); CHECK(previous_environment);
    make_environment(first_environment, false); make_environment(second_environment, true);
    CHECK(SetEnvironmentStringsW(first_environment)); invalid_outputs();
#if defined(CLONE_INJECTED)
    allocation_and_work(); read_boundary();
#endif
    run_lifetime(true); run_lifetime(false);
    prepare_completion=XR_PROC_COMPLETE_ROOT; run_lifetime(true); run_lifetime(false);
    prepare_completion=XR_PROC_COMPLETE_TREE;
    CHECK(SetEnvironmentStringsW(previous_environment) && FreeEnvironmentStringsW(previous_environment));
    physical_zero(); puts("observed clone frozen owner PASS"); return 0;
}
