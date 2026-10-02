/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native_invocation_owner.c - Actual Source C and six observed MSVC runs
 */
#include "app/toolchain/xtc_xir_invocation.h"
#include "toolchain/xcompiler_session.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "invocation_allocator.h"
#ifdef INVOCATION_INJECTED
#include "invocation_faults.inc.h"
#endif
static bool cancel_immediately(void *context) { (void)context; return true; }

static void *read_input(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb"); CHECK(file && !fseek(file, 0, SEEK_END));
    long size = ftell(file); CHECK(size > 0 && !fseek(file, 0, SEEK_SET));
    void *bytes = malloc((size_t)size); CHECK(bytes);
    CHECK(fread(bytes, 1, (size_t)size, file) == (size_t)size && !fclose(file));
    *length = (size_t)size; return bytes;
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
    XrXirNativeProjectionRequest prepare = {product, "invocation_source", 16777216};
    XrXirNativeProjection *result = NULL;
    CHECK(xr_compile_native_projection_prepare(&prepare, &result, NULL) == XR_XIR_OK);
    xr_xir_compile_source_product_free(product); return result;
}
typedef struct CommandStorage {
    XrProcessSpec spec;
    char source[32768], object[32768], report[32768];
    char object_argument[32768], include[3][32768];
} CommandStorage;
static XrToolchainProcess *compile(XrCompileResources *resources, const char *compiler,
    const char *directory, const char *output_directory, const char *sdk, unsigned stage, CommandStorage *storage) {
    static const char *const fixed[] = {"/nologo", "/std:c11", "/utf-8", "/experimental:c11atomics",
        "/MD", "/O2", "/W4", "/WX", "/DNDEBUG", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN", "/D_CRT_SECURE_NO_WARNINGS"};
    CHECK(snprintf(storage->source, sizeof(storage->source), "%s/%s.c", directory, stage ? "launcher" : "generated") > 0);
    CHECK(snprintf(storage->object, sizeof(storage->object), "%s/%s.obj", output_directory, stage ? "launcher" : "generated") > 0);
    CHECK(snprintf(storage->report, sizeof(storage->report), "%s/%s.json", output_directory, stage ? "launcher" : "generated") > 0);
    CHECK(snprintf(storage->object_argument, sizeof(storage->object_argument), "/Fo%s", storage->object) > 0);
    xtc_process_spec_init(&storage->spec, compiler, 30000);
    storage->spec.environment_source = XTC_PROCESS_ENV_SNAPSHOT;
    storage->spec.cwd = directory; storage->spec.output_limit = 4 * 1024 * 1024;
    storage->spec.argv[0] = "independent-argv-zero";
    for (unsigned i = 0; i < 12; ++i) storage->spec.argv[i + 1] = fixed[i];
    const char *names[] = {"src", "include", "generated"};
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(snprintf(storage->include[i], sizeof(storage->include[i]), "/I%s/%s", sdk, names[i]) > 0);
        storage->spec.argv[13 + i] = storage->include[i];
    }
    storage->spec.argv[16] = "/c"; storage->spec.argv[17] = storage->source;
    storage->spec.argv[18] = storage->object_argument; storage->spec.argv[19] = "/sourceDependencies";
    storage->spec.argv[20] = storage->report;
    if (stage) storage->spec.argv[21] = "/DXIR_SDK_PROGRAM_SYMBOL=invocation_source_program";
    XrToolchainProcess *process = NULL;
    CHECK(xtc_process_prepare(resources, &storage->spec, &process) == XTC_PROCESS_OK);
    return process;
}
static int run_test(int argc, char **argv) {
    CHECK(argc == 16);
    const char *mode = argv[15];
#ifdef INVOCATION_INJECTED
    if (!strcmp(mode, "positive")) owner_record_boundaries(argv[2]);
#endif
    DWORD handles = 0; CHECK(GetProcessHandleCount(GetCurrentProcess(), &handles));
    XrCompileResourceLimits limits = sdk_unlimited;
    if (!strncmp(mode, "budget-work-", 12)) limits.work = strtoull(mode + 12, NULL, 10);
    if (!strncmp(mode, "budget-bytes-", 13)) limits.allocated_bytes = strtoull(mode + 13, NULL, 10);
    if (!strncmp(mode, "budget-live-", 12)) limits.live_bytes = strtoull(mode + 12, NULL, 10);
    XrCompileResources *resources = sdk_ledger(&limits);
    XrXirNativeProjection *project = projection(resources, argv[1], argv[2], argv[3]);
    char manifest_path[32768]; CHECK(snprintf(manifest_path, sizeof(manifest_path), "%s/sdk_manifest.json", argv[4]) > 0);
    size_t manifest_length = 0, launcher_length = 0;
    void *manifest = read_input(manifest_path, &manifest_length), *launcher = read_input(argv[8], &launcher_length);
    XrXirRuntimeSdkRequest load = {argv[4], manifest, manifest_length, resources};
    XrXirRuntimeSdk *sdk = NULL; CHECK(xr_xir_runtime_sdk_load(&load, &sdk) == XR_XIR_SDK_OK); free(manifest);
    CommandStorage commands[2]; memset(commands, 0, sizeof(commands));
    XrToolchainProcess *processes[3] = {NULL, NULL, NULL};
    for (unsigned i = 0; i < 2; ++i) processes[i] = compile(resources, argv[5], argv[7], argv[14], xr_xir_runtime_sdk_root(sdk), i, &commands[i]);
    char output[32768], report[32768], output_argument[32768], report_argument[32768];
    CHECK(snprintf(output, sizeof(output), "%s/program.exe", argv[14]) > 0);
    CHECK(snprintf(report, sizeof(report), "%s/actual.rsp", argv[14]) > 0);
    CHECK(snprintf(output_argument, sizeof(output_argument), "/out:%s", output) > 0);
    CHECK(snprintf(report_argument, sizeof(report_argument), "/LINKREPROFULLPATHRSP:%s", report) > 0);
    XrProcessSpec spec; xtc_process_spec_init(&spec, argv[6], 30000); spec.cwd = argv[7]; spec.output_limit = 4 * 1024 * 1024;
    spec.environment_source = XTC_PROCESS_ENV_SNAPSHOT;
    spec.argv[0] = "independent-link-argv-zero"; spec.argv[1] = "/nologo"; spec.argv[2] = "/incremental:no";
    spec.argv[3] = "/NODEFAULTLIB"; spec.argv[4] = "/verbose:lib"; spec.argv[5] = output_argument; spec.argv[6] = report_argument;
    spec.argv[7] = commands[0].object; spec.argv[8] = commands[1].object;
    static const char *const names[] = {"lib/xray_compile_resources.lib", "lib/xray_xir_admission.lib",
        "lib/xray_xir_declarations.lib", "lib/xray_xir_scalar.lib", "lib/xray_xir_runtime_host.lib"};
    for (unsigned i = 0; i < 5; ++i) CHECK(xr_xir_runtime_sdk_file(sdk, names[i], &spec.argv[9 + i]) == XR_XIR_SDK_OK);
    XrXirInvocationLibrary libraries[5];
    for (unsigned i = 0; i < 5; ++i) {
        libraries[i] = (XrXirInvocationLibrary){argv[9 + i], i == 4 ? XR_XIR_INVOCATION_SYSTEM : XR_XIR_INVOCATION_CRT};
        spec.argv[14 + i] = libraries[i].path;
    }
    CHECK(xtc_process_prepare(resources, &spec, &processes[2]) == XTC_PROCESS_OK);
    XrXirInvocationRequest request = {0}; request.projection = project; request.sdk = sdk;
    for (unsigned i = 0; i < 2; ++i) request.compile[i] = (XrXirInvocationCompile){processes[i], commands[i].source, commands[i].object, commands[i].report};
    request.link = processes[2]; request.input_directory = argv[7]; request.output_directory = argv[14];
    request.link_report = report; request.output = output;
    request.launcher = launcher; request.launcher_length = launcher_length; request.libraries = libraries; request.library_count = 5;
    request.limits = (XrXirInvocationLimits){{4 * 1024 * 1024, 32768, 4096}, 64 * 1024 * 1024, 4096};
    XrXirInvocationStatus expected = XR_XIR_INVOCATION_OK;
    XrCompileResources *foreign = NULL; XrXirRuntimeSdk *foreign_sdk = NULL; XrToolchainProcess *foreign_process = NULL;
    char alternate_directory[32768] = {0}, collision[32768] = {0};
    bool preallocation = false;
    void *large_launcher = NULL;
    if (!strncmp(mode, "budget-", 7)) expected = XR_XIR_INVOCATION_BUDGET;
    if (!strcmp(mode, "source-shape")) {
        request.limits.artifact_bytes = xr_compile_native_projection_source(project)->length - 1;
        expected = XR_XIR_INVOCATION_BUDGET;
    } else if (!strcmp(mode, "launcher-shape")) {
        request.limits.artifact_bytes = xr_compile_native_projection_source(project)->length;
        request.launcher_length = (size_t)request.limits.artifact_bytes + 1;
        large_launcher = malloc(request.launcher_length); CHECK(large_launcher);
        memset(large_launcher, ' ', request.launcher_length); request.launcher = large_launcher;
        expected = XR_XIR_INVOCATION_BUDGET;
    }
    if (!strcmp(mode, "bad-recipe")) {
        commands[0].spec.argv[2] = "/std:c99";
        xtc_process_free(processes[0]); processes[0] = NULL;
        CHECK(xtc_process_prepare(resources, &commands[0].spec, &processes[0]) == XTC_PROCESS_OK);
        request.compile[0].process = processes[0]; expected = XR_XIR_INVOCATION_UNSUPPORTED;
    } else if (!strcmp(mode, "cancel")) {
        request.cancelled = cancel_immediately; expected = XR_XIR_INVOCATION_CANCELLED;
    } else if (!strcmp(mode, "timeout")) {
        commands[0].spec.timeout_ms = 1;
        xtc_process_free(processes[0]); processes[0] = NULL;
        CHECK(xtc_process_prepare(resources, &commands[0].spec, &processes[0]) == XTC_PROCESS_OK);
        request.compile[0].process = processes[0]; expected = XR_XIR_INVOCATION_TIMEOUT;
    } else if (!strcmp(mode, "child")) {
        request.launcher = "#error invocation_actual_child_failure\n";
        request.launcher_length = strlen(request.launcher); expected = XR_XIR_INVOCATION_CHILD_FAILED;
    } else if (!strcmp(mode, "nonempty")) {
        CHECK(snprintf(collision, sizeof(collision), "%s/existing.txt", request.input_directory) > 0);
        FILE *file = fopen(collision, "wb"); CHECK(file && fputs("preserved", file) >= 0 && !fclose(file));
        expected = XR_XIR_INVOCATION_INVALID;
    } else if (!strcmp(mode, "same-directory")) {
        request.output_directory = request.input_directory; expected = XR_XIR_INVOCATION_INVALID;
    } else if (!strcmp(mode, "case-directory")) {
        size_t length = strlen(request.input_directory); CHECK(length < sizeof(alternate_directory));
        for (size_t i = 0; i <= length; ++i) {
            char c = request.input_directory[i]; alternate_directory[i] = c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
        }
        request.output_directory = alternate_directory; expected = XR_XIR_INVOCATION_INVALID;
    } else if (!strcmp(mode, "ancestor-directory")) {
        CHECK(snprintf(alternate_directory, sizeof(alternate_directory), "%s/nested", request.input_directory) > 0);
        CHECK(CreateDirectoryA(alternate_directory, NULL)); request.output_directory = alternate_directory;
        expected = XR_XIR_INVOCATION_INVALID;
    } else if (!strcmp(mode, "reverse-ancestor")) {
        request.output_directory = argv[1]; expected = XR_XIR_INVOCATION_INVALID;
    } else if (!strncmp(mode, "foreign-", 8) && strcmp(mode, "foreign-guard")) {
        foreign = sdk_ledger(&sdk_unlimited); preallocation = true; expected = XR_XIR_INVOCATION_INVALID;
        if (!strcmp(mode, "foreign-sdk")) {
            manifest = read_input(manifest_path, &manifest_length);
            load.resources = foreign; load.manifest = manifest; load.manifest_length = manifest_length;
            CHECK(xr_xir_runtime_sdk_load(&load, &foreign_sdk) == XR_XIR_SDK_OK); free(manifest);
            request.sdk = foreign_sdk;
        } else {
            unsigned stage = (unsigned)strtoul(mode + 8, NULL, 10); CHECK(stage < 3);
            CHECK(xtc_process_prepare(foreign, stage < 2 ? &commands[stage].spec : &spec, &foreign_process) == XTC_PROCESS_OK);
            if (stage < 2) request.compile[stage].process = foreign_process; else request.link = foreign_process;
        }
    }
#ifdef INVOCATION_INJECTED
    invocation_other_source = commands[1].source;
    if (!strncmp(mode, "oom-", 4)) { owner_allocation_fail = strtoull(mode + 4, NULL, 10); expected = XR_XIR_INVOCATION_OUT_OF_MEMORY; }
    if (!strncmp(mode, "process-oom-", 12)) { invocation_process_fail = (unsigned)strtoul(mode + 12, NULL, 10); expected = XR_XIR_INVOCATION_OUT_OF_MEMORY; }
    if (!strncmp(mode, "io-", 3)) { owner_compare_fail = strtoull(mode + 3, NULL, 10); expected = XR_XIR_INVOCATION_IO; }
    if (!strncmp(mode, "osoom-", 6)) { owner_compare_fail = strtoull(mode + 6, NULL, 10); owner_compare_error = ERROR_NOT_ENOUGH_MEMORY; expected = XR_XIR_INVOCATION_OUT_OF_MEMORY; }
    if (!strcmp(mode, "replay")) { invocation_tamper = 1; expected = XR_XIR_INVOCATION_REPLAY_MISMATCH; }
    if (!strcmp(mode, "includes-replay")) { invocation_tamper = 2; expected = XR_XIR_INVOCATION_REPLAY_MISMATCH; }
    if (!strcmp(mode, "link-replay")) { invocation_tamper = 3; expected = XR_XIR_INVOCATION_REPLAY_MISMATCH; }
    if (!strcmp(mode, "image-replay")) { invocation_image_difference = true; expected = XR_XIR_INVOCATION_REPLAY_MISMATCH; }
    if (!strcmp(mode, "launcher-race") || !strcmp(mode, "source-race")) {
        owner_tamper_write = !strcmp(mode, "launcher-race") ? 1 : 0; expected = XR_XIR_INVOCATION_REPLAY_MISMATCH;
    }
#endif
    XrCompileResourceStats before_guard;
    CHECK(xr_compile_resources_stats(resources, &before_guard) == XR_COMPILE_RESOURCE_OK);
    XrXirNamespaceRoot guard_root = {request.input_directory, XR_XIR_NAMESPACE_DIRECTORY};
    XrXirNamespaceRequest guard_request = {&guard_root, 1, {1, 8, 8, 32768}};
    XrXirNamespace *guard = NULL;
    if (!strcmp(mode, "foreign-guard")) {
        foreign = sdk_ledger(&sdk_unlimited); expected = XR_XIR_INVOCATION_INVALID; preallocation = true;
    }
    CHECK(xtc_xir_namespace_new(!strcmp(mode, "foreign-guard") ? foreign : resources,
        &guard_request, &guard) == XR_XIR_NAMESPACE_OK);
    request.namespace_owner = guard;
    if (!strcmp(mode, "null-guard")) {
        request.namespace_owner = NULL; expected = XR_XIR_INVOCATION_INVALID; preallocation = true;
    } else if (!strcmp(mode, "armed-guard") || !strcmp(mode, "closing-guard") || !strcmp(mode, "failed-guard")) {
        CHECK(xtc_xir_namespace_arm(guard) == XR_XIR_NAMESPACE_OK);
        expected = XR_XIR_INVOCATION_INVALID; preallocation = true;
#ifdef INVOCATION_INJECTED
        if (!strcmp(mode, "failed-guard")) {
            guard_input_path = request.input_directory; guard_change_directory();
            CHECK(xtc_xir_namespace_check(guard) == XR_XIR_NAMESPACE_BROKEN);
        }
        if (!strcmp(mode, "closing-guard")) {
            guard_hold_cancel = true;
            CHECK(xtc_xir_namespace_close(&guard, 0) == XR_XIR_NAMESPACE_PENDING && guard && guard_cancel_holds);
        }
#endif
    }
#ifdef INVOCATION_INJECTED
    guard_input_path = request.input_directory;
    for (unsigned i = 0; i < 2; ++i) guard_source_paths[i] = commands[i].source;
    if (!strcmp(mode, "guard-break")) { guard_break_check = 2; expected = XR_XIR_INVOCATION_BROKEN; }
    if (!strcmp(mode, "guard-final-break")) { guard_break_check = 7; expected = XR_XIR_INVOCATION_BROKEN; }
    if (!strcmp(mode, "guard-arm-oom")) { guard_arm_oom = true; expected = XR_XIR_INVOCATION_OUT_OF_MEMORY; }
    if (!strcmp(mode, "guard-check-budget")) { guard_check_budget = true; expected = XR_XIR_INVOCATION_BUDGET; }
    if (!strcmp(mode, "guard-check-io")) { guard_check_io = true; expected = XR_XIR_INVOCATION_IO; }
#endif
    XrXirInvocation *owner = NULL; XrXirInvocationDiagnostic diagnostic = {0};
    if (!strcmp(mode, "occupied-out")) {
        owner = (XrXirInvocation *)(uintptr_t)1; expected = XR_XIR_INVOCATION_INVALID; preallocation = true;
    }
    DWORD prepared_handles = 0; CHECK(GetProcessHandleCount(GetCurrentProcess(), &prepared_handles));
    printf("handles initial=%lu prepared=%lu\n", (unsigned long)handles, (unsigned long)prepared_handles); fflush(stdout);
    XrCompileResourceStats before, after_run;
    CHECK(xr_compile_resources_stats(resources, &before) == XR_COMPILE_RESOURCE_OK);
    ULONGLONG started = GetTickCount64();
    size_t allocation_start = runtime_attempts;
    XrXirInvocationStatus status;
#ifdef INVOCATION_INJECTED
    if (!strncmp(mode, "io-all-", 7)) {
        size_t count = strtoull(mode + 7, NULL, 10);
        CHECK(count);
        const DWORD errors[] = {ERROR_ACCESS_DENIED, ERROR_NOT_ENOUGH_MEMORY, ERROR_OUTOFMEMORY};
        for (unsigned error = 0; error < 3; ++error) for (size_t point = 0; point < count; ++point) {
            owner_compare_count = 0; owner_compare_fail = point; owner_compare_error = errors[error];
            expected = error ? XR_XIR_INVOCATION_OUT_OF_MEMORY : XR_XIR_INVOCATION_IO;
            status = xtc_xir_invocation_run(&request, &owner, &diagnostic);
            CHECK(status == expected && !owner && owner_compare_count == point + 1 && !invocation_runs);
            XrCompileResourceStats current; CHECK(xr_compile_resources_stats(resources, &current) == XR_COMPILE_RESOURCE_OK);
            CHECK(current.live_bytes == before.live_bytes);
        }
        status = expected;
        printf("all %zu actual owner ordinal calls x IO/two OS OOM; unchanged output/live PASS\n", count);
    } else
#endif
    status = xtc_xir_invocation_run(&request, &owner, &diagnostic);
    CHECK(xr_compile_resources_stats(resources, &after_run) == XR_COMPILE_RESOURCE_OK);
    printf("invocation cost ms=%llu malloc=%zu bytes=%llu peak=%llu live=%llu work=%llu\n",
        (unsigned long long)(GetTickCount64() - started), runtime_attempts - allocation_start,
        (unsigned long long)(after_run.allocated_bytes - before.allocated_bytes),
        (unsigned long long)after_run.peak_bytes, (unsigned long long)after_run.live_bytes,
        (unsigned long long)(after_run.work - before.work));
    printf("ledger before: allocated=%llu peak=%llu live=%llu work=%llu\n", (unsigned long long)before.allocated_bytes,
        (unsigned long long)before.peak_bytes, (unsigned long long)before.live_bytes, (unsigned long long)before.work);
    printf("invocation status=%d stage=%d pass=%d domain=%d code=%d exit=%d\n", status, diagnostic.stage,
        diagnostic.pass, diagnostic.domain, diagnostic.code, diagnostic.exit_code); fflush(stdout);
    CHECK(status == expected);
#ifdef INVOCATION_INJECTED
    printf("owner direct allocations=%zu comparisons=%zu attempted-runs=%u\n", owner_allocation_count, owner_compare_count, invocation_runs);
#endif
    if (preallocation) CHECK(runtime_attempts == allocation_start && !memcmp(&before, &after_run, sizeof(before)));
#ifdef INVOCATION_INJECTED
    if (!strcmp(mode, "guard-pending")) {
        CHECK(status == XR_XIR_INVOCATION_OK && owner && guard && guard_checks == 7);
        XrCompileResourceStats retained, pending;
        CHECK(xr_compile_resources_stats(resources, &retained) == XR_COMPILE_RESOURCE_OK);
        guard_hold_cancel = true;
        CHECK(xtc_xir_namespace_close(&guard, 0) == XR_XIR_NAMESPACE_PENDING && guard && guard_cancel_holds);
        CHECK(xr_compile_resources_stats(resources, &pending) == XR_COMPILE_RESOURCE_OK);
        CHECK(!memcmp(&retained, &pending, sizeof(retained)));
        CHECK(xtc_xir_invocation_facts(owner)->completed_runs == 6);
        CHECK(GetFileAttributesA(request.output) != INVALID_FILE_ATTRIBUTES);
        puts("upper operation, output lease and directory retained through real PENDING PASS");
    }
    guard_hold_cancel = false;
#endif
    XrXirNamespaceStatus closed = XR_XIR_NAMESPACE_PENDING;
    for (unsigned attempt = 0; guard && attempt < 100; ++attempt) {
        closed = xtc_xir_namespace_close(&guard, 100);
        CHECK(closed == XR_XIR_NAMESPACE_OK || closed == XR_XIR_NAMESPACE_PENDING);
    }
    CHECK(closed == XR_XIR_NAMESPACE_OK && !guard);
    XrCompileResourceStats after_guard;
    CHECK(xr_compile_resources_stats(resources, &after_guard) == XR_COMPILE_RESOURCE_OK);
    if (expected != XR_XIR_INVOCATION_OK) {
        if (!strcmp(mode, "occupied-out")) { CHECK(owner == (XrXirInvocation *)(uintptr_t)1); owner = NULL; }
        CHECK(!owner && before_guard.live_bytes == after_guard.live_bytes);
#ifdef INVOCATION_INJECTED
        if (!strcmp(mode, "guard-break") || !strcmp(mode, "guard-final-break")) {
            CHECK(diagnostic.domain == XR_XIR_INVOCATION_NAMESPACE && diagnostic.code == XR_XIR_NAMESPACE_BROKEN);
            CHECK(invocation_runs == (!strcmp(mode, "guard-break") ? 2u : 6u));
            CHECK(guard_checks == guard_break_check);
            CHECK(diagnostic.stage == (!strcmp(mode, "guard-break") ? XR_XIR_INVOCATION_GENERATED : XR_XIR_INVOCATION_NO_STAGE));
            CHECK(diagnostic.pass == (!strcmp(mode, "guard-break") ? XR_XIR_INVOCATION_REPLAY : XR_XIR_INVOCATION_NO_PASS));
        }
        if (!strcmp(mode, "guard-check-budget") || !strcmp(mode, "guard-check-io")) {
            CHECK(diagnostic.domain == XR_XIR_INVOCATION_NAMESPACE && invocation_runs == 1 && guard_checks == 1);
            CHECK(diagnostic.code == (!strcmp(mode, "guard-check-budget") ? XR_XIR_NAMESPACE_BUDGET : XR_XIR_NAMESPACE_IO));
            CHECK(diagnostic.stage == XR_XIR_INVOCATION_GENERATED && diagnostic.pass == XR_XIR_INVOCATION_OBSERVE);
        }
        if (!strcmp(mode, "cancel")) CHECK(!guard_checks && diagnostic.domain == XR_XIR_INVOCATION_PROCESS);
        if (!strcmp(mode, "guard-arm-oom")) {
            CHECK(diagnostic.domain == XR_XIR_INVOCATION_NAMESPACE && diagnostic.code == XR_XIR_NAMESPACE_OUT_OF_MEMORY);
            CHECK(!invocation_runs && !guard_checks && diagnostic.stage == XR_XIR_INVOCATION_NO_STAGE);
        }
#endif
        if (!strcmp(mode, "bad-recipe")) CHECK(diagnostic.stage == XR_XIR_INVOCATION_GENERATED && diagnostic.pass == XR_XIR_INVOCATION_NO_PASS);
        if (!strcmp(mode, "child")) CHECK(diagnostic.stage == XR_XIR_INVOCATION_LAUNCHER && diagnostic.pass == XR_XIR_INVOCATION_OBSERVE && diagnostic.exit_code);
        if (!strcmp(mode, "timeout") || !strcmp(mode, "cancel"))
            CHECK(diagnostic.stage == XR_XIR_INVOCATION_GENERATED && diagnostic.pass == XR_XIR_INVOCATION_OBSERVE);
        if (!strcmp(mode, "replay")) CHECK(diagnostic.stage == XR_XIR_INVOCATION_GENERATED && diagnostic.pass == XR_XIR_INVOCATION_REPLAY);
        if (!strcmp(mode, "includes-replay") || !strcmp(mode, "image-replay"))
            CHECK(diagnostic.stage == XR_XIR_INVOCATION_GENERATED && diagnostic.pass == XR_XIR_INVOCATION_REPLAY);
        if (!strcmp(mode, "link-replay")) CHECK(diagnostic.stage == XR_XIR_INVOCATION_LINK && diagnostic.pass == XR_XIR_INVOCATION_REPLAY);
        if (!strcmp(mode, "source-shape") || !strcmp(mode, "source-race"))
            CHECK(diagnostic.stage == XR_XIR_INVOCATION_GENERATED && diagnostic.pass == XR_XIR_INVOCATION_NO_PASS);
        if (!strcmp(mode, "launcher-shape") || !strcmp(mode, "launcher-race"))
            CHECK(diagnostic.stage == XR_XIR_INVOCATION_LAUNCHER && diagnostic.pass == XR_XIR_INVOCATION_NO_PASS);
        if (!strcmp(mode, "same-directory") || !strcmp(mode, "case-directory") ||
            !strcmp(mode, "ancestor-directory") || !strcmp(mode, "reverse-ancestor"))
            CHECK(diagnostic.stage == XR_XIR_INVOCATION_NO_STAGE && diagnostic.pass == XR_XIR_INVOCATION_NO_PASS);
        if (collision[0]) {
            size_t bytes = 0; void *text = read_input(collision, &bytes);
            CHECK(bytes == 9 && !memcmp(text, "preserved", 9)); free(text);
        }
    } else CHECK(owner);
    xtc_process_free(foreign_process); xr_xir_runtime_sdk_free(foreign_sdk); xr_compile_resources_release(foreign);
    for (unsigned i = 0; i < 3; ++i) xtc_process_free(processes[i]);
    xr_compile_native_projection_owner_free(project); xr_xir_runtime_sdk_free(sdk); free(launcher); free(large_launcher);
    xr_compile_resources_release(resources);
    const XrXirInvocationFacts *facts = xtc_xir_invocation_facts(owner);
    if (!owner) {
        CHECK(!runtime_live && !runtime_bytes); printf("typed failure %s; unchanged output/live; physical heap zero PASS\n", mode); return 0;
    }
#ifdef INVOCATION_INJECTED
    CHECK(guard_checks == 7 && invocation_runs == 6);
#endif
    CHECK(facts && facts->kind == XR_XIR_INVOCATION_LOCKED_REPLAY_FACTS && facts->completed_runs == 6);
    CHECK(xtc_xir_invocation_resources(owner) == resources && !strcmp(facts->projection.prefix, "invocation_source"));
    CHECK(strcmp(xtc_xir_invocation_command(owner, XR_XIR_INVOCATION_GENERATED)->executable,
        xtc_xir_invocation_command(owner, XR_XIR_INVOCATION_GENERATED)->argv[0]));
    uint64_t output_length = 0;
    unsigned kinds[9] = {0};
    for (uint32_t i = 0; i < facts->file_count; ++i) {
        const XrXirInvocationFile *file = xtc_xir_invocation_file(owner, i);
        CHECK(file && file->kind < 9 && file->path && file->length); ++kinds[file->kind];
        if (file->kind == XR_XIR_INVOCATION_OUTPUT) output_length = file->length;
    }
    CHECK(kinds[XR_XIR_INVOCATION_SOURCE] == 2 && kinds[XR_XIR_INVOCATION_OBJECT] == 2 &&
        kinds[XR_XIR_INVOCATION_SDK_ARCHIVE] == 5 && kinds[XR_XIR_INVOCATION_CRT] == 4 &&
        kinds[XR_XIR_INVOCATION_SYSTEM] == 1 && kinds[XR_XIR_INVOCATION_REPORT] == 3 && kinds[XR_XIR_INVOCATION_OUTPUT] == 1 &&
        kinds[XR_XIR_INVOCATION_HEADER] && kinds[XR_XIR_INVOCATION_PROVIDER_IMAGE]);
    printf("six calls; %u observed files; dead producers; frozen complete commands PASS\n", facts->file_count);
    void *bytes = NULL; size_t length = 0;
    CHECK(output_length && output_length <= SIZE_MAX);
    size_t expected_length = 0; void *expected_output = read_input(output, &expected_length);
    CHECK(expected_length == output_length);
    HANDLE write = CreateFileA(output, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(write == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);
    CHECK(xtc_xir_invocation_read_output(owner, output_length, &bytes, &length) == XR_XIR_INVOCATION_OK);
    CHECK(bytes && length == output_length && length > 2 && !memcmp(bytes, expected_output, length));
    void *again = NULL; size_t again_length = 0;
    CHECK(xtc_xir_invocation_read_output(owner, output_length, &again, &again_length) == XR_XIR_INVOCATION_OK);
    CHECK(again_length == length && !memcmp(bytes, again, length)); xr_compile_resources_free(again);
    void *sentinel = (void *)(uintptr_t)1; size_t occupied_length = 0;
    size_t attempts = runtime_attempts;
    CHECK(xtc_xir_invocation_read_output(owner, output_length, &sentinel, &occupied_length) == XR_XIR_INVOCATION_INVALID);
    CHECK(sentinel == (void *)(uintptr_t)1 && !occupied_length);
    sentinel = NULL; occupied_length = 1;
    CHECK(xtc_xir_invocation_read_output(owner, output_length, &sentinel, &occupied_length) == XR_XIR_INVOCATION_INVALID);
    CHECK(!sentinel && occupied_length == 1 && runtime_attempts == attempts);
    again = NULL; again_length = 0;
    if (!strcmp(mode, "read-oom")) {
        runtime_fail_at = runtime_attempts;
        CHECK(xtc_xir_invocation_read_output(owner, output_length, &again, &again_length) == XR_XIR_INVOCATION_OUT_OF_MEMORY);
        runtime_fail_at = SIZE_MAX; CHECK(runtime_attempts == attempts + 1);
    } else {
        CHECK(xtc_xir_invocation_read_output(owner, output_length - 1, &again, &again_length) == XR_XIR_INVOCATION_BUDGET);
        CHECK(runtime_attempts == attempts);
    }
    CHECK(!again && !again_length);
    xtc_xir_invocation_free(owner);
    CHECK(length == output_length && !memcmp(bytes, expected_output, length) && runtime_live);
    free(expected_output);
    xr_compile_resources_free(bytes); CHECK(!runtime_live && !runtime_bytes);
    puts("same-handle exact repeated output reads, bothout failure, dead producers/result, physical zero PASS");
    DWORD after = 0; CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
    printf("handles initial=%lu after=%lu\n", (unsigned long)handles, (unsigned long)after); fflush(stdout);
    puts("compiler physical heap zero; disk artifacts remain caller-directory-owned PASS"); return 0;
}
typedef struct InvocationThread { int argc; char **argv; } InvocationThread;
static DWORD WINAPI invocation_thread(void *context) {
    InvocationThread *test = context; return (DWORD)run_test(test->argc, test->argv);
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--native-control-child")) return 0;
    /* A pure WinAPI child is an explicit control for process-wide lazy state.
     * Windows also retains its debugger object until the calling thread exits. */
    wchar_t self[32768], command[32768];
    CHECK(GetModuleFileNameW(NULL, self, 32768));
    DWORD initial = 0, first = 0; CHECK(GetProcessHandleCount(GetCurrentProcess(), &initial));
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
        for (unsigned i = 0; i < 2; ++i) {
            HANDLE read = NULL, write = NULL;
            CHECK(CreatePipe(&read, &write, &security, 0));
            CHECK(CloseHandle(read) && CloseHandle(write));
        }
        CHECK(swprintf(command, 32768, L"\"%ls\" --native-control-child", self) > 0);
        STARTUPINFOW startup = {0}; startup.cb = sizeof(startup); PROCESS_INFORMATION process = {0};
        CHECK(CreateProcessW(self, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process));
        CHECK(WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0);
        CHECK(CloseHandle(process.hThread) && CloseHandle(process.hProcess));
        DWORD current = 0; CHECK(GetProcessHandleCount(GetCurrentProcess(), &current));
        if (!repeat) first = current; else CHECK(current == first);
    }
    DWORD baseline = 0; CHECK(GetProcessHandleCount(GetCurrentProcess(), &baseline));
    printf("pure WinAPI handles initial=%lu first=%lu repeated=%lu\n", (unsigned long)initial,
        (unsigned long)first, (unsigned long)baseline); fflush(stdout);
    InvocationThread test = {argc, argv}; HANDLE thread = CreateThread(NULL, 0, invocation_thread, &test, 0, NULL); CHECK(thread);
    CHECK(WaitForSingleObject(thread, 300000) == WAIT_OBJECT_0); DWORD code = 1;
    CHECK(GetExitCodeThread(thread, &code) && !code && CloseHandle(thread));
    DWORD after = 0; CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
    printf("thread scope handles baseline=%lu after=%lu\n", (unsigned long)baseline, (unsigned long)after);
    CHECK(after == baseline); return 0;
}
