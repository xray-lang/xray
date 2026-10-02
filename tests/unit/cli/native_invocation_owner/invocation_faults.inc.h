/* Test-only interposition around the unique production owner. Every allocation
 * failure reaches the actual ledger allocator. Dependency implementations and
 * real child commands remain unchanged. */
#include "app/toolchain/xtc_xir_file_lease.h"
#include "app/toolchain/xtc_xir_images.h"
#include "base/xwindows_utf8.h"
#include "os/os_fs.h"
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
    return xtc_xir_images_seal(images);
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
#include "app/toolchain/xtc_xir_invocation.c"
#undef xr_compile_resources_alloc
#undef xr_compile_resources_calloc
#undef xr_compile_resources_resize
#undef CompareStringOrdinal
#undef xtc_process_run
#undef xr_os_io_write_new_file_sync
#undef xtc_xir_images_seal

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
