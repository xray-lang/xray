/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_toolchain_process_owned.c - Frozen requests and physical process cleanup
 */
#include "base/xmalloc.h"
#include "app/toolchain/xtc_process.h"
#include "app/toolchain/xtc_process_internal.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s (win=%lu)\n", __LINE__, #c, GetLastError()); exit(1); } } while (0)
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[4096];
static size_t attempts, fail_at = SIZE_MAX, live, physical_bytes;
static void *test_alloc(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *p = xr_malloc(bytes); if (!p) return NULL;
    for (size_t i = 0; i < 4096; ++i) if (!allocations[i].pointer) {
        allocations[i] = (Allocation){p, bytes}; ++live; physical_bytes += bytes; return p;
    }
    CHECK(false); return NULL;
}
static void test_free(void *p) {
    if (!p) return;
    for (size_t i = 0; i < 4096; ++i) if (allocations[i].pointer == p) {
        --live; physical_bytes -= allocations[i].bytes; allocations[i] = (Allocation){0}; xr_free(p); return;
    }
    CHECK(false);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc test_alloc
#define xr_free test_free
#include "base/xcompile_resources.c"
static size_t os_attempts, os_fail_at = SIZE_MAX;
static DWORD os_failure = ERROR_READ_FAULT;
static SRWLOCK os_fault_lock = SRWLOCK_INIT, handle_trace_lock = SRWLOCK_INIT;
static bool os_fail(void) {
    AcquireSRWLockExclusive(&os_fault_lock); bool fail = os_attempts++ == os_fail_at;
    DWORD error = os_failure; ReleaseSRWLockExclusive(&os_fault_lock);
    if (fail) SetLastError(error); return fail;
}
static HANDLE owned_handles[512];
static size_t physical_handles;
static void track_handle(HANDLE h) {
    CHECK(h && h != INVALID_HANDLE_VALUE);
    AcquireSRWLockExclusive(&handle_trace_lock);
    for (unsigned i = 0; i < 512; ++i) if (!owned_handles[i]) { owned_handles[i] = h; ++physical_handles; ReleaseSRWLockExclusive(&handle_trace_lock); return; }
    CHECK(false);
}
static BOOL test_close(HANDLE h) {
    AcquireSRWLockExclusive(&handle_trace_lock);
    BOOL ok = CloseHandle(h);
    if (ok) for (unsigned i = 0; i < 512; ++i) if (owned_handles[i] == h) { owned_handles[i] = NULL; --physical_handles; ReleaseSRWLockExclusive(&handle_trace_lock); return ok; }
    ReleaseSRWLockExclusive(&handle_trace_lock); CHECK(!ok); return ok;
}
static BOOL test_pipe(PHANDLE read, PHANDLE write, LPSECURITY_ATTRIBUTES attributes, DWORD size) {
    if (os_fail()) return FALSE;
    BOOL ok = CreatePipe(read, write, attributes, size); if (ok) { track_handle(*read); track_handle(*write); } return ok;
}
static BOOL test_duplicate(HANDLE source_process, HANDLE source, HANDLE target_process, LPHANDLE target, DWORD access, BOOL inherit, DWORD options) {
    if (os_fail()) return FALSE;
    BOOL ok = DuplicateHandle(source_process, source, target_process, target, access, inherit, options);
    if (ok) track_handle(*target); return ok;
}
static HANDLE test_job(LPSECURITY_ATTRIBUTES attributes, LPCWSTR name) {
    if (os_fail()) return NULL;
    HANDLE h = CreateJobObjectW(attributes, name); if (h) track_handle(h); return h;
}
static HANDLE test_file(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes, DWORD disposition, DWORD flags, HANDLE template_file) {
    if (os_fail()) return INVALID_HANDLE_VALUE;
    HANDLE h = CreateFileW(path, access, share, attributes, disposition, flags, template_file);
    if (h != INVALID_HANDLE_VALUE) track_handle(h); return h;
}
static DWORD observed_creation_flags;
static bool reject_program;
static BOOL test_process(LPCWSTR app, LPWSTR command, LPSECURITY_ATTRIBUTES process_attributes, LPSECURITY_ATTRIBUTES thread_attributes, BOOL inherit, DWORD flags, LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION info) {
    if (os_fail()) return FALSE;
    if (reject_program) { SetLastError(ERROR_READ_FAULT); return FALSE; }
    observed_creation_flags = flags;
    BOOL ok = CreateProcessW(app, command, process_attributes, thread_attributes, inherit, flags, env, cwd, startup, info);
    if (ok) { track_handle(info->hProcess); track_handle(info->hThread); } return ok;
}
static BOOL test_job_info(HANDLE job, JOBOBJECTINFOCLASS kind, LPVOID info, DWORD size) {
    if (os_fail()) return FALSE; return SetInformationJobObject(job, kind, info, size);
}
static BOOL test_job_assign(HANDLE job, HANDLE process) {
    if (os_fail()) return FALSE; return AssignProcessToJobObject(job, process);
}
static DWORD test_resume(HANDLE thread) { if (os_fail()) return (DWORD)-1; return ResumeThread(thread); }
static BOOL test_attributes(LPPROC_THREAD_ATTRIBUTE_LIST list, DWORD count, DWORD flags, PSIZE_T size) {
    if (os_fail()) return FALSE; return InitializeProcThreadAttributeList(list, count, flags, size);
}
static BOOL test_attribute_update(LPPROC_THREAD_ATTRIBUTE_LIST list, DWORD flags, DWORD_PTR attribute, PVOID value, SIZE_T size, PVOID previous, PSIZE_T returned) {
    if (os_fail()) return FALSE; return UpdateProcThreadAttribute(list, flags, attribute, value, size, previous, returned);
}
static int test_wide(UINT page, DWORD flags, LPCCH input, int count, LPWSTR output, int capacity) {
    if (os_fail()) return 0; return MultiByteToWideChar(page, flags, input, count, output, capacity);
}
static size_t search_calls;
static bool fail_search;
static DWORD test_search(LPCWSTR path, LPCWSTR file, LPCWSTR extension, DWORD size, LPWSTR output, LPWSTR *part) {
    ++search_calls;
    if (fail_search) { SetLastError(ERROR_FILE_NOT_FOUND); return 0; }
    return SearchPathW(path, file, extension, size, output, part);
}
static size_t environment_blocks;
static bool fail_environment;
static LPWCH test_environment(void) {
    if (fail_environment) { fail_environment = false; SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    LPWCH p = GetEnvironmentStringsW(); if (p) ++environment_blocks; return p;
}
static BOOL test_environment_free(LPWCH p) {
    BOOL ok = FreeEnvironmentStringsW(p); CHECK(ok && environment_blocks); --environment_blocks; return ok;
}
#define GetEnvironmentStringsW test_environment
#define FreeEnvironmentStringsW test_environment_free
static bool fail_read, fail_peek, fail_exit;
static BOOL test_read(HANDLE file, LPVOID bytes, DWORD size, LPDWORD count, LPOVERLAPPED overlapped) {
    if (fail_read) { fail_read = false; SetLastError(ERROR_READ_FAULT); return FALSE; }
    return ReadFile(file, bytes, size, count, overlapped);
}
static BOOL test_peek(HANDLE pipe, LPVOID bytes, DWORD size, LPDWORD count, LPDWORD available, LPDWORD left) {
    if (fail_peek) { fail_peek = false; SetLastError(ERROR_READ_FAULT); return FALSE; }
    return PeekNamedPipe(pipe, bytes, size, count, available, left);
}
static BOOL test_exit(HANDLE process, LPDWORD code) {
    if (fail_exit) { fail_exit = false; SetLastError(ERROR_READ_FAULT); return FALSE; }
    return GetExitCodeProcess(process, code);
}
#define SetInformationJobObject test_job_info
#define AssignProcessToJobObject test_job_assign
#define ResumeThread test_resume
#define InitializeProcThreadAttributeList test_attributes
#define UpdateProcThreadAttribute test_attribute_update
#define MultiByteToWideChar test_wide
#define SearchPathW test_search
#define ReadFile test_read
#define PeekNamedPipe test_peek
#define GetExitCodeProcess test_exit
#define CloseHandle test_close
#define CreatePipe test_pipe
#define DuplicateHandle test_duplicate
#define CreateJobObjectW test_job
#define CreateFileW test_file
#define CreateProcessW test_process
static bool fail_wait;
static HANDLE wait_lease_entered, wait_consumer_finished;
static DWORD held_wait_thread;
static volatile LONG wait_lease_entries;
static DWORD test_wait(HANDLE process, DWORD milliseconds) {
    if (wait_lease_entered && InterlockedIncrement(&wait_lease_entries) == 1) {
        held_wait_thread = GetCurrentThreadId(); CHECK(SetEvent(wait_lease_entered));
        CHECK(WaitForSingleObject(wait_consumer_finished, 5000) == WAIT_OBJECT_0);
        DWORD flags = 0; CHECK(GetHandleInformation(process, &flags));
    }
    if (fail_wait && !milliseconds) { fail_wait = false; SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    return WaitForSingleObject(process, milliseconds);
}
#define WaitForSingleObject test_wait
#include "os/win/proc_win.c"
#undef WaitForSingleObject
#include "app/toolchain/xtc_process.c"
#include "os/win/pipe_win.c"
#undef CloseHandle
#undef CreatePipe
#undef DuplicateHandle
#undef CreateJobObjectW
#undef CreateFileW
#undef CreateProcessW
#undef SetInformationJobObject
#undef AssignProcessToJobObject
#undef ResumeThread
#undef InitializeProcThreadAttributeList
#undef UpdateProcThreadAttribute
#undef MultiByteToWideChar
#undef SearchPathW
#undef ReadFile
#undef PeekNamedPipe
#undef GetExitCodeProcess
#undef GetEnvironmentStringsW
#undef FreeEnvironmentStringsW
#ifdef XTC_TEST_SDK
#include "toolchain/xr_xir_runtime_sdk.c"
#include "app/toolchain/xtc_xir_target.c"
#include "app/toolchain/xtc_xir_sysroot.c"
#endif

static char executable[32768], directory[32768];
static DWORD handles(void) { DWORD result; CHECK(GetProcessHandleCount(GetCurrentProcess(), &result)); return result; }
static XrCompileResources *ledger(XrCompileResourceLimits limits) {
    XrCompileResources *r = NULL; CHECK(xr_compile_resources_new(&limits, &r) == XR_COMPILE_RESOURCE_OK); return r;
}
static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
static void spec_init(XrProcessSpec *s, const char *mode) {
    xtc_process_spec_init(s, executable, 3000); s->cwd = directory; s->argv[1] = mode;
}
static bool cancel_now(void *context) { (void)context; return true; }
static void basic(void) {
    XrCompileResources *r = ledger(unlimited); XrToolchainProcess *p = NULL; XrProcessSpec s;
    spec_init(&s, "--child"); s.argv[2] = "space \\\" value"; s.env_keys[0] = "OWNED_VALUE"; s.env_values[0] = "yes"; s.env_count = 1;
    char argument[64]; strcpy(argument, s.argv[2]); s.argv[2] = argument;
    CHECK(SetEnvironmentVariableA("FORBIDDEN_PARENT_VALUE", "secret"));
    CHECK(xtc_process_prepare(r, &s, &p) == XTC_PROCESS_OK);
    memset(argument, 'x', sizeof(argument)); memset(&s, 0xCD, sizeof(s));
    XrProcessResult result = {0}; CHECK(xtc_process_run(p, NULL, NULL, &result) == XTC_PROCESS_OK);
    CHECK(result.exit_code == 23);
    CHECK(result.stdout_bytes.length == strlen("out|yes|absent|space \\\" value"));
    CHECK(!memcmp(result.stdout_bytes.data, "out|yes|absent|space \\\" value", result.stdout_bytes.length));
    CHECK(result.stderr_bytes.length == 3 && !memcmp(result.stderr_bytes.data, "err", 3));
    xtc_process_free(p); xr_compile_resources_release(r);
    CHECK(!memcmp(result.stderr_bytes.data, "err", 3)); xtc_process_result_free(&result);
    CHECK(!live && !physical_bytes && !physical_handles);
}
static void failures(void) {
    XrCompileResources *r = ledger(unlimited); XrToolchainProcess *p = NULL; XrProcessSpec s;
    XrProcessResult result = {0}, before = {0}; result.exit_code = 912; before = result;
    spec_init(&s, "--sleep"); s.timeout_ms = 30;
    CHECK(xtc_process_prepare(r, &s, &p) == XTC_PROCESS_OK);
    CHECK(xtc_process_run(p, cancel_now, NULL, &result) == XTC_PROCESS_CANCELLED);
    CHECK(!memcmp(&result, &before, sizeof(result)));
    CHECK(xtc_process_run(p, NULL, NULL, &result) == XTC_PROCESS_TIMEOUT);
    CHECK(!memcmp(&result, &before, sizeof(result)));
    fail_wait = true;
    CHECK(xtc_process_run(p, NULL, NULL, &result) == XTC_PROCESS_IO);
    CHECK(!memcmp(&result, &before, sizeof(result)));
    xtc_process_free(p); xr_compile_resources_release(r); CHECK(!live);
}
static bool cancel_after_child(void *context) {
    return GetFileAttributesA((const char *)context) != INVALID_FILE_ATTRIBUTES;
}
static void descendants(void) {
    char temp[MAX_PATH], path[MAX_PATH]; CHECK(GetTempPathA(sizeof(temp), temp));
    CHECK(snprintf(path, sizeof(path), "%sowned-process-%lu.pid", temp, GetCurrentProcessId()) > 0);
    (void)DeleteFileA(path);
    for (unsigned cancel = 0; cancel < 3; ++cancel) {
        XrCompileResources *r = ledger(unlimited); XrToolchainProcess *p = NULL; XrProcessSpec s;
        spec_init(&s, "--descendant-parent"); s.timeout_ms = 500; s.argv[2] = path; s.argv[3] = cancel == 2 ? "close-streams" : NULL;
        CHECK(xtc_process_prepare(r, &s, &p) == XTC_PROCESS_OK);
        XrProcessResult result = {0};
        CHECK(xtc_process_run(p, cancel == 1 ? cancel_after_child : NULL, path, &result) == (cancel == 2 ? XTC_PROCESS_OK : cancel ? XTC_PROCESS_CANCELLED : XTC_PROCESS_TIMEOUT));
        FILE *stream = fopen(path, "rb"); CHECK(stream); unsigned long child = 0; CHECK(fscanf(stream, "%lu", &child) == 1); CHECK(!fclose(stream));
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, child);
        if (process) { CHECK(WaitForSingleObject(process, 5000) == WAIT_OBJECT_0); CHECK(CloseHandle(process)); }
        CHECK(DeleteFileA(path));
        if (cancel == 2) { CHECK(result.exit_code == 0 && !result.stdout_bytes.length && !result.stderr_bytes.length); xtc_process_result_free(&result); }
        else CHECK(!result.stdout_bytes.data && !result.stderr_bytes.data);
        xtc_process_free(p); xr_compile_resources_release(r); CHECK(!live && !physical_handles);
    }
}
#ifdef XTC_TEST_SDK
static void sdk_target_process(const char *root) {
    XrCompileResources *r = ledger(unlimited); XrXirRuntimeSdk *sdk = NULL; XrXirTargetSnapshot *target = NULL;
    char manifest_path[32768]; CHECK(snprintf(manifest_path, sizeof(manifest_path), "%s/sdk_manifest.json", root) > 0);
    FILE *file = fopen(manifest_path, "rb"); CHECK(file); CHECK(!fseek(file, 0, SEEK_END)); long size = ftell(file); CHECK(size > 0); CHECK(!fseek(file, 0, SEEK_SET));
    void *manifest = NULL; CHECK(xr_compile_resources_alloc(r, (size_t)size, &manifest) == XR_COMPILE_RESOURCE_OK);
    CHECK(fread(manifest, 1, (size_t)size, file) == (size_t)size); CHECK(!fclose(file));
    XrXirRuntimeSdkRequest sdk_request = {root, manifest, (size_t)size, r};
    CHECK(xr_xir_runtime_sdk_load(&sdk_request, &sdk) == XR_XIR_SDK_OK);
    xr_compile_resources_free(manifest); memset(&sdk_request, 0xCD, sizeof(sdk_request));
    const char *args[] = {executable, "--child"};
    XrXirTargetDependency dependency = {executable, XR_XIR_TARGET_PROVIDER_SUPPORT};
    XrXirTargetCommand command = {directory, args, 2, NULL, 0};
    XrXirTargetRequest request = {r, "x86_64-windows-msvc", 3, 2, 11, &dependency, 1, &command, 1};
    CHECK(xtc_xir_target_capture(&request, &target) == XR_XIR_TARGET_OK);
    const XrXirTargetCommand *saved = xtc_xir_target_command(target, 0);
    XrProcessSpec spec; xtc_process_spec_init(&spec, saved->argv[0], 3000); spec.argv[1] = saved->argv[1]; spec.cwd = saved->cwd;
    XrToolchainProcess *owner = NULL; CHECK(xtc_process_prepare(r, &spec, &owner) == XTC_PROCESS_OK);
    XrCompileResourceStats stats; CHECK(xr_compile_resources_stats(r, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes > xr_xir_runtime_sdk_facts(sdk)->metadata_bytes);
    xr_compile_resources_release(r);
    CHECK(xr_xir_runtime_sdk_facts(sdk)->file_count == 136);
    memset(&request, 0xCD, sizeof(request)); memset(&spec, 0xCD, sizeof(spec));
    XrProcessResult result = {0}; CHECK(xtc_process_run(owner, NULL, NULL, &result) == XTC_PROCESS_OK);
    CHECK(result.exit_code == 23 && result.stdout_bytes.length == strlen("out|none|absent|"));
    CHECK(!memcmp(result.stdout_bytes.data, "out|none|absent|", result.stdout_bytes.length));
    xtc_process_free(owner); xtc_xir_target_free(target); xr_xir_runtime_sdk_free(sdk);
    CHECK(result.stderr_bytes.length == 3 && !memcmp(result.stderr_bytes.data, "err", 3));
    xtc_process_result_free(&result); CHECK(!live && !physical_bytes && !physical_handles);
}
#endif
static bool cancel_started(void *context) { return ++*(unsigned *)context > 1; }
static void os_failures(void) {
    XrProcessSpec spec; spec_init(&spec, "--sleep");
    XrCompileResources *r = ledger(unlimited); XrToolchainProcess *p = NULL;
    CHECK(xtc_process_prepare(r, &spec, &p) == XTC_PROCESS_OK);
    XrProcessResult result = {0}; unsigned checks = 0; size_t begin = os_attempts;
    CHECK(xtc_process_run(p, cancel_started, &checks, &result) == XTC_PROCESS_CANCELLED);
    size_t count = os_attempts - begin;
    xtc_process_free(p); xr_compile_resources_release(r);
    for (unsigned memory = 0; memory < 2; ++memory) for (size_t i = 0; i < count; ++i) {
        r = ledger(unlimited); p = NULL; CHECK(xtc_process_prepare(r, &spec, &p) == XTC_PROCESS_OK);
        os_fail_at = os_attempts + i; os_failure = memory ? ERROR_NOT_ENOUGH_MEMORY : ERROR_READ_FAULT; checks = 0;
        CHECK(xtc_process_run(p, cancel_started, &checks, &result) == (memory ? XTC_PROCESS_OUT_OF_MEMORY : XTC_PROCESS_IO));
        CHECK(!result.stdout_bytes.data && !result.stderr_bytes.data);
        xtc_process_free(p); xr_compile_resources_release(r); CHECK(!live && !physical_handles);
        os_fail_at = SIZE_MAX;
    }
    spec_init(&spec, "--child");
    for (unsigned operation = 0; operation < 3; ++operation) {
        r = ledger(unlimited); p = NULL; CHECK(xtc_process_prepare(r, &spec, &p) == XTC_PROCESS_OK);
        fail_read = !operation; fail_peek = operation == 1; fail_exit = operation == 2;
        CHECK(xtc_process_run(p, NULL, NULL, &result) == XTC_PROCESS_IO);
        CHECK(!fail_read && !fail_peek && !fail_exit);
        xtc_process_free(p); xr_compile_resources_release(r); CHECK(!live && !physical_handles);
    }
    printf("process startup OS failures: %zu IO + %zu OOM; read, peek, exit and wait IO\n", count, count);
}

static void unicode_environment(void) {
    const wchar_t *names[] = {L"XR_OWNED_测试", L"XR OWNED SPACE"};
    wchar_t previous[2][32768]; DWORD lengths[2];
    for (unsigned i = 0; i < 2; ++i) {
        lengths[i] = GetEnvironmentVariableW(names[i], previous[i], 32768);
        CHECK(lengths[i] < 32768); CHECK(SetEnvironmentVariableW(names[i], L"frozen"));
    }
    XrCompileResources *r = ledger(unlimited); XrProcessSpec s; spec_init(&s, "--unicode-env");
    s.environment_source = XTC_PROCESS_ENV_SNAPSHOT;
    XrToolchainProcess *p = NULL; CHECK(xtc_process_prepare(r, &s, &p) == XTC_PROCESS_OK);
    for (unsigned i = 0; i < 2; ++i) CHECK(SetEnvironmentVariableW(names[i], L"changed"));
    XrProcessResult result = {0}; CHECK(xtc_process_run(p, NULL, NULL, &result) == XTC_PROCESS_OK);
    CHECK(result.exit_code == 0 && result.stdout_bytes.length == 13);
    CHECK(!memcmp(result.stdout_bytes.data, "frozen|frozen", 13));
    xtc_process_result_free(&result); xtc_process_free(p); p = NULL;
    spec_init(&s, "--child"); s.env_count = 2;
    s.env_keys[0] = "XR_Ä"; s.env_keys[1] = "xr_ä"; s.env_values[0] = "one"; s.env_values[1] = "two";
    CHECK(xtc_process_prepare(r, &s, &p) == XTC_PROCESS_INVALID && !p);
    xr_compile_resources_release(r);
    for (unsigned i = 0; i < 2; ++i) CHECK(SetEnvironmentVariableW(names[i], lengths[i] ? previous[i] : NULL));
    CHECK(!live && !physical_bytes && !physical_handles);
}
static void check_hidden_drive(const wchar_t *expected) {
    LPWCH block = GetEnvironmentStringsW(); CHECK(block);
    unsigned found = 0;
    for (const wchar_t *entry = block; *entry; entry += wcslen(entry) + 1) {
        if (wcsncmp(entry, L"=C:=", 4)) continue;
        CHECK(!wcscmp(entry + 4, expected)); ++found;
    }
    CHECK(found == 1); CHECK(FreeEnvironmentStringsW(block));
}
static void hidden_environment(void) {
    /* Whole-block mutation runs only in its own CTest process. The child must
     * observe fixed values captured before the ambient environment changed. */
    CHECK(xr_proc_self_exe_path(executable, sizeof(executable)) == 0);
    CHECK(GetCurrentDirectoryA(sizeof(directory), directory));
    LPWCH previous = GetEnvironmentStringsW(); CHECK(previous);
    wchar_t first[] = L"=C:=C:\\xray-drive-A\0XR OWNED SPACE=space-A\0XR_OWNED_测试=unicode-A\0";
    wchar_t second[] = L"=C:=C:\\xray-drive-B\0XR OWNED SPACE=space-B\0XR_OWNED_测试=unicode-B\0";
    CHECK(SetEnvironmentStringsW(first)); check_hidden_drive(L"C:\\xray-drive-A");
    XrCompileResources *r = ledger(unlimited); XrProcessSpec spec; spec_init(&spec, "--hidden-env-read");
    spec.environment_source = XTC_PROCESS_ENV_SNAPSHOT; spec.cwd = NULL;
    XrToolchainProcess *process = NULL; CHECK(xtc_process_prepare(r, &spec, &process) == XTC_PROCESS_OK);
    CHECK(SetEnvironmentStringsW(second)); check_hidden_drive(L"C:\\xray-drive-B");
    XrProcessResult result = {0}; CHECK(xtc_process_run(process, NULL, NULL, &result) == XTC_PROCESS_OK);
    CHECK(result.exit_code == 0 && result.stderr_bytes.length == 0);
    const char expected[] = "hidden|unicode|space";
    CHECK(result.stdout_bytes.length == sizeof(expected) - 1);
    CHECK(!memcmp(result.stdout_bytes.data, expected, sizeof(expected) - 1));
    xtc_process_result_free(&result); xtc_process_free(process); process = NULL;
    for (unsigned snapshot = 0; snapshot < 2; ++snapshot) {
        spec_init(&spec, "--exit");
        spec.environment_source = snapshot ? XTC_PROCESS_ENV_SNAPSHOT : XTC_PROCESS_ENV_EXPLICIT;
        spec.env_keys[0] = "=C:"; spec.env_values[0] = "C:\\forged"; spec.env_count = 1;
        CHECK(xtc_process_prepare(r, &spec, &process) == XTC_PROCESS_INVALID && !process);
    }
    xr_compile_resources_release(r);
    CHECK(SetEnvironmentStringsW(previous)); CHECK(FreeEnvironmentStringsW(previous));
    CHECK(!live && !physical_bytes && !physical_handles && !environment_blocks);
    puts("hidden drive preserved, Unicode and space names readable, user drive keys rejected");
}
static void snapshot_failures(void) {
    XrProcessSpec spec; spec_init(&spec, "--exit"); spec.environment_source = XTC_PROCESS_ENV_SNAPSHOT;
    spec.cwd = NULL;
    size_t begin = attempts; XrCompileResources *r = ledger(unlimited); XrToolchainProcess *p = NULL;
    CHECK(xtc_process_prepare(r, &spec, &p) == XTC_PROCESS_OK); size_t count = attempts - begin;
    xtc_process_free(p); xr_compile_resources_release(r); CHECK(!environment_blocks);
    for (size_t i = 0; i < count; ++i) {
        fail_at = attempts + i; r = NULL; p = NULL;
        XrCompileResourceStatus rs = xr_compile_resources_new(&unlimited, &r);
        XrProcessStatus status = rs == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XTC_PROCESS_OUT_OF_MEMORY : XTC_PROCESS_OK;
        if (status == XTC_PROCESS_OK) status = xtc_process_prepare(r, &spec, &p);
        CHECK(status == XTC_PROCESS_OUT_OF_MEMORY && !p);
        xr_compile_resources_release(r); CHECK(!live && !physical_bytes && !environment_blocks);
    }
    fail_at = SIZE_MAX; r = ledger(unlimited); p = NULL; fail_environment = true;
    CHECK(xtc_process_prepare(r, &spec, &p) == XTC_PROCESS_OUT_OF_MEMORY && !p && !fail_environment);
    xr_compile_resources_release(r); CHECK(!live && !environment_blocks);
    printf("snapshot allocation failures: %zu; OS snapshot blocks returned on all paths\n", count);
}
static void detached_group_flag(void) {
    const char *args[] = {executable, "--exit", NULL}; XrProcSpawnOptions options = {0};
    options.memory = xr_proc_system_memory(); options.detached = true;
    XrProcId pid = XR_PROC_INVALID; observed_creation_flags = 0;
    size_t before_search = search_calls;
    CHECK(xr_proc_spawn(executable, args, &options, &pid) == XR_PROC_OK);
    CHECK(search_calls == before_search);
    CHECK(observed_creation_flags & CREATE_NEW_PROCESS_GROUP);
    CHECK(!(observed_creation_flags & CREATE_NO_WINDOW));
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (process) { CHECK(WaitForSingleObject(process, 5000) == WAIT_OBJECT_0); CHECK(CloseHandle(process)); }
    CHECK(!live && !physical_bytes && !physical_handles);
}
static void runtime_program_paths(void) {
    const char *exact[] = {"C:\\tools\\app.exe", "c:/tools/app.exe", "\\\\server\\share\\app.exe",
        "//server/share/app.exe", "\\\\?\\C:\\tools\\app.exe", "\\\\?\\UNC\\server\\share\\app.exe", "\\\\.\\C:\\tools\\app.exe"};
    const char *resolved[] = {"app.exe", "C:app.exe", "\\app.exe", "/app.exe", "C:\\tools\\app",
        "\\\\server\\share\\app", "C:\\tools.ext\\app", "\\\\?\\C:\\tools.ext\\app"};
    XrProcSpawnOptions options = {0}; options.memory = xr_proc_system_memory();
    fail_search = true; reject_program = true;
    for (unsigned group = 0; group < 2; ++group) {
        const char *const *paths = group ? resolved : exact;
        size_t count = group ? sizeof(resolved) / sizeof(*resolved) : sizeof(exact) / sizeof(*exact);
        for (size_t i = 0; i < count; ++i) {
            const char *args[] = {paths[i], NULL}; XrProcId pid = XR_PROC_INVALID;
            size_t before_search = search_calls;
            CHECK(xr_proc_spawn(paths[i], args, &options, &pid) == (group ? XR_PROC_UNRESOLVED : XR_PROC_IO));
            CHECK(pid == XR_PROC_INVALID && search_calls == before_search + group);
            CHECK(!live && !physical_bytes && !physical_handles && !environment_blocks);
        }
    }
    fail_search = false; reject_program = false;
}
typedef struct WaitRace { XrProcId pid; HANDLE start; bool poll; XrProcWaitResult result; int code; } WaitRace;
static DWORD WINAPI competing_wait(LPVOID input) {
    WaitRace *w = input; CHECK(WaitForSingleObject(w->start, INFINITE) == WAIT_OBJECT_0);
    if (!w->poll) w->result = xr_proc_wait(w->pid, &w->code) ? XR_PROC_WAIT_ERROR : XR_PROC_WAIT_EXITED;
    else do { w->result = xr_proc_try_wait(w->pid, &w->code); if (w->result == XR_PROC_WAIT_RUNNING) Sleep(1); } while (w->result == XR_PROC_WAIT_RUNNING);
    if (wait_consumer_finished && GetCurrentThreadId() != held_wait_thread) CHECK(SetEvent(wait_consumer_finished));
    return 0;
}
static void concurrent_waiters(void) {
    for (unsigned round = 0; round < 12; ++round) {
        const char *args[] = {executable, "--short-sleep", NULL}; XrProcSpawnOptions options = {0};
        options.memory = xr_proc_system_memory(); XrProcId pid = XR_PROC_INVALID;
        CHECK(xr_proc_spawn(executable, args, &options, &pid) == XR_PROC_OK);
        HANDLE start = CreateEventW(NULL, TRUE, FALSE, NULL); CHECK(start);
        WaitRace waiters[2] = {{pid, start, (round & 1) != 0, XR_PROC_WAIT_RUNNING, -9}, {pid, start, round != 0, XR_PROC_WAIT_RUNNING, -9}};
        if (!round) {
            wait_lease_entries = 0; wait_lease_entered = CreateEventW(NULL, TRUE, FALSE, NULL);
            wait_consumer_finished = CreateEventW(NULL, TRUE, FALSE, NULL); CHECK(wait_lease_entered && wait_consumer_finished);
        }
        HANDLE threads[2] = {CreateThread(NULL, 0, competing_wait, &waiters[0], 0, NULL), NULL};
        CHECK(threads[0]); CHECK(SetEvent(start));
        if (!round) CHECK(WaitForSingleObject(wait_lease_entered, 5000) == WAIT_OBJECT_0);
        threads[1] = CreateThread(NULL, 0, competing_wait, &waiters[1], 0, NULL); CHECK(threads[1]);
        CHECK(WaitForMultipleObjects(2, threads, TRUE, 5000) == WAIT_OBJECT_0);
        if (!round) {
            CHECK(wait_lease_entries == 2 && waiters[1].result == XR_PROC_WAIT_EXITED && waiters[0].result == XR_PROC_WAIT_ERROR);
            CHECK(CloseHandle(wait_lease_entered)); CHECK(CloseHandle(wait_consumer_finished));
            wait_lease_entered = wait_consumer_finished = NULL;
        }
        unsigned successes = 0;
        for (unsigned i = 0; i < 2; ++i) { successes += waiters[i].result == XR_PROC_WAIT_EXITED; CHECK(waiters[i].code == (waiters[i].result == XR_PROC_WAIT_EXITED ? 37 : -9)); CHECK(CloseHandle(threads[i])); }
        CHECK(successes == 1); CHECK(CloseHandle(start)); CHECK(!live && !physical_handles);
    }
    for (unsigned memory = 0; memory < 2; ++memory) {
        const char *args[] = {executable, "--exit", NULL}; XrProcSpawnOptions options = {0};
        options.memory = xr_proc_system_memory(); XrProcId pid = XR_PROC_INVALID;
        CHECK(xr_proc_spawn(executable, args, &options, &pid) == XR_PROC_OK);
        os_fail_at = os_attempts; os_failure = memory ? ERROR_NOT_ENOUGH_MEMORY : ERROR_READ_FAULT;
        CHECK(xr_proc_try_wait(pid, NULL) == XR_PROC_WAIT_ERROR);
        CHECK(xr_proc_last_error() == (memory ? XR_PROC_OUT_OF_MEMORY : XR_PROC_IO));
        os_fail_at = SIZE_MAX; int code = -1; CHECK(!xr_proc_wait(pid, &code) && code == 0);
        CHECK(!live && !physical_handles);
    }
}
static void cumulative_requests(void) {
    XrProcessSpec spec; spec_init(&spec, "--exit"); XrProcessResult result = {0};
    XrCompileResources *r = ledger(unlimited); XrToolchainProcessContext context = {r, XTC_PROCESS_OK};
    XrCompileResourceStats initial; CHECK(xr_compile_resources_stats(r, &initial) == XR_COMPILE_RESOURCE_OK);
    CHECK(xtc_process_request_run(&context, &spec, &result, NULL, 0) == XTC_PROCESS_OK);
    xtc_process_result_free(&result); XrCompileResourceStats first;
    CHECK(xr_compile_resources_stats(r, &first) == XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(r); CHECK(!live);
    XrCompileResourceLimits limits = unlimited; limits.allocated_bytes = initial.allocated_bytes + (first.allocated_bytes - initial.allocated_bytes) * 2 - 1;
    r = ledger(limits); context = (XrToolchainProcessContext){r, XTC_PROCESS_OK};
    CHECK(xtc_process_request_run(&context, &spec, &result, NULL, 0) == XTC_PROCESS_OK);
    xtc_process_result_free(&result);
    CHECK(xtc_process_request_run(&context, &spec, &result, NULL, 0) == XTC_PROCESS_BUDGET);
    size_t os_before = os_attempts, allocation_before = attempts;
    CHECK(xtc_process_request_run(&context, &spec, &result, NULL, 0) == XTC_PROCESS_BUDGET);
    CHECK(os_attempts == os_before && attempts == allocation_before);
    CHECK(!result.stdout_bytes.data && !result.stderr_bytes.data);
    xr_compile_resources_release(r); CHECK(!live && !physical_bytes && !physical_handles);
}

static void capture_idle_work(void) {
    XrCompileResources *r = ledger(unlimited); XtcCapture capture = {0}; XrPipe pipe;
    CHECK(xr_pipe_create(&pipe, NULL) == 0); XrCompileResourceStats before, after;
    CHECK(xr_compile_resources_stats(r, &before) == XR_COMPILE_RESOURCE_OK);
    for (unsigned i = 0; i < 20; ++i) CHECK(process_capture_read(r, pipe.read, &capture) == XTC_PROCESS_OK);
    CHECK(xr_compile_resources_stats(r, &after) == XR_COMPILE_RESOURCE_OK);
    CHECK(after.work - before.work == 20 && after.allocated_bytes == before.allocated_bytes);
    CHECK(!capture.eof); CHECK(!xr_pipe_close(pipe.write));
    CHECK(process_capture_read(r, pipe.read, &capture) == XTC_PROCESS_OK && capture.eof);
    CHECK(!xr_pipe_close(pipe.read)); xr_compile_resources_release(r); CHECK(!live && !physical_handles);
}
static void missing_inputs(void) {
    char missing[32768]; CHECK(snprintf(missing, sizeof(missing), "%s/missing-owned-process-%lu", directory, GetCurrentProcessId()) > 0);
    CHECK(GetFileAttributesA(missing) == INVALID_FILE_ATTRIBUTES);
    for (unsigned cwd = 0; cwd < 2; ++cwd) {
        XrCompileResources *r = ledger(unlimited); XrToolchainProcess *p = NULL; XrProcessSpec spec;
        spec_init(&spec, "--exit"); if (cwd) spec.cwd = missing; else spec.executable = missing;
        CHECK(xtc_process_prepare(r, &spec, &p) == XTC_PROCESS_OK); XrProcessResult result = {0};
        CHECK(xtc_process_run(p, NULL, NULL, &result) == XTC_PROCESS_UNRESOLVED);
        CHECK(!result.stdout_bytes.data && !result.stderr_bytes.data);
        xtc_process_free(p); xr_compile_resources_release(r); CHECK(!live && !physical_handles);
    }
}
static void bad_requests(void) {
    XrCompileResources *r = ledger(unlimited); XrToolchainProcess *p = NULL; XrProcessSpec s;
    spec_init(&s, "--child"); s.env_keys[0] = "A"; s.env_values[0] = "1"; s.env_keys[1] = "a"; s.env_values[1] = "2"; s.env_count = 2;
    CHECK(xtc_process_prepare(r, &s, &p) == XTC_PROCESS_INVALID && !p);
    spec_init(&s, "--child"); s.executable = "relative.exe";
    CHECK(xtc_process_prepare(r, &s, &p) == XTC_PROCESS_INVALID && !p);
    spec_init(&s, "--child"); s.argv[2] = "\xc3(";
    CHECK(xtc_process_prepare(r, &s, &p) == XTC_PROCESS_INVALID && !p);
    xr_compile_resources_release(r); CHECK(!live);
}
static void oom(void) {
    size_t begin = attempts; XrCompileResources *r = ledger(unlimited); XrProcessSpec s; spec_init(&s, "--large");
    XrToolchainProcess *p = NULL; XrProcessResult result = {0};
    CHECK(xtc_process_prepare(r, &s, &p) == XTC_PROCESS_OK);
    CHECK(xtc_process_run(p, NULL, NULL, &result) == XTC_PROCESS_OK);
    CHECK(result.stdout_bytes.length == 20000 && result.stderr_bytes.length == 20000);
    size_t count = attempts - begin;
    xtc_process_result_free(&result); xtc_process_free(p); xr_compile_resources_release(r); CHECK(!live);
    for (size_t i = 0; i < count; ++i) {
        fail_at = attempts + i; r = NULL; p = NULL; result = (XrProcessResult){0};
        XrCompileResourceStatus rs = xr_compile_resources_new(&unlimited, &r);
        XrProcessStatus status = rs == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XTC_PROCESS_OUT_OF_MEMORY : XTC_PROCESS_OK;
        if (status == XTC_PROCESS_OK) status = xtc_process_prepare(r, &s, &p);
        if (status == XTC_PROCESS_OK) status = xtc_process_run(p, NULL, NULL, &result);
        CHECK(status == XTC_PROCESS_OUT_OF_MEMORY);
        CHECK(!result.stdout_bytes.data && !result.stderr_bytes.data);
        xtc_process_free(p); xr_compile_resources_release(r); CHECK(!live && !physical_bytes && !physical_handles);
    }
    fail_at = SIZE_MAX; printf("owned process allocation failures: %zu\n", count);
}
static void budgets(void) {
    XrCompileResources *r = ledger(unlimited); XrProcessSpec s; spec_init(&s, "--child"); XrToolchainProcess *p = NULL;
    XrCompileResourceStats stats;
    CHECK(xtc_process_prepare(r, &s, &p) == XTC_PROCESS_OK);
    CHECK(xr_compile_resources_stats(r, &stats) == XR_COMPILE_RESOURCE_OK);
    uint64_t expected = 2 + sizeof(XrToolchainProcess) + 3 * (uint64_t)(strlen(executable) + strlen(directory) + strlen(executable) + strlen("--child")) + 4 * 3;
    CHECK(stats.work == expected);
    xtc_process_free(p); xr_compile_resources_release(r);
    for (unsigned field = 0; field < 3; ++field) for (unsigned less = 0; less < 2; ++less) {
        XrCompileResourceLimits limits = unlimited;
        if (!field) limits.allocated_bytes = stats.allocated_bytes - less;
        if (field == 1) limits.live_bytes = stats.peak_bytes - less;
        if (field == 2) limits.work = stats.work - less;
        r = ledger(limits); p = NULL;
        CHECK(xtc_process_prepare(r, &s, &p) == (less ? XTC_PROCESS_BUDGET : XTC_PROCESS_OK));
        xtc_process_free(p); xr_compile_resources_release(r); CHECK(!live);
    }
}
int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--sdk")) {
        if (!strcmp(argv[1], "--hidden-env-parent")) { hidden_environment(); return 0; }
        if (!strcmp(argv[1], "--hidden-env-read")) {
            check_hidden_drive(L"C:\\xray-drive-A");
            wchar_t value[32];
            CHECK(GetEnvironmentVariableW(L"XR_OWNED_测试", value, 32) == 9 && !wcscmp(value, L"unicode-A"));
            CHECK(GetEnvironmentVariableW(L"XR OWNED SPACE", value, 32) == 7 && !wcscmp(value, L"space-A"));
            fputs("hidden|unicode|space", stdout); return 0;
        }
        if (!strcmp(argv[1], "--descendant-parent")) {
            wchar_t self[32768], command[32768]; CHECK(GetModuleFileNameW(NULL, self, 32768));
            CHECK(swprintf(command, 32768, L"\"%ls\" %ls", self, argc > 3 ? L"--sleep-close" : L"--sleep") > 0);
            STARTUPINFOW startup = {0}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE); startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE); startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
            PROCESS_INFORMATION info = {0}; CHECK(CreateProcessW(self, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup, &info));
            FILE *file = fopen(argv[2], "wb"); CHECK(file); fprintf(file, "%lu", info.dwProcessId); CHECK(!fclose(file));
            CHECK(CloseHandle(info.hThread)); CHECK(CloseHandle(info.hProcess)); return 0;
        }
        if (!strcmp(argv[1], "--exit")) return 0;
        if (!strcmp(argv[1], "--short-sleep")) { Sleep(15); return 37; }
        if (!strcmp(argv[1], "--unicode-env")) {
            wchar_t first[32], second[32]; CHECK(GetEnvironmentVariableW(L"XR_OWNED_测试", first, 32)); CHECK(GetEnvironmentVariableW(L"XR OWNED SPACE", second, 32));
            printf("%ls|%ls", first, second); return 0;
        }
        if (!strcmp(argv[1], "--sleep-close")) { fclose(stdout); fclose(stderr); Sleep(10000); return 0; }
        if (!strcmp(argv[1], "--sleep")) { Sleep(10000); return 0; }
        if (!strcmp(argv[1], "--large")) { for (unsigned i = 0; i < 20000; ++i) { fputc('o', stdout); fputc('e', stderr); } return 0; }
        if (!strcmp(argv[1], "--child")) {
            const char *value = getenv("OWNED_VALUE"); printf("out|%s|%s|%s", value ? value : "none", getenv("FORBIDDEN_PARENT_VALUE") ? "present" : "absent", argc > 2 ? argv[2] : ""); fputs("err", stderr); return 23;
        }
        return 99;
    }
    CHECK(xr_proc_self_exe_path(executable, sizeof(executable)) == 0);
    CHECK(GetCurrentDirectoryA(sizeof(directory), directory));
    DWORD initial = handles(); basic(); DWORD after_spawn = handles(); missing_inputs();
    CHECK(!physical_handles); DWORD before = handles();
    printf("OS handle samples: initial=%lu first-spawn=%lu first-missing-input=%lu\n", initial, after_spawn, before);
    basic(); failures(); descendants(); os_failures(); bad_requests(); missing_inputs();
    capture_idle_work(); budgets(); oom(); unicode_environment(); snapshot_failures();
    detached_group_flag(); runtime_program_paths(); concurrent_waiters(); cumulative_requests();
#ifdef XTC_TEST_SDK
    if (argc == 3 && !strcmp(argv[1], "--sdk")) sdk_target_process(argv[2]);
#endif
    printf("handle baseline=%lu final=%lu\n", before, handles());
    CHECK(handles() == before); CHECK(!live && !physical_bytes && !physical_handles && !environment_blocks);
    puts("frozen process request, statuses and physical cleanup passed"); return 0;
}
