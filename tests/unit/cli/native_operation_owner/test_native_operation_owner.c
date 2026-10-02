/* Actual Source/SDK and private workspace; injected gates stop before native
 * execution explicitly. Native success uses the published SDK launcher. */
#include "app/toolchain/xtc_xir_native_operation.h"
#include "toolchain/xcompiler_session.h"
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "../native_invocation_owner/invocation_allocator.h"
#ifdef OPERATION_INJECTED
#include "operation_inject.h"
#endif
static const XtcXirNativeOperationLimits test_limits = {{{4 * 1024 * 1024, 32768, 4096}, 64 * 1024 * 1024, 4096}, 30000, 4 * 1024 * 1024};
static XtcXirWorkspaceRequest workspace_request(const char *parent) {
    XtcXirWorkspaceRequest request = {parent, {32767, 32, 65536}}; return request;
}
static void close_owner(XtcXirNativeOperation **owner) {
    for (unsigned i = 0; *owner && i < 10000; ++i) {
        XtcXirNativeOperationStatus status = xtc_xir_native_operation_close(owner, 64);
        CHECK(status == XTC_XIR_NATIVE_OK || status == XTC_XIR_NATIVE_PENDING);
    }
    CHECK(!*owner);
}
static void print_diagnostic(const XtcXirNativeOperation *owner) {
    const XtcXirNativeOperationDiagnostic *d = xtc_xir_native_operation_diagnostic(owner);
    printf("status=%u domain=%u code=%d stage=%u pass=%u exit=%d\n",
        (unsigned)d->status, (unsigned)d->domain, d->code, (unsigned)d->invocation.stage,
        (unsigned)d->invocation.pass, d->invocation.exit_code); fflush(stdout);
}
static XrXirNativeProjection *projection(XrCompileResources *resources, const char *directory,
    const char *source, const char *stdlib) {
    XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, directory};
    XrXirSourceProductRequest request = {{session, source, &authority, &context, stdlib, NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product = NULL;
    CHECK(xr_xir_compile_source_product_build(&request, &product, NULL) == XR_XIR_OK);
    xr_compile_session_free(session);
    XrXirNativeProjectionRequest prepare = {product, "operation_source", 16777216};
    XrXirNativeProjection *result = NULL;
    CHECK(xr_compile_native_projection_prepare(&prepare, &result, NULL) == XR_XIR_OK);
    xr_xir_compile_source_product_free(product); return result;
}
static XrXirRuntimeSdk *load_sdk(XrCompileResources *resources, const char *root) {
    char path[32768]; CHECK(snprintf(path, sizeof(path), "%s/sdk_manifest.json", root) > 0);
    FILE *file = fopen(path, "rb"); CHECK(file && !fseek(file, 0, SEEK_END));
    long length = ftell(file); CHECK(length > 0 && !fseek(file, 0, SEEK_SET));
    void *bytes = malloc((size_t)length); CHECK(bytes);
    CHECK(fread(bytes, 1, (size_t)length, file) == (size_t)length && !fclose(file));
    XrXirRuntimeSdk *sdk = NULL; XrXirRuntimeSdkRequest request = {root, bytes, (size_t)length, resources};
    CHECK(xr_xir_runtime_sdk_load(&request, &sdk) == XR_XIR_SDK_OK); free(bytes); return sdk;
}
#ifdef OPERATION_INJECTED
static void unit(const char *parent) {
    XtcXirWorkspaceRequest ws = workspace_request(parent);
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited);
    size_t before = runtime_attempts;
    XtcXirNativeOperation *owner = NULL;
    CHECK(xtc_xir_native_operation_new(resources, &ws, &test_limits, &owner) == XTC_XIR_NATIVE_OK);
    size_t allocations = runtime_attempts - before;
    XrCompileResourceStats exact; CHECK(xr_compile_resources_stats(resources, &exact) == XR_COMPILE_RESOURCE_OK);
    CHECK(xtc_xir_native_operation_phase(owner) == XTC_XIR_NATIVE_NEW && !xtc_xir_native_operation_facts(owner));
    CHECK(!xtc_xir_native_operation_provider(owner) && !xtc_xir_native_operation_provider(NULL));
    void *sentinel = (void *)(uintptr_t)1; size_t length = 17;
    CHECK(xtc_xir_native_operation_read_output(owner, 100, &sentinel, &length) == XTC_XIR_NATIVE_INVALID);
    CHECK(sentinel == (void *)(uintptr_t)1 && length == 17);
    close_owner(&owner); xr_compile_resources_release(resources); CHECK(!runtime_live && !runtime_bytes);
    for (size_t fail = 0; fail < allocations; ++fail) {
        resources = sdk_ledger(&sdk_unlimited); runtime_fail_at = runtime_attempts + fail;
        CHECK(xtc_xir_native_operation_new(resources, &ws, &test_limits, &owner) == XTC_XIR_NATIVE_OUT_OF_MEMORY);
        CHECK(!owner); runtime_fail_at = SIZE_MAX;
        xr_compile_resources_release(resources); CHECK(!runtime_live && !runtime_bytes);
    }
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = sdk_unlimited;
        if (!axis) limits.allocated_bytes = exact.allocated_bytes - minus;
        else if (axis == 1) limits.live_bytes = exact.peak_bytes - minus;
        else limits.work = exact.work - minus;
        resources = sdk_ledger(&limits);
        CHECK(xtc_xir_native_operation_new(resources, &ws, &test_limits, &owner) ==
            (minus ? XTC_XIR_NATIVE_BUDGET : XTC_XIR_NATIVE_OK));
        CHECK(minus ? !owner : !!owner);
        close_owner(&owner); xr_compile_resources_release(resources); CHECK(!runtime_live && !runtime_bytes);
    }
    resources = sdk_ledger(&sdk_unlimited);
    CHECK(xtc_xir_native_operation_new(resources, &ws, &test_limits, &owner) == XTC_XIR_NATIVE_OK);
    CHECK(xtc_xir_workspace_create(owner->workspace) == XTC_XIR_WORKSPACE_OK);
    char root[32768]; CHECK(snprintf(root, sizeof(root), "%s", xtc_xir_workspace_paths(owner->workspace)->root) > 0);
    char temporary[32768], unregistered[32768];
    CHECK(snprintf(temporary, sizeof(temporary), "%s/compiler-temp", xtc_xir_workspace_paths(owner->workspace)->output) > 0);
    CHECK(CreateDirectoryA(temporary, NULL));
    CHECK(snprintf(unregistered, sizeof(unregistered), "%s/unregistered.tmp", temporary) > 0);
    HANDLE temp = CreateFileA(unregistered, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(temp != INVALID_HANDLE_VALUE); DWORD written = 0;
    CHECK(WriteFile(temp, "partial compiler temporary", 26, &written, NULL) && written == 26 && CloseHandle(temp));
    native_fail(owner, XTC_XIR_NATIVE_CANCELLED, XTC_XIR_NATIVE_SELF, 91);
    operation_workspace_closes = 0;
    size_t allocation_boundary = runtime_attempts; XrCompileResourceStats before_close;
    CHECK(xr_compile_resources_stats(resources, &before_close) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_work(resources, UINT64_MAX - before_close.work) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(resources, &before_close) == XR_COMPILE_RESOURCE_OK && before_close.work == UINT64_MAX);
    operation_hold_workspace = true;
    XtcXirNativeOperationStatus status = XTC_XIR_NATIVE_PENDING;
    for (unsigned i = 0; status == XTC_XIR_NATIVE_PENDING && i < 100; ++i)
        status = xtc_xir_native_operation_close(&owner, 64);
    CHECK(status == XTC_XIR_NATIVE_IO && owner && operation_workspace_closes);
    CHECK(xtc_xir_native_operation_diagnostic(owner)->code == 91 &&
        xtc_xir_native_operation_cleanup_diagnostic(owner)->domain == XTC_XIR_NATIVE_WORKSPACE);
    operation_hold_workspace = false;
    CHECK(xtc_xir_native_operation_close(&owner, 1) == XTC_XIR_NATIVE_PENDING && owner);
    CHECK(xtc_xir_native_operation_diagnostic(owner)->status == XTC_XIR_NATIVE_CANCELLED);
    close_owner(&owner); CHECK(GetFileAttributesA(root) == INVALID_FILE_ATTRIBUTES && GetFileAttributesA(unregistered) == INVALID_FILE_ATTRIBUTES);
    XrCompileResourceStats after_close; CHECK(xr_compile_resources_stats(resources, &after_close) == XR_COMPILE_RESOURCE_OK);
    CHECK(runtime_attempts == allocation_boundary && after_close.work == before_close.work);
    xr_compile_resources_release(resources); CHECK(!runtime_live && !runtime_bytes);
    printf("unit constructor OOM=%zu exact allocated=%llu peak=%llu work=%llu; workspace pending/cleanup=PASS physical=0\n",
        allocations, (unsigned long long)exact.allocated_bytes, (unsigned long long)exact.peak_bytes, (unsigned long long)exact.work);
}
static bool cancelled(void *context) { (void)context; return true; }
static void preflight(const char *parent, XrCompileResources *resources, XtcXirNativeOperationRequest *request) {
    XtcXirWorkspaceRequest ws = workspace_request(parent);
    XrCompileResources *foreign = sdk_ledger(&sdk_unlimited);
    XtcXirNativeOperation *owner = NULL;
    CHECK(xtc_xir_native_operation_new(foreign, &ws, &test_limits, &owner) == XTC_XIR_NATIVE_OK);
    size_t before = runtime_attempts;
    CHECK(xtc_xir_native_operation_run(owner, request) == XTC_XIR_NATIVE_INVALID);
    CHECK(runtime_attempts == before && !xtc_xir_workspace_paths(owner->workspace)->root);
    close_owner(&owner);
    XrXirRuntimeSdk *foreign_sdk = load_sdk(foreign, xr_xir_runtime_sdk_root(request->sdk));
    const XrXirRuntimeSdk *saved_sdk = request->sdk; request->sdk = foreign_sdk;
    CHECK(xtc_xir_native_operation_new(resources, &ws, &test_limits, &owner) == XTC_XIR_NATIVE_OK);
    before = runtime_attempts;
    CHECK(xtc_xir_native_operation_run(owner, request) == XTC_XIR_NATIVE_INVALID);
    CHECK(runtime_attempts == before && !xtc_xir_workspace_paths(owner->workspace)->root);
    close_owner(&owner); request->sdk = saved_sdk;
    xr_xir_runtime_sdk_free(foreign_sdk); xr_compile_resources_release(foreign);
    CHECK(xtc_xir_native_operation_new(resources, &ws, &test_limits, &owner) == XTC_XIR_NATIVE_OK);
    request->cancelled = cancelled; before = runtime_attempts;
    CHECK(xtc_xir_native_operation_run(owner, request) == XTC_XIR_NATIVE_CANCELLED);
    CHECK(runtime_attempts == before && !xtc_xir_workspace_paths(owner->workspace)->root);
    close_owner(&owner); request->cancelled = NULL;
    operation_stop_before_native = true; operation_direct_count = 0;
    CHECK(xtc_xir_native_operation_new(resources, &ws, &test_limits, &owner) == XTC_XIR_NATIVE_OK);
    CHECK(xtc_xir_native_operation_run(owner, request) == XTC_XIR_NATIVE_IO);
    CHECK(operation_run_calls == 1 && !xtc_xir_native_operation_facts(owner));
    size_t direct = operation_direct_count;
    close_owner(&owner);
    XrCompileResourceStats baseline; CHECK(xr_compile_resources_stats(resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    for (size_t fail = 0; fail < direct; ++fail) {
        operation_direct_count = 0; operation_direct_fail = fail; operation_run_calls = 0;
        XtcXirNativeOperationStatus status = xtc_xir_native_operation_new(resources, &ws, &test_limits, &owner);
        if (status == XTC_XIR_NATIVE_OK) status = xtc_xir_native_operation_run(owner, request);
        CHECK(status == XTC_XIR_NATIVE_OUT_OF_MEMORY && !operation_run_calls);
        operation_direct_fail = SIZE_MAX; close_owner(&owner);
        XrCompileResourceStats current; CHECK(xr_compile_resources_stats(resources, &current) == XR_COMPILE_RESOURCE_OK);
        CHECK(current.live_bytes == baseline.live_bytes);
    }
    printf("preflight direct actual OOM=%zu foreign/pre-cancel zero allocation; real SDK launcher/3 prepared=PASS\n", direct);
}
#endif
static int run_test(int argc, char **argv) {
#ifdef OPERATION_INJECTED
    if (argc == 3 && !strcmp(argv[1], "unit")) { unit(argv[2]); return 0; }
#endif
    CHECK(argc == 20);
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited);
    XrXirNativeProjection *project = projection(resources, argv[2], argv[3], argv[4]);
    XrXirRuntimeSdk *sdk = load_sdk(resources, argv[5]);
    XrXirInvocationLibrary libraries[5];
    for (unsigned i = 0; i < 5; ++i) libraries[i] = (XrXirInvocationLibrary){argv[9 + i], i == 4 ? XR_XIR_INVOCATION_SYSTEM : XR_XIR_INVOCATION_CRT};
    XtcXirNativeOperationRequest request = {project, sdk, argv[6], argv[7],
        {argv[14], argv[15], argv[16], argv[17], argv[18]}, libraries, 5, NULL, NULL};
#ifdef OPERATION_INJECTED
    if (!strcmp(argv[1], "preflight")) {
        preflight(argv[8], resources, &request);
        xr_compile_native_projection_owner_free(project); xr_xir_runtime_sdk_free(sdk);
        xr_compile_resources_release(resources); CHECK(!runtime_live && !runtime_bytes); return 0;
    }
#endif
    XtcXirWorkspaceRequest ws = workspace_request(argv[8]); XtcXirNativeOperation *owner = NULL;
    CHECK(xtc_xir_native_operation_new(resources, &ws, &test_limits, &owner) == XTC_XIR_NATIVE_OK);
    XtcXirNativeOperationStatus status = xtc_xir_native_operation_run(owner, &request);
    /* Producers and every request borrow end before result use and cleanup. */
    xr_compile_native_projection_owner_free(project); xr_xir_runtime_sdk_free(sdk);
    memset(&request, 0xA5, sizeof(request)); xr_compile_resources_release(resources);
    print_diagnostic(owner);
    if (status != XTC_XIR_NATIVE_OK) {
        close_owner(&owner); CHECK(!runtime_live && !runtime_bytes);
        return 1;
    }
    const XrXirInvocationFacts *facts = xtc_xir_native_operation_facts(owner);
    const XrXirInvocationProviderFacts *provider = xtc_xir_native_operation_provider(owner);
    CHECK(provider && provider->compiler.path[0] && provider->linker.path[0]);
    CHECK(provider->compiler.version.file_text[0] && provider->linker.version.file_text[0]);
    CHECK(facts && facts->completed_runs == 6 && facts->kind == XR_XIR_INVOCATION_LOCKED_REPLAY_FACTS);
    const XrXirInvocationFile *output = NULL;
    for (uint32_t i = 0; i < facts->file_count; ++i) {
        const XrXirInvocationFile *file = xtc_xir_native_operation_file(owner, i);
        if (file->kind == XR_XIR_INVOCATION_OUTPUT) { CHECK(!output); output = file; }
    }
    CHECK(output);
    char output_path[32768]; CHECK(snprintf(output_path, sizeof(output_path), "%s", output->path) > 0);
    HANDLE write = CreateFileA(output_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, 0, NULL);
    CHECK(write == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);
    void *bytes = NULL; size_t length = 7;
    CHECK(xtc_xir_native_operation_read_output(owner, 64 * 1024 * 1024, &bytes, &length) == XTC_XIR_NATIVE_INVALID);
    CHECK(!bytes && length == 7 && xtc_xir_native_operation_phase(owner) == XTC_XIR_NATIVE_READY);
    bytes = (void *)(uintptr_t)1; length = 0;
    CHECK(xtc_xir_native_operation_read_output(owner, 64 * 1024 * 1024, &bytes, &length) == XTC_XIR_NATIVE_INVALID);
    CHECK(bytes == (void *)(uintptr_t)1 && !length && xtc_xir_native_operation_phase(owner) == XTC_XIR_NATIVE_READY);
    bytes = NULL;
    length = 0;
    CHECK(xtc_xir_native_operation_read_output(owner, 64 * 1024 * 1024, &bytes, &length) == XTC_XIR_NATIVE_OK);
    CHECK(length > 2 && ((char *)bytes)[0] == 'M' && ((char *)bytes)[1] == 'Z');
    printf("native completed=%u files=%u output=%zu producer-dead=PASS\n", facts->completed_runs, facts->file_count, length);
    void *failed_bytes = NULL; size_t failed_length = 0;
    runtime_fail_at = runtime_attempts;
    CHECK(xtc_xir_native_operation_read_output(owner, 64 * 1024 * 1024, &failed_bytes, &failed_length) == XTC_XIR_NATIVE_OUT_OF_MEMORY);
    CHECK(!failed_bytes && !failed_length); runtime_fail_at = SIZE_MAX;
    close_owner(&owner); CHECK(GetFileAttributesA(output_path) == INVALID_FILE_ATTRIBUTES);
    FILE *exported = fopen(argv[19], "wb"); CHECK(exported);
    CHECK(fwrite(bytes, 1, length, exported) == length && !fclose(exported));
    xr_compile_resources_free(bytes); CHECK(!runtime_live && !runtime_bytes);
    printf("output lifetime/blocked write/workspace reclaimed/physical=0 PASS\n"); return 0;
}
typedef struct TestThread { int argc; char **argv; int result; } TestThread;
static DWORD WINAPI run_thread(void *context) {
    TestThread *test = context; test->result = run_test(test->argc, test->argv); return 0;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--winapi-control-child")) return 0;
    DWORD cold = 0, random_first = 0, random_last = 0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &cold));
    for (unsigned i = 0; i < 3; ++i) {
        UCHAR nonce[16]; CHECK(BCryptGenRandom(NULL, nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0);
        CHECK(GetProcessHandleCount(GetCurrentProcess(), &random_last));
        if (i == 1) random_first = random_last;
    }
    CHECK(random_first == random_last);
    wchar_t self[32768], command[32768]; CHECK(GetModuleFileNameW(NULL, self, 32768));
    DWORD process_first = 0, process_last = 0;
    for (unsigned i = 0; i < 2; ++i) {
        SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE}; HANDLE read = NULL, write = NULL;
        CHECK(CreatePipe(&read, &write, &security, 0)); CHECK(CloseHandle(read) && CloseHandle(write));
        CHECK(swprintf(command, 32768, L"\"%ls\" --winapi-control-child", self) > 0);
        STARTUPINFOW startup = {0}; startup.cb = sizeof(startup); PROCESS_INFORMATION process = {0};
        CHECK(CreateProcessW(self, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process));
        CHECK(WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0);
        CHECK(CloseHandle(process.hThread) && CloseHandle(process.hProcess));
        CHECK(GetProcessHandleCount(GetCurrentProcess(), &process_last));
        if (!i) process_first = process_last;
    }
    CHECK(process_first == process_last);
    printf("raw WinAPI controls cold=%lu BCrypt=%lu repeated-process=%lu\n", cold, random_last, process_last);
    /* Windows loader thread-local state is measured after the actual thread
     * terminates, matching the already qualified process/SDK lifetime gates. */
    DWORD before = 0, after = 0; CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
    TestThread test = {argc, argv, 1}; HANDLE thread = CreateThread(NULL, 0, run_thread, &test, 0, NULL); CHECK(thread);
    CHECK(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0 && CloseHandle(thread));
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
    printf("thread-exit handles before=%lu after=%lu\n", before, after); CHECK(before == after);
    return test.result;
}
