/* Test-only interposition around the unique production owner. Every allocation
 * failure reaches the actual ledger allocator. Dependency implementations and
 * real child commands remain unchanged. */
#include "app/toolchain/xtc_xir_file_lease.h"
#include "app/toolchain/xtc_xir_images.h"
#include "base/xwindows_utf8.h"
#include "os/os_fs.h"
/* A retained real pending kernel request exercises the upper owner boundary. */
static bool guard_hold_cancel, guard_fail_result;
static unsigned guard_cancel_holds;
static BOOL WINAPI guard_cancel(HANDLE handle, LPOVERLAPPED overlapped) {
    if (guard_hold_cancel) { ++guard_cancel_holds; SetLastError(ERROR_NOT_FOUND); return FALSE; }
    return CancelIoEx(handle, overlapped);
}
static BOOL WINAPI guard_result(HANDLE handle, LPOVERLAPPED overlapped, LPDWORD bytes, BOOL wait) {
    if (guard_fail_result) { guard_fail_result = false; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return GetOverlappedResult(handle, overlapped, bytes, wait);
}
#define GetOverlappedResult guard_result
#define CancelIoEx guard_cancel
#include "app/toolchain/xtc_xir_namespace.c"
#undef CancelIoEx
#undef GetOverlappedResult
static unsigned guard_checks, guard_break_check;
static bool guard_arm_oom, guard_check_budget, guard_check_io;
static const char *guard_input_path, *guard_source_paths[2];
static void guard_change_directory(void) {
    char path[32768]; CHECK(snprintf(path, sizeof(path), "%s/actual-namespace-change", guard_input_path) > 0);
    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE && CloseHandle(file) && DeleteFileA(path));
}
static XrXirNamespaceStatus guard_arm(XrXirNamespace *owner) {
    CHECK(xtc_xir_namespace_phase(owner) == XR_XIR_NAMESPACE_NEW);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(GetFileAttributesA(guard_source_paths[i]) != INVALID_FILE_ATTRIBUTES);
        HANDLE file = CreateFileA(guard_source_paths[i], GENERIC_WRITE, FILE_SHARE_READ,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);
    }
    if (guard_arm_oom) runtime_fail_at = runtime_attempts;
    XrXirNamespaceStatus status = xtc_xir_namespace_arm(owner);
    runtime_fail_at = SIZE_MAX;
    return status;
}
static XrXirNamespaceStatus guard_check(XrXirNamespace *owner) {
    ++guard_checks;
    if (guard_check_budget) {
        XrCompileResourceStats stats;
        XrCompileResources *resources = xtc_xir_namespace_resources(owner);
        CHECK(xr_compile_resources_stats(resources, &stats) == XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_resources_work(resources, UINT64_MAX - stats.work) == XR_COMPILE_RESOURCE_OK);
    }
    if (guard_check_io) guard_fail_result = true;
    if (guard_checks == guard_break_check) guard_change_directory();
    return xtc_xir_namespace_check(owner);
}
static size_t owner_allocation_count, owner_allocation_fail = SIZE_MAX;
static size_t owner_compare_count, owner_compare_fail = SIZE_MAX;
static DWORD owner_compare_error = ERROR_ACCESS_DENIED;
static unsigned invocation_runs, invocation_process_fail = UINT_MAX;
static unsigned invocation_tamper;
static bool invocation_image_difference;
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
    FILE *file = fopen(view->argv[20], "rb"); CHECK(file && !fseek(file, 0, SEEK_END));
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
        CHECK(*end == ']'); file = fopen(view->argv[20], "wb"); CHECK(file);
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
    file = fopen(view->argv[20], "wb"); CHECK(file);
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
#define xr_compile_resources_alloc owner_alloc
#define xr_compile_resources_calloc owner_calloc
#define xr_compile_resources_resize owner_resize
#define CompareStringOrdinal owner_compare
#define xtc_process_run owner_run
#define xr_os_io_write_new_file_sync owner_write
#define xtc_xir_images_seal owner_seal
#define xtc_xir_file_lease_open owner_open
#define xtc_xir_namespace_arm guard_arm
#define xtc_xir_namespace_check guard_check
#include "app/toolchain/xtc_xir_invocation.c"
#undef xr_compile_resources_alloc
#undef xr_compile_resources_calloc
#undef xr_compile_resources_resize
#undef CompareStringOrdinal
#undef xtc_process_run
#undef xr_os_io_write_new_file_sync
#undef xtc_xir_images_seal
#undef xtc_xir_file_lease_open
#undef xtc_xir_namespace_arm
#undef xtc_xir_namespace_check

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
