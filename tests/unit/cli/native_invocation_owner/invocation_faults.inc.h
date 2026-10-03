/* Test-only interposition around the unique production owner. Every allocation
 * failure reaches the actual ledger allocator. Dependency implementations and
 * real child commands remain unchanged. */
#include "app/toolchain/xtc_xir_file_lease.h"
#include "app/toolchain/xtc_xir_images.h"
#include "base/xwindows_utf8.h"
#include "os/os_fs.h"
#include <appmodel.h>
static size_t profile_os_calls, profile_os_failure = SIZE_MAX;
static DWORD profile_os_error = ERROR_ACCESS_DENIED;
static bool profile_packaged;
static bool profile_os_enter(void) {
    if (profile_os_calls++ != profile_os_failure) return true;
    SetLastError(profile_os_error); return false;
}
static UINT WINAPI profile_windows_directory(LPWSTR buffer, UINT size) {
    return profile_os_enter() ? GetWindowsDirectoryW(buffer, size) : 0;
}
static UINT WINAPI profile_system_directory(LPWSTR buffer, UINT size) {
    return profile_os_enter() ? GetSystemDirectoryW(buffer, size) : 0;
}
static DWORD WINAPI profile_dll_directory(DWORD size, LPWSTR buffer) {
    return profile_os_enter() ? GetDllDirectoryW(size, buffer) : 0;
}
static LONG WINAPI profile_package(UINT32 *size, PWSTR buffer) {
    if (!profile_os_enter()) return (LONG)profile_os_error;
    return profile_packaged ? ERROR_INSUFFICIENT_BUFFER : GetCurrentPackageFullName(size, buffer);
}
static size_t owner_compare_count, owner_preexecution_comparisons, owner_compare_fail = SIZE_MAX;
static size_t owner_allocation_count, owner_allocation_fail = SIZE_MAX;
static DWORD owner_compare_error = ERROR_ACCESS_DENIED;
static unsigned invocation_runs, invocation_process_fail = UINT_MAX;
static unsigned invocation_tamper;
static bool invocation_image_difference, profile_image_outside;
static const char *invocation_compiler;
static const char *invocation_other_source;
static unsigned owner_writes, owner_tamper_write = UINT_MAX;
static XrOsIoStatus owner_write(const XrOsIoPolicy *policy, const char *path, const void *bytes, size_t length) {
    XrOsIoStatus status = xr_os_io_write_new_file_sync(policy, path, bytes, length);
    if (owner_writes++ == owner_tamper_write && status == XR_OS_IO_OK) {
        CHECK(length); unsigned char *changed = malloc(length); CHECK(changed);
        memcpy(changed, bytes, length); changed[0] ^= 1;
        FILE *file = fopen(path, "wb"); CHECK(file);
        CHECK(fwrite(changed, 1, length, file) == length && !fclose(file)); free(changed);
    }
    return status;
}
static bool owner_allocation_begin(void) {
    bool fail = owner_allocation_count++ == owner_allocation_fail;
    if (fail) runtime_fail_at = runtime_attempts;
    return fail;
}
static XrCompileResourceStatus owner_alloc(XrCompileResources *resources, size_t bytes, void **out) {
    bool fail = owner_allocation_begin();
    XrCompileResourceStatus status = xr_compile_resources_alloc(resources, bytes, out);
    if (fail) { runtime_fail_at = SIZE_MAX; CHECK(status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY); }
    return status;
}
static XrCompileResourceStatus owner_calloc(XrCompileResources *resources, size_t count, size_t bytes, void **out) {
    bool fail = owner_allocation_begin();
    XrCompileResourceStatus status = xr_compile_resources_calloc(resources, count, bytes, out);
    if (fail) { runtime_fail_at = SIZE_MAX; CHECK(status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY); }
    return status;
}
static XrCompileResourceStatus owner_resize(XrCompileResources *resources, void **out, size_t bytes) {
    bool fail = owner_allocation_begin();
    XrCompileResourceStatus status = xr_compile_resources_resize(resources, out, bytes);
    if (fail) { runtime_fail_at = SIZE_MAX; CHECK(status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY); }
    return status;
}
static int WINAPI owner_compare(LPCWCH left, int left_length, LPCWCH right, int right_length, BOOL ignore_case) {
    if (owner_compare_count++ == owner_compare_fail) { SetLastError(owner_compare_error); return 0; }
    return CompareStringOrdinal(left, left_length, right, right_length, ignore_case);
}
static void owner_tamper_report(const XrProcessView *view) {
    /* Replace a successful real replay's dependency report with another actual
     * leased source. This remains valid JSON but is a different source fact. */
    FILE *file = fopen(view->argv[25], "rb"); CHECK(file && !fseek(file, 0, SEEK_END));
    long length = ftell(file); CHECK(length > 0 && !fseek(file, 0, SEEK_SET));
    char *text = malloc((size_t)length + 1); CHECK(text);
    CHECK(fread(text, 1, (size_t)length, file) == (size_t)length && !fclose(file)); text[length] = 0;
    if (invocation_tamper == 2) {
        char *includes = strstr(text, "\"Includes\""); CHECK(includes);
        includes = strchr(includes, '['); CHECK(includes); ++includes;
        char *end = includes; bool quoted = false;
        while (*end) {
            if (*end == '\\' && quoted && end[1]) { end += 2; continue; }
            if (*end == '"') quoted = !quoted;
            if (*end == ']' && !quoted) break;
            ++end;
        }
        CHECK(*end == ']'); file = fopen(view->argv[25], "wb"); CHECK(file);
        CHECK(fwrite(text, 1, (size_t)(includes - text), file) == (size_t)(includes - text));
        CHECK(fwrite(end, 1, (size_t)length - (size_t)(end - text), file) == (size_t)length - (size_t)(end - text));
        CHECK(!fclose(file)); free(text); return;
    }
    char *source = strstr(text, "\"Source\""); CHECK(source);
    source = strchr(source + strlen("\"Source\""), ':'); CHECK(source);
    source = strchr(source, '"'); CHECK(source); ++source;
    char *end = source;
    while (*end && *end != '"') { if (*end == '\\' && end[1]) ++end; ++end; }
    CHECK(*end == '"');
    file = fopen(view->argv[25], "wb"); CHECK(file);
    CHECK(fwrite(text, 1, (size_t)(source - text), file) == (size_t)(source - text));
    for (const char *p = invocation_other_source; *p; ++p) CHECK(fputc(*p == '\\' ? '/' : *p, file) != EOF);
    CHECK(fwrite(end, 1, (size_t)length - (size_t)(end - text), file) == (size_t)length - (size_t)(end - text));
    CHECK(!fclose(file)); free(text);
}
static void owner_tamper_link(const XrProcessView *view) {
    const char *path = view->argv[6] + strlen("/LINKREPROFULLPATHRSP:");
    FILE *file = fopen(path, "rb"); CHECK(file && !fseek(file, 0, SEEK_END));
    long length = ftell(file); CHECK(length > 8 && !(length % 2) && !fseek(file, 0, SEEK_SET));
    wchar_t *text = malloc((size_t)length); CHECK(text);
    CHECK(fread(text, 1, (size_t)length, file) == (size_t)length && !fclose(file));
    CHECK(text[0] == 0xfeff);
    size_t first = 1, second = 1, after = 1, units = (size_t)length / sizeof(wchar_t);
    while (second < units && text[second] != L'\n') ++second; CHECK(second < units); ++second;
    after = second;
    while (after < units && text[after] != L'\n') ++after; CHECK(after < units); ++after;
    file = fopen(path, "wb"); CHECK(file);
    CHECK(fwrite(text, sizeof(wchar_t), second, file) == second);
    CHECK(fwrite(text + first, sizeof(wchar_t), second - first, file) == second - first);
    CHECK(fwrite(text + after, sizeof(wchar_t), units - after, file) == units - after);
    CHECK(!fclose(file)); free(text);
}
typedef struct OwnerPhaseMeasurement {
    uint64_t ticks, work, allocated, bytes;
    size_t allocations;
    unsigned calls;
} OwnerPhaseMeasurement;
static OwnerPhaseMeasurement finish_measurement, seal_measurement;
static const char *measure_report, *measure_output;
static bool measure_finishing;
static uint64_t measurement_clock(void) {
    LARGE_INTEGER value; CHECK(QueryPerformanceCounter(&value)); return (uint64_t)value.QuadPart;
}
static void measurement_add(OwnerPhaseMeasurement *sum, uint64_t start, size_t allocations,
    const XrCompileResourceStats *before, XrCompileResources *resources) {
    XrCompileResourceStats after; CHECK(xr_compile_resources_stats(resources, &after) == XR_COMPILE_RESOURCE_OK);
    sum->ticks += measurement_clock() - start;
    sum->work += after.work - before->work;
    sum->allocated += after.allocated_bytes - before->allocated_bytes;
    sum->allocations += runtime_attempts - allocations;
    ++sum->calls;
}
static XrXirTargetStatus owner_open(XrCompileResources *resources, const char *path, XtcXirFileLease **out) {
    bool finish = measure_finishing;
    XrCompileResourceStats before; CHECK(xr_compile_resources_stats(resources, &before) == XR_COMPILE_RESOURCE_OK);
    uint64_t start = measurement_clock(); size_t allocations = runtime_attempts;
    XrXirTargetStatus status = xtc_xir_file_lease_open(resources, path, out);
    if (invocation_runs == 6 && measure_output && !strcmp(path, measure_output) && status == XR_XIR_TARGET_OK)
        measure_finishing = true;
    if (finish) {
        measurement_add(&finish_measurement, start, allocations, &before, resources);
        if (status == XR_XIR_TARGET_OK) finish_measurement.bytes += xtc_xir_file_lease_facts(*out)->length;
    }
    return status;
}
static XrXirTargetStatus owner_seal(XrXirImageCollector *images) {
    if (profile_image_outside && invocation_runs == 1) {
        HANDLE file = CreateFileA(invocation_other_source, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(file != INVALID_HANDLE_VALUE);
        XrProcImageObserver observer = xtc_xir_images_observer(images);
        XrProcImageEvent event = {GetCurrentProcessId(), XR_PROC_IMAGE_DLL, (intptr_t)file};
        CHECK(observer.observe(observer.context, &event) == XR_PROC_OK && CloseHandle(file));
        puts("synthetic out-of-root DLL observation with a real held file, not a loader claim");
    }
    if (invocation_image_difference && invocation_runs == 2) {
        wchar_t path[32768]; CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, invocation_compiler, -1, path, 32768));
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(file != INVALID_HANDLE_VALUE);
        XrProcImageObserver observer = xtc_xir_images_observer(images);
        /* A deliberately different event kind for the real already-observed
         * compiler image preserves the file set but changes its kind mask. */
        XrProcImageEvent event = {GetCurrentProcessId(), XR_PROC_IMAGE_DLL, (intptr_t)file};
        CHECK(observer.observe(observer.context, &event) == XR_PROC_OK && CloseHandle(file));
    }
    XrCompileResources *resources = xtc_xir_images_resources(images);
    XrCompileResourceStats before; CHECK(xr_compile_resources_stats(resources, &before) == XR_COMPILE_RESOURCE_OK);
    uint64_t start = measurement_clock(); size_t allocations = runtime_attempts;
    XrXirTargetStatus status = xtc_xir_images_seal(images);
    measurement_add(&seal_measurement, start, allocations, &before, resources);
    return status;
}
static XrProcessStatus owner_run(const XrToolchainProcess *process, XrProcessCancelled cancel,
    void *context, XrProcessResult *output) {
    unsigned run = invocation_runs++;
    if (!run) owner_preexecution_comparisons = owner_compare_count;
    XrCompileResourceStats before, after;
    CHECK(xr_compile_resources_stats(xtc_process_resources(process), &before) == XR_COMPILE_RESOURCE_OK);
    ULONGLONG started = GetTickCount64(); size_t allocations = runtime_attempts;
    if (run == invocation_process_fail) runtime_fail_at = runtime_attempts;
    XrProcessStatus status = xtc_process_run(process, cancel, context, output);
    if (invocation_image_difference && run == 1 && status == XTC_PROCESS_OK) {
        XrProcessView view; CHECK(xtc_process_view(process, &view) == XTC_PROCESS_OK); invocation_compiler = view.executable;
    }
    runtime_fail_at = SIZE_MAX;
    CHECK(xr_compile_resources_stats(xtc_process_resources(process), &after) == XR_COMPILE_RESOURCE_OK);
    printf("stage-cost run=%u ms=%llu malloc=%zu bytes=%llu work=%llu status=%d\n", run,
        (unsigned long long)(GetTickCount64() - started), runtime_attempts - allocations,
        (unsigned long long)(after.allocated_bytes - before.allocated_bytes),
        (unsigned long long)(after.work - before.work), status);
    if ((invocation_tamper == 1 || invocation_tamper == 2) && run == 1 && status == XTC_PROCESS_OK) {
        XrProcessView view; CHECK(xtc_process_view(process, &view) == XTC_PROCESS_OK); owner_tamper_report(&view);
    }
    if (invocation_tamper == 3 && run == 5 && status == XTC_PROCESS_OK) {
        XrProcessView view; CHECK(xtc_process_view(process, &view) == XTC_PROCESS_OK); owner_tamper_link(&view);
    }
    return status;
}
static unsigned provider_reads, provider_read_oom = UINT_MAX, provider_corrupt = UINT_MAX;
static uint64_t provider_read_before[2], provider_read_after[2];
static XrXirTargetStatus provider_read(XrXirImageCollector *images, uint32_t index,
    size_t limit, void **bytes, size_t *length) {
    unsigned call = provider_reads++;
    XrCompileResourceStats stats;
    CHECK(xr_compile_resources_stats(xtc_xir_images_resources(images), &stats) == XR_COMPILE_RESOURCE_OK);
    if (call < 2) provider_read_before[call] = stats.work;
    size_t attempts = runtime_attempts;
    if (call == provider_read_oom) runtime_fail_at = attempts;
    XrXirTargetStatus status = xtc_xir_images_read(images, index, limit, bytes, length);
    runtime_fail_at = SIZE_MAX;
    if (call == provider_read_oom) CHECK(status == XR_XIR_TARGET_OUT_OF_MEMORY && runtime_attempts == attempts + 1);
    CHECK(xr_compile_resources_stats(xtc_xir_images_resources(images), &stats) == XR_COMPILE_RESOURCE_OK);
    if (call < 2) provider_read_after[call] = stats.work;
    if (status == XR_XIR_TARGET_OK && call == provider_corrupt) {
        /* Deliberately malformed decoded input tests the PE diagnostic boundary;
         * it is not presented as a changed or authorized executable image. */
        CHECK(*length); ((uint8_t *)*bytes)[0] = 0;
    }
    return status;
}
#define xtc_xir_images_read provider_read
#define xr_compile_resources_alloc owner_alloc
#define xr_compile_resources_calloc owner_calloc
#define xr_compile_resources_resize owner_resize
#define CompareStringOrdinal owner_compare
#define xtc_process_run owner_run
#define xr_os_io_write_new_file_sync owner_write
#define xtc_xir_images_seal owner_seal
#define xtc_xir_file_lease_open owner_open
#define GetWindowsDirectoryW profile_windows_directory
#define GetSystemDirectoryW profile_system_directory
#define GetDllDirectoryW profile_dll_directory
#define GetCurrentPackageFullName profile_package
#include "app/toolchain/xtc_xir_invocation.c"
#undef xtc_xir_images_read
#undef GetWindowsDirectoryW
#undef GetSystemDirectoryW
#undef GetDllDirectoryW
#undef GetCurrentPackageFullName
#undef xr_compile_resources_alloc
#undef xr_compile_resources_calloc
#undef xr_compile_resources_resize
#undef CompareStringOrdinal
#undef xtc_process_run
#undef xr_os_io_write_new_file_sync
#undef xtc_xir_images_seal
#undef xtc_xir_file_lease_open

static bool owner_record_transaction(const char *path, const XrCompileResourceLimits *limits,
    XrCompileResourceStats *stats) {
    XrCompileResources *resources = NULL;
    XrCompileResourceStatus created = xr_compile_resources_new(limits, &resources);
    CHECK(created == XR_COMPILE_RESOURCE_OK);
    XrXirInvocation owner = {0}; owner.context.resources = resources; owner.io = xr_compile_io_policy(resources);
    owner.limits.files = 16;
    XtcXirFileLease *lease = NULL;
    bool okay = invocation_target(&owner, xtc_xir_file_lease_open(resources, path, &lease));
    if (okay) okay = invocation_file_add(&owner, XR_XIR_INVOCATION_GENERATED, XR_XIR_INVOCATION_SOURCE, &lease);
    CHECK(xr_compile_resources_stats(resources, stats) == XR_COMPILE_RESOURCE_OK);
    if (okay) CHECK(owner.facts.file_count == 1 && !lease);
    else CHECK(owner.status == XR_XIR_INVOCATION_BUDGET && owner.facts.file_count == 0);
    for (uint32_t i = 0; i < owner.facts.file_count; ++i) xtc_xir_file_lease_free(owner.files[i].lease);
    xtc_xir_file_lease_free(lease); xr_compile_resources_free(owner.files); xr_compile_resources_release(resources);
    CHECK(!runtime_live && !runtime_bytes); return okay;
}
static void owner_record_boundaries(const char *path) {
    XrCompileResourceStats baseline;
    CHECK(owner_record_transaction(path, &sdk_unlimited, &baseline));
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = sdk_unlimited;
        if (!axis) limits.allocated_bytes = baseline.allocated_bytes - minus;
        else if (axis == 1) limits.live_bytes = baseline.peak_bytes - minus;
        else limits.work = baseline.work - minus;
        XrCompileResourceStats actual;
        CHECK(owner_record_transaction(path, &limits, &actual) == !minus);
    }
    printf("real regular lease -> owner file record three-axis exact/minus1: allocated=%llu peak=%llu work=%llu PASS\n",
        (unsigned long long)baseline.allocated_bytes, (unsigned long long)baseline.peak_bytes,
        (unsigned long long)baseline.work);
    owner_allocation_count = 0;
}

/* This bookkeeping gate supplies a synthetic observer event carrying a real
 * fixture handle. Actual image observation is tested by the six-run consumer. */
static bool owner_image_transaction(const char *path, const XrCompileResourceLimits *limits,
    size_t fail, XrCompileResourceStats *stats, size_t *attempts) {
    XrCompileResources *resources = sdk_ledger(limits);
    size_t start = runtime_attempts;
    if (fail != SIZE_MAX) runtime_fail_at = start + fail;
    XrXirInvocation owner = {0}; owner.context.resources = resources; owner.limits.files = 1;
    wchar_t wide[32768]; CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 32768));
    HANDLE writable = CreateFileW(wide, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(writable != INVALID_HANDLE_VALUE && CloseHandle(writable));
    XrXirImageCollector *images = NULL;
    bool okay = invocation_target(&owner, xtc_xir_images_new(resources, &images));
    if (okay) {
        HANDLE file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(file != INVALID_HANDLE_VALUE);
        XrProcImageObserver observer = xtc_xir_images_observer(images);
        XrProcImageEvent event = {GetCurrentProcessId(), XR_PROC_IMAGE_EXECUTABLE, (intptr_t)file};
        XrOsProcStatus observed = observer.observe(observer.context, &event);
        CHECK(CloseHandle(file));
        okay = invocation_target(&owner, xtc_xir_images_status(images));
        CHECK((observed == XR_PROC_OK) == okay);
    }
    if (okay) okay = invocation_target(&owner, xtc_xir_images_seal(images));
    if (okay) {
        const XrXirImageFile *image = xtc_xir_images_file(images, 0); CHECK(image);
        okay = invocation_file_publish(&owner, XR_XIR_INVOCATION_GENERATED,
            XR_XIR_INVOCATION_PROVIDER_IMAGE, image->path, image->length, image->digest, NULL);
        if (okay) {
            CHECK(owner.facts.file_count == 1 && !owner.files[0].lease && owner.files[0].facts.path == image->path);
            writable = CreateFileW(wide, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            CHECK(writable == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);
        }
    }
    *attempts = runtime_attempts - start; runtime_fail_at = SIZE_MAX;
    CHECK(xr_compile_resources_stats(resources, stats) == XR_COMPILE_RESOURCE_OK);
    if (!okay) CHECK(owner.status == (fail == SIZE_MAX ? XR_XIR_INVOCATION_BUDGET : XR_XIR_INVOCATION_OUT_OF_MEMORY));
    xr_compile_resources_free(owner.files); xtc_xir_images_free(images); xr_compile_resources_release(resources);
    writable = CreateFileW(wide, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(writable != INVALID_HANDLE_VALUE && CloseHandle(writable));
    CHECK(!runtime_live && !runtime_bytes);
    return okay;
}
static void owner_image_boundaries(const char *path) {
    XrCompileResourceStats baseline; size_t allocations = 0;
    CHECK(owner_image_transaction(path, &sdk_unlimited, SIZE_MAX, &baseline, &allocations));
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = sdk_unlimited;
        if (!axis) limits.allocated_bytes = baseline.allocated_bytes - minus;
        else if (axis == 1) limits.live_bytes = baseline.peak_bytes - minus;
        else limits.work = baseline.work - minus;
        XrCompileResourceStats stats; size_t attempts = 0;
        CHECK(owner_image_transaction(path, &limits, SIZE_MAX, &stats, &attempts) == !minus);
    }
    for (size_t point = 0; point < allocations; ++point) {
        XrCompileResourceStats stats; size_t attempts = 0;
        CHECK(!owner_image_transaction(path, &sdk_unlimited, point, &stats, &attempts) && attempts == point + 1);
    }
    printf("sealed image borrowed row: %zu actual malloc failures; three-axis exact/minus1; write before/after and blocked while leased; physical zero PASS\n", allocations);
    owner_allocation_count = 0;
}

/* Synthetic EXE events carry real file handles through the production collector.
 * This checks composition and does not claim that a provider process executed. */
static void provider_collector(XrCompileResources *resources, const char *path,
    bool executable, XrXirImageCollector **images) {
    CHECK(xtc_xir_images_new(resources, images) == XR_XIR_TARGET_OK);
    wchar_t wide[32768]; CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 32768));
    HANDLE handle = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(handle != INVALID_HANDLE_VALUE);
    XrProcImageObserver observer = xtc_xir_images_observer(*images);
    XrProcImageEvent event = {GetCurrentProcessId(), executable ? XR_PROC_IMAGE_EXECUTABLE : XR_PROC_IMAGE_DLL, (intptr_t)handle};
    CHECK(observer.observe(observer.context, &event) == XR_PROC_OK && CloseHandle(handle));
    CHECK(xtc_xir_images_seal(*images) == XR_XIR_TARGET_OK);
}
static bool provider_transaction(const char *compiler, const char *linker,
    const XrCompileResourceLimits *limits, unsigned scenario, XrCompileResourceStats *cost) {
    XrCompileResources *resources = sdk_ledger(limits);
    XrXirInvocation *owner = NULL;
    CHECK(xr_compile_resources_calloc(resources, 1, sizeof(*owner), (void **)&owner) == XR_COMPILE_RESOURCE_OK);
    owner->context.resources = resources; owner->io = xr_compile_io_policy(resources);
    owner->limits.artifact_bytes = UINT64_MAX; owner->limits.dependencies.path_bytes = 32768;
    for (unsigned stage = 0; stage < 3; ++stage) {
        const char *path = stage == 2 || (stage == 1 && scenario == 1) ? linker : compiler;
        owner->commands[stage].executable = path;
        provider_collector(resources, path, stage != 1 || scenario != 2, &owner->images[stage]);
    }
    if (scenario == 3) owner->commands[0].executable = linker;
    if (scenario == 4) owner->limits.artifact_bytes = xtc_xir_images_file(owner->images[0], 0)->length - 1;

    XrXirInvocationProviderFacts unchanged;
    memset(&owner->provider, 0xa5, sizeof(owner->provider)); unchanged = owner->provider;
    provider_reads = 0;
    provider_read_oom = scenario == 5 ? 0u : scenario == 6 ? 1u : UINT_MAX;
    provider_corrupt = scenario == 7 ? 0u : scenario == 8 ? 1u : UINT_MAX;
    bool okay = invocation_provider_versions(owner);
    if (scenario && scenario < 9) {
        XrXirInvocationStatus expected = scenario == 1 ? XR_XIR_INVOCATION_REPLAY_MISMATCH :
            scenario == 2 || scenario == 3 ? XR_XIR_INVOCATION_UNRESOLVED :
            scenario == 4 ? XR_XIR_INVOCATION_BUDGET :
            scenario == 5 || scenario == 6 ? XR_XIR_INVOCATION_OUT_OF_MEMORY : XR_XIR_INVOCATION_INVALID;
        CHECK(!okay && owner->status == expected && owner->diagnostic.pass == XR_XIR_INVOCATION_NO_PASS);
        CHECK(owner->diagnostic.stage == (scenario == 1 || scenario == 2 ? XR_XIR_INVOCATION_LAUNCHER :
            scenario == 6 || scenario == 8 ? XR_XIR_INVOCATION_LINK : XR_XIR_INVOCATION_GENERATED));
        if (scenario >= 7) CHECK(owner->diagnostic.domain == XR_XIR_INVOCATION_PE_VERSION &&
            owner->diagnostic.code == XTC_XIR_PE_VERSION_INVALID);
        if (scenario == 5 || scenario == 6) CHECK(owner->diagnostic.domain == XR_XIR_INVOCATION_TARGET &&
            owner->diagnostic.code == XR_XIR_TARGET_OUT_OF_MEMORY);
    }
    if (okay) {
        CHECK(provider_reads == 2);
        const XrXirInvocationProviderFacts *facts = xtc_xir_invocation_provider(owner);
        CHECK(facts == &owner->provider && facts->compiler.path != compiler && facts->linker.path != linker);
        CHECK(facts->compiler.version.file_text[0] && facts->linker.version.file_text[0]);
        CHECK(!facts->compiler.observed_image_index && !facts->linker.observed_image_index && !facts->launcher_compiler_image_index);
        CHECK(!memcmp(facts->compiler.digest, xtc_xir_images_file(owner->images[0], 0)->digest, 32));
        CHECK(!memcmp(facts->linker.digest, xtc_xir_images_file(owner->images[2], 0)->digest, 32));
    } else {
        CHECK(!memcmp(&owner->provider, &unchanged, sizeof(unchanged)));
        if (!scenario) CHECK(owner->status == XR_XIR_INVOCATION_BUDGET);
    }

    CHECK(xr_compile_resources_stats(resources, cost) == XR_COMPILE_RESOURCE_OK);
    XrXirInvocationProviderFacts saved = owner->provider;
    xr_compile_resources_release(resources);
    if (okay) CHECK(!memcmp(xtc_xir_invocation_provider(owner), &saved, sizeof(saved)) &&
        xtc_xir_invocation_provider(owner)->compiler.path[0]);
    xtc_xir_invocation_free(owner);
    provider_read_oom = provider_corrupt = UINT_MAX;
    CHECK(!runtime_live && !runtime_bytes); return okay;
}
static int owner_provider_suite(const char *compiler, const char *linker) {
    DWORD handles, after; CHECK(GetProcessHandleCount(GetCurrentProcess(), &handles));
    XrCompileResourceStats baseline;
    CHECK(provider_transaction(compiler, linker, &sdk_unlimited, 0, &baseline));
    printf("provider transaction exact: allocated=%llu peak=%llu work=%llu\n",
        (unsigned long long)baseline.allocated_bytes, (unsigned long long)baseline.peak_bytes,
        (unsigned long long)baseline.work);
    printf("provider actual read work boundaries: %llu..%llu / %llu..%llu\n",
        (unsigned long long)provider_read_before[0], (unsigned long long)provider_read_after[0],
        (unsigned long long)provider_read_before[1], (unsigned long long)provider_read_after[1]);
    uint64_t boundaries[4] = {provider_read_before[0], provider_read_after[0], provider_read_before[1], provider_read_after[1]};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = sdk_unlimited;
        if (!axis) limits.allocated_bytes = baseline.allocated_bytes - minus;
        else if (axis == 1) limits.live_bytes = baseline.peak_bytes - minus;
        else limits.work = baseline.work - minus;
        XrCompileResourceStats actual;
        CHECK(provider_transaction(compiler, linker, &limits, 0, &actual) == !minus);
    }
    for (unsigned i = 0; i < 4; ++i) {
        XrCompileResourceLimits limits = sdk_unlimited; limits.work = boundaries[i];
        XrCompileResourceStats actual;
        CHECK(!provider_transaction(compiler, linker, &limits, 0, &actual));
    }
    for (unsigned scenario = 1; scenario <= 8; ++scenario) {
        XrCompileResourceStats actual; CHECK(!provider_transaction(compiler, linker, &sdk_unlimited, scenario, &actual));
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &after) && after == handles);
    printf("provider-only real leases/PE/read OOM/three-axis exact-minus1/bad matches: heap=0 handles=%lu/%lu PASS\n",
        (unsigned long)handles, (unsigned long)after);
    return 0;
}
