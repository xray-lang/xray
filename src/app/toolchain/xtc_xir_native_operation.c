/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_native_operation.c - Compose existing owners without losing cleanup
 */
#include "xtc_xir_native_operation.h"
#include "xtc_xir_file_lease.h"
#include "../../base/xfileio.h"
#include <string.h>

struct XtcXirNativeOperation {
    XrCompileResources *resources;
    XtcXirNativeOperationLimits limits;
    XtcXirNativeOperationPhase phase;
    XrXirInvocationStage stage;
    XtcXirNativeOperationDiagnostic first, cleanup;
    XtcXirWorkspace *workspace;
    XrXirInvocation *invocation;
    XrToolchainProcess *processes[3];
    XtcXirFileLease *launcher_lease;
    void *launcher;
    size_t launcher_length;
    char *strings[32];
    uint32_t string_count, path_limit;
};
static XtcXirNativeOperationDiagnostic native_diagnostic(XtcXirNativeOperationStatus status,
    XtcXirNativeOperationDomain domain, int code, uint32_t error) {
    XtcXirNativeOperationDiagnostic result = {status, domain, code, error,
        {XR_XIR_INVOCATION_NO_STAGE, XR_XIR_INVOCATION_NO_PASS, XR_XIR_INVOCATION_SELF, 0, 0}};
    return result;
}
static XtcXirNativeOperationStatus native_fail(XtcXirNativeOperation *owner,
    XtcXirNativeOperationStatus status, XtcXirNativeOperationDomain domain, int code) {
    if (owner->first.status == XTC_XIR_NATIVE_OK) {
        owner->first = native_diagnostic(status, domain, code, 0);
        owner->first.invocation.stage = owner->stage;
    }
    owner->phase = XTC_XIR_NATIVE_FAILED;
    return status;
}
static XtcXirNativeOperationStatus native_resource_status(XrCompileResourceStatus status) {
    switch (status) {
        case XR_COMPILE_RESOURCE_OK: return XTC_XIR_NATIVE_OK;
        case XR_COMPILE_RESOURCE_BUDGET: return XTC_XIR_NATIVE_BUDGET;
        case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: return XTC_XIR_NATIVE_OUT_OF_MEMORY;
        default: return XTC_XIR_NATIVE_INVALID;
    }
}
static bool native_resource(XtcXirNativeOperation *owner, XrCompileResourceStatus status) {
    if (status == XR_COMPILE_RESOURCE_OK) return true;
    native_fail(owner, native_resource_status(status), XTC_XIR_NATIVE_RESOURCE, status); return false;
}
static bool native_work(XtcXirNativeOperation *owner, uint64_t units) {
    return native_resource(owner, xr_compile_resources_work(owner->resources, units));
}
static XtcXirNativeOperationStatus native_workspace_status(XtcXirWorkspaceStatus status) {
    switch (status) {
        case XTC_XIR_WORKSPACE_OK: return XTC_XIR_NATIVE_OK;
        case XTC_XIR_WORKSPACE_UNRESOLVED: return XTC_XIR_NATIVE_UNRESOLVED;
        case XTC_XIR_WORKSPACE_UNSUPPORTED: case XTC_XIR_WORKSPACE_REPARSE: return XTC_XIR_NATIVE_UNSUPPORTED;
        case XTC_XIR_WORKSPACE_BUDGET: return XTC_XIR_NATIVE_BUDGET;
        case XTC_XIR_WORKSPACE_OUT_OF_MEMORY: return XTC_XIR_NATIVE_OUT_OF_MEMORY;
        case XTC_XIR_WORKSPACE_IO: return XTC_XIR_NATIVE_IO;
        case XTC_XIR_WORKSPACE_IDENTITY_MISMATCH: return XTC_XIR_NATIVE_REPLAY_MISMATCH;
        case XTC_XIR_WORKSPACE_PENDING: return XTC_XIR_NATIVE_PENDING;
        default: return XTC_XIR_NATIVE_INVALID;
    }
}
static XtcXirNativeOperationStatus native_invocation_status(XrXirInvocationStatus status) {
    switch (status) {
        case XR_XIR_INVOCATION_OK: return XTC_XIR_NATIVE_OK;
        case XR_XIR_INVOCATION_UNRESOLVED: return XTC_XIR_NATIVE_UNRESOLVED;
        case XR_XIR_INVOCATION_UNSUPPORTED: return XTC_XIR_NATIVE_UNSUPPORTED;
        case XR_XIR_INVOCATION_BUDGET: return XTC_XIR_NATIVE_BUDGET;
        case XR_XIR_INVOCATION_OUT_OF_MEMORY: return XTC_XIR_NATIVE_OUT_OF_MEMORY;
        case XR_XIR_INVOCATION_IO: return XTC_XIR_NATIVE_IO;
        case XR_XIR_INVOCATION_TIMEOUT: return XTC_XIR_NATIVE_TIMEOUT;
        case XR_XIR_INVOCATION_CANCELLED: return XTC_XIR_NATIVE_CANCELLED;
        case XR_XIR_INVOCATION_CHILD_FAILED: return XTC_XIR_NATIVE_CHILD_FAILED;
        case XR_XIR_INVOCATION_REPLAY_MISMATCH: return XTC_XIR_NATIVE_REPLAY_MISMATCH;
        default: return XTC_XIR_NATIVE_INVALID;
    }
}
static bool native_invocation(XtcXirNativeOperation *owner, XrXirInvocationStatus status,
    const XrXirInvocationDiagnostic *diagnostic) {
    if (status == XR_XIR_INVOCATION_OK) return true;
    if (owner->first.status == XTC_XIR_NATIVE_OK) {
        native_fail(owner, native_invocation_status(status), XTC_XIR_NATIVE_INVOCATION, status);
        owner->first.invocation = *diagnostic;
    }
    return false;
}
static bool native_sdk(XtcXirNativeOperation *owner, XrXirRuntimeSdkStatus status) {
    if (status == XR_XIR_SDK_OK) return true;
    XtcXirNativeOperationStatus mapped = XTC_XIR_NATIVE_INVALID;
    switch (status) {
        case XR_XIR_SDK_UNRESOLVED: mapped = XTC_XIR_NATIVE_UNRESOLVED; break;
        case XR_XIR_SDK_BUDGET: mapped = XTC_XIR_NATIVE_BUDGET; break;
        case XR_XIR_SDK_OUT_OF_MEMORY: mapped = XTC_XIR_NATIVE_OUT_OF_MEMORY; break;
        case XR_XIR_SDK_IO: mapped = XTC_XIR_NATIVE_IO; break;
        case XR_XIR_SDK_UNSUPPORTED: mapped = XTC_XIR_NATIVE_UNSUPPORTED; break;
        default: break;
    }
    native_fail(owner, mapped, XTC_XIR_NATIVE_SDK, status); return false;
}
static bool native_lease(XtcXirNativeOperation *owner, XrXirTargetStatus status) {
    if (status == XR_XIR_TARGET_OK) return true;
    XtcXirNativeOperationStatus mapped = XTC_XIR_NATIVE_INVALID;
    switch (status) {
        case XR_XIR_TARGET_UNRESOLVED: mapped = XTC_XIR_NATIVE_UNRESOLVED; break;
        case XR_XIR_TARGET_BUDGET: mapped = XTC_XIR_NATIVE_BUDGET; break;
        case XR_XIR_TARGET_OUT_OF_MEMORY: mapped = XTC_XIR_NATIVE_OUT_OF_MEMORY; break;
        case XR_XIR_TARGET_IO: mapped = XTC_XIR_NATIVE_IO; break;
        case XR_XIR_TARGET_UNSUPPORTED: mapped = XTC_XIR_NATIVE_UNSUPPORTED; break;
        default: break;
    }
    native_fail(owner, mapped, XTC_XIR_NATIVE_FILE_LEASE, status); return false;
}
static bool native_process(XtcXirNativeOperation *owner, XrProcessStatus status) {
    if (status == XTC_PROCESS_OK) return true;
    XtcXirNativeOperationStatus mapped = XTC_XIR_NATIVE_INVALID;
    switch (status) {
        case XTC_PROCESS_UNRESOLVED: mapped = XTC_XIR_NATIVE_UNRESOLVED; break;
        case XTC_PROCESS_BUDGET: mapped = XTC_XIR_NATIVE_BUDGET; break;
        case XTC_PROCESS_OUT_OF_MEMORY: mapped = XTC_XIR_NATIVE_OUT_OF_MEMORY; break;
        case XTC_PROCESS_IO: mapped = XTC_XIR_NATIVE_IO; break;
        case XTC_PROCESS_TIMEOUT: mapped = XTC_XIR_NATIVE_TIMEOUT; break;
        case XTC_PROCESS_CANCELLED: mapped = XTC_XIR_NATIVE_CANCELLED; break;
        case XTC_PROCESS_UNSUPPORTED: mapped = XTC_XIR_NATIVE_UNSUPPORTED; break;
        default: break;
    }
    native_fail(owner, mapped, XTC_XIR_NATIVE_PROCESS, status); return false;
}
static bool native_path(XtcXirNativeOperation *owner, const char *directory, const char *leaf, char **out) {
    if (owner->string_count == sizeof(owner->strings) / sizeof(owner->strings[0])) {
        native_fail(owner, XTC_XIR_NATIVE_BUDGET, XTC_XIR_NATIVE_SELF, 0); return false;
    }
    XrOsIoPolicy policy = xr_compile_io_policy(owner->resources);
    XrOsIoStatus status = xr_path_join_owned(&policy, directory, leaf, out);
    if (status == XR_OS_IO_OK) { owner->strings[owner->string_count++] = *out; return true; }
    XtcXirNativeOperationStatus mapped = status == XR_OS_IO_BUDGET ? XTC_XIR_NATIVE_BUDGET :
        status == XR_OS_IO_OUT_OF_MEMORY ? XTC_XIR_NATIVE_OUT_OF_MEMORY :
        status == XR_OS_IO_IO ? XTC_XIR_NATIVE_IO : XTC_XIR_NATIVE_INVALID;
    native_fail(owner, mapped, XTC_XIR_NATIVE_PATH, status); return false;
}
static bool native_length(XtcXirNativeOperation *owner, const char *text, size_t *length) {
    for (size_t i = 0;; ++i) {
        if (!native_work(owner, 1)) return false;
        if (!text[i]) { *length = i; return true; }
        if (i == owner->path_limit) {
            native_fail(owner, XTC_XIR_NATIVE_BUDGET, XTC_XIR_NATIVE_SELF, 0); return false;
        }
    }
}
static bool native_join(XtcXirNativeOperation *owner, const char *a, const char *b, const char *c, char **out) {
    if (owner->string_count == sizeof(owner->strings) / sizeof(owner->strings[0])) {
        native_fail(owner, XTC_XIR_NATIVE_BUDGET, XTC_XIR_NATIVE_SELF, 0); return false;
    }
    size_t lengths[3]; const char *parts[3] = {a, b, c}; size_t total = 1;
    for (unsigned i = 0; i < 3; ++i) {
        if (!native_length(owner, parts[i], &lengths[i])) return false;
        if (lengths[i] > SIZE_MAX - total) {
            native_fail(owner, XTC_XIR_NATIVE_BUDGET, XTC_XIR_NATIVE_SELF, 0); return false;
        }
        total += lengths[i];
    }
    char *value = NULL;
    if (!native_resource(owner, xr_compile_resources_alloc(owner->resources, total, (void **)&value))) return false;
    owner->strings[owner->string_count++] = value;
    size_t offset = 0;
    for (unsigned i = 0; i < 3; ++i) {
        if (!native_work(owner, lengths[i])) return false;
        memcpy(value + offset, parts[i], lengths[i]); offset += lengths[i];
    }
    if (!native_work(owner, 1)) return false;
    value[offset] = 0; *out = value; return true;
}
static bool native_spec(XtcXirNativeOperation *owner, const XtcXirNativeOperationRequest *request,
    const char *executable, XrProcessSpec *spec) {
    if (!native_work(owner, sizeof(*spec))) return false;
    xtc_process_spec_init(spec, executable, owner->limits.timeout_ms);
    const XtcXirWorkspacePaths *paths = xtc_xir_workspace_paths(owner->workspace);
    spec->environment_source = XTC_PROCESS_ENV_EXPLICIT;
    spec->cwd = paths->input; spec->output_limit = owner->limits.process_output_bytes;
    spec->env_count = 3;
    spec->env_keys[0] = "SystemRoot"; spec->env_values[0] = request->msvc.system_root;
    spec->env_keys[1] = "TEMP"; spec->env_values[1] = paths->output;
    spec->env_keys[2] = "TMP"; spec->env_values[2] = paths->output;
    return true;
}
static bool native_commands(XtcXirNativeOperation *owner, const XtcXirNativeOperationRequest *request,
    XrXirInvocationRequest *invocation) {
    const XtcXirWorkspacePaths *paths = xtc_xir_workspace_paths(owner->workspace);
    static const char *const leaves[8] = {"generated.c", "launcher.c", "generated.obj", "launcher.obj",
        "generated.json", "launcher.json", "actual.rsp", "program.exe"};
    char *files[8] = {0};
    for (unsigned i = 0; i < 8; ++i) {
        owner->stage = i >= 6 ? XR_XIR_INVOCATION_LINK :
            i % 2 ? XR_XIR_INVOCATION_LAUNCHER : XR_XIR_INVOCATION_GENERATED;
        if (!native_path(owner, i < 2 ? paths->input : paths->output, leaves[i], &files[i])) return false;
    }
    owner->stage = XR_XIR_INVOCATION_NO_STAGE;
    const char *sdk_root = xr_xir_runtime_sdk_root(request->sdk);
    static const char *const children[3] = {"src", "include", "generated"};
    char *includes[7] = {0};
    for (unsigned i = 0; i < 3; ++i) {
        char *path = NULL;
        if (!native_path(owner, sdk_root, children[i], &path) || !native_join(owner, "/I", path, "", &includes[i])) return false;
    }
    const char *external[4] = {request->msvc.vc_include, request->msvc.ucrt_include,
        request->msvc.shared_include, request->msvc.um_include};
    for (unsigned i = 0; i < 4; ++i)
        if (!native_join(owner, "/I", external[i], "", &includes[3 + i])) return false;
    static const char *const flags[13] = {"/nologo", "/std:c11", "/utf-8", "/experimental:c11atomics",
        "/MD", "/O2", "/W4", "/WX", "/DNDEBUG", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN",
        "/D_CRT_SECURE_NO_WARNINGS", "/X"};
    for (unsigned stage = 0; stage < 2; ++stage) {
        owner->stage = stage ? XR_XIR_INVOCATION_LAUNCHER : XR_XIR_INVOCATION_GENERATED;
        XrProcessSpec spec;
        if (!native_spec(owner, request, request->compiler, &spec)) return false;
        for (unsigned i = 0; i < 13; ++i) spec.argv[1 + i] = flags[i];
        for (unsigned i = 0; i < 7; ++i) spec.argv[14 + i] = includes[i];
        char *object_argument = NULL;
        if (!native_join(owner, "/Fo", files[2 + stage], "", &object_argument)) return false;
        spec.argv[21] = "/c"; spec.argv[22] = files[stage]; spec.argv[23] = object_argument;
        spec.argv[24] = "/sourceDependencies"; spec.argv[25] = files[4 + stage];
        if (stage) {
            char *symbol = NULL;
            const XrXirNativeProjectionFacts *facts = xr_compile_native_projection_facts(request->projection);
            if (!native_join(owner, "/DXIR_SDK_PROGRAM_SYMBOL=", facts->prefix, "_program", &symbol)) return false;
            spec.argv[26] = symbol;
        }
        if (!native_process(owner, xtc_process_prepare(owner->resources, &spec, &owner->processes[stage]))) return false;
        invocation->compile[stage] = (XrXirInvocationCompile){owner->processes[stage], files[stage], files[2 + stage], files[4 + stage]};
    }
    XrProcessSpec spec;
    owner->stage = XR_XIR_INVOCATION_LINK;
    if (!native_spec(owner, request, request->linker, &spec)) return false;
    char *output_argument = NULL, *report_argument = NULL;
    if (!native_join(owner, "/out:", files[7], "", &output_argument) ||
        !native_join(owner, "/LINKREPROFULLPATHRSP:", files[6], "", &report_argument)) return false;
    spec.argv[1] = "/nologo"; spec.argv[2] = "/incremental:no"; spec.argv[3] = "/NODEFAULTLIB";
    spec.argv[4] = "/verbose:lib"; spec.argv[5] = output_argument; spec.argv[6] = report_argument;
    spec.argv[7] = files[2]; spec.argv[8] = files[3];
    static const char *const archives[5] = {"lib/xray_compile_resources.lib", "lib/xray_xir_admission.lib",
        "lib/xray_xir_declarations.lib", "lib/xray_xir_scalar.lib", "lib/xray_xir_runtime_host.lib"};
    for (unsigned i = 0; i < 5; ++i)
        if (!native_sdk(owner, xr_xir_runtime_sdk_file(request->sdk, archives[i], &spec.argv[9 + i]))) return false;
    for (uint32_t i = 0; i < request->library_count; ++i) {
        if (!native_work(owner, sizeof(request->libraries[i]))) return false;
        if (!request->libraries[i].path || (request->libraries[i].kind != XR_XIR_INVOCATION_CRT &&
            request->libraries[i].kind != XR_XIR_INVOCATION_SYSTEM)) {
            native_fail(owner, XTC_XIR_NATIVE_INVALID, XTC_XIR_NATIVE_SELF, 0); return false;
        }
        spec.argv[14 + i] = request->libraries[i].path;
    }
    if (!native_process(owner, xtc_process_prepare(owner->resources, &spec, &owner->processes[2]))) return false;
    invocation->link = owner->processes[2]; invocation->input_directory = paths->input;
    invocation->output_directory = paths->output; invocation->link_report = files[6]; invocation->output = files[7];
    return true;
}
static void native_release_inputs(XtcXirNativeOperation *owner) {
    xtc_xir_invocation_free(owner->invocation); owner->invocation = NULL;
    for (unsigned i = 0; i < 3; ++i) { xtc_process_free(owner->processes[i]); owner->processes[i] = NULL; }
    xtc_xir_file_lease_free(owner->launcher_lease); owner->launcher_lease = NULL;
    xr_compile_resources_free(owner->launcher); owner->launcher = NULL; owner->launcher_length = 0;
    for (uint32_t i = 0; i < owner->string_count; ++i) xr_compile_resources_free(owner->strings[i]);
    owner->string_count = 0;
}
XR_FUNC XtcXirNativeOperationStatus xtc_xir_native_operation_new(XrCompileResources *resources,
    const XtcXirWorkspaceRequest *workspace, const XtcXirNativeOperationLimits *limits, XtcXirNativeOperation **output) {
    if (!resources || !workspace || !limits || !output || *output ||
        !limits->invocation.dependencies.frame_bytes || !limits->invocation.dependencies.path_bytes ||
        !limits->invocation.dependencies.records || !limits->invocation.artifact_bytes ||
        !limits->invocation.files || !limits->timeout_ms || !limits->process_output_bytes)
        return XTC_XIR_NATIVE_INVALID;
    XtcXirNativeOperation *owner = NULL;
    XrCompileResourceStatus allocated = xr_compile_resources_calloc(resources, 1, sizeof(*owner), (void **)&owner);
    if (allocated != XR_COMPILE_RESOURCE_OK) return native_resource_status(allocated);
    owner->resources = resources; owner->path_limit = workspace->limits.path_bytes;
    owner->stage = XR_XIR_INVOCATION_NO_STAGE;
    owner->first = native_diagnostic(XTC_XIR_NATIVE_OK, XTC_XIR_NATIVE_SELF, 0, 0);
    owner->cleanup = owner->first;
    if (!native_work(owner, sizeof(*limits))) { XtcXirNativeOperationStatus status = owner->first.status; xr_compile_resources_free(owner); return status; }
    owner->limits = *limits;
    XtcXirWorkspaceStatus created = xtc_xir_workspace_new(resources, workspace, &owner->workspace);
    if (created != XTC_XIR_WORKSPACE_OK) { xr_compile_resources_free(owner); return native_workspace_status(created); }
    *output = owner; return XTC_XIR_NATIVE_OK;
}
XR_FUNC XtcXirNativeOperationStatus xtc_xir_native_operation_run(XtcXirNativeOperation *owner,
    const XtcXirNativeOperationRequest *request) {
    if (!owner || owner->phase != XTC_XIR_NATIVE_NEW) return XTC_XIR_NATIVE_INVALID;
    const XrXirCompileContext *context = request ? xr_compile_native_projection_context(request->projection) : NULL;
    if (!request || !context || context->resources != owner->resources ||
        xr_xir_runtime_sdk_resources(request->sdk) != owner->resources || !request->compiler || !request->linker ||
        !request->msvc.vc_include || !request->msvc.ucrt_include || !request->msvc.shared_include ||
        !request->msvc.um_include || !request->msvc.system_root ||
        request->library_count > XTC_PROCESS_MAX_ARGS - 15 || (request->library_count && !request->libraries))
        return native_fail(owner, XTC_XIR_NATIVE_INVALID, XTC_XIR_NATIVE_SELF, 0);
    owner->phase = XTC_XIR_NATIVE_RUNNING;
    if (request->cancelled && request->cancelled(request->cancel_context))
        return native_fail(owner, XTC_XIR_NATIVE_CANCELLED, XTC_XIR_NATIVE_SELF, 0);
    XtcXirWorkspaceStatus created = xtc_xir_workspace_create(owner->workspace);
    if (created != XTC_XIR_WORKSPACE_OK) {
        native_fail(owner, native_workspace_status(created), XTC_XIR_NATIVE_WORKSPACE, created);
        owner->first.os_error = xtc_xir_workspace_diagnostic(owner->workspace)->os_error;
        return owner->first.status;
    }
    const char *launcher_path = NULL;
    owner->stage = XR_XIR_INVOCATION_LAUNCHER;
    if (!native_sdk(owner, xr_xir_runtime_sdk_file(request->sdk, "src/execution/xr_xir_native_main.inc.c", &launcher_path)) ||
        !native_lease(owner, xtc_xir_file_lease_open(owner->resources, launcher_path, &owner->launcher_lease))) return owner->first.status;
    size_t launcher_limit = (size_t)owner->limits.invocation.artifact_bytes;
#if SIZE_MAX < UINT64_MAX
    if (owner->limits.invocation.artifact_bytes > SIZE_MAX) launcher_limit = SIZE_MAX;
#endif
    if (!native_lease(owner, xtc_xir_file_lease_read(owner->launcher_lease, launcher_limit,
        &owner->launcher, &owner->launcher_length))) return owner->first.status;
    XrXirInvocationRequest invoke = {0};
    if (!native_commands(owner, request, &invoke)) return owner->first.status;
    invoke.projection = request->projection; invoke.sdk = request->sdk; invoke.msvc = request->msvc;
    invoke.launcher = owner->launcher; invoke.launcher_length = owner->launcher_length;
    invoke.libraries = request->libraries; invoke.library_count = request->library_count;
    invoke.limits = owner->limits.invocation; invoke.cancelled = request->cancelled; invoke.cancel_context = request->cancel_context;
    XrXirInvocationDiagnostic diagnostic;
    XrXirInvocationStatus status = xtc_xir_invocation_run(&invoke, &owner->invocation, &diagnostic);
    if (!native_invocation(owner, status, &diagnostic)) return owner->first.status;
    owner->stage = XR_XIR_INVOCATION_NO_STAGE;
    owner->phase = XTC_XIR_NATIVE_READY;
    return XTC_XIR_NATIVE_OK;
}
XR_FUNC XtcXirNativeOperationStatus xtc_xir_native_operation_close(XtcXirNativeOperation **slot,
    uint32_t workspace_steps) {
    if (!slot || !workspace_steps) return XTC_XIR_NATIVE_INVALID;
    XtcXirNativeOperation *owner = *slot;
    if (!owner) return XTC_XIR_NATIVE_OK;
    if (owner->phase == XTC_XIR_NATIVE_RUNNING) return XTC_XIR_NATIVE_INVALID;
    owner->phase = XTC_XIR_NATIVE_CLOSING;
    native_release_inputs(owner);
    XtcXirWorkspaceStatus status = xtc_xir_workspace_close(&owner->workspace, workspace_steps);
    uint32_t error = owner->workspace ? xtc_xir_workspace_diagnostic(owner->workspace)->os_error : 0;
    owner->cleanup = native_diagnostic(native_workspace_status(status), XTC_XIR_NATIVE_WORKSPACE, status, error);
    if (status != XTC_XIR_WORKSPACE_OK) return owner->cleanup.status;
    xr_compile_resources_free(owner); *slot = NULL; return XTC_XIR_NATIVE_OK;
}
XR_FUNC const XrXirInvocationFacts *xtc_xir_native_operation_facts(const XtcXirNativeOperation *owner) {
    return owner && owner->phase == XTC_XIR_NATIVE_READY ? xtc_xir_invocation_facts(owner->invocation) : NULL;
}
XR_FUNC const XrXirInvocationProviderFacts *xtc_xir_native_operation_provider(const XtcXirNativeOperation *owner) {
    return owner && owner->phase == XTC_XIR_NATIVE_READY ? xtc_xir_invocation_provider(owner->invocation) : NULL;
}
XR_FUNC const XrProcessView *xtc_xir_native_operation_command(const XtcXirNativeOperation *owner,
    XrXirInvocationStage stage) {
    return owner && owner->phase == XTC_XIR_NATIVE_READY ? xtc_xir_invocation_command(owner->invocation, stage) : NULL;
}
XR_FUNC const XrXirInvocationFile *xtc_xir_native_operation_file(const XtcXirNativeOperation *owner, uint32_t index) {
    return owner && owner->phase == XTC_XIR_NATIVE_READY ? xtc_xir_invocation_file(owner->invocation, index) : NULL;
}
XR_FUNC XtcXirNativeOperationStatus xtc_xir_native_operation_read_output(XtcXirNativeOperation *owner,
    uint64_t limit, void **bytes, size_t *length) {
    if (!owner || owner->phase != XTC_XIR_NATIVE_READY || !limit || !bytes || *bytes || !length || *length)
        return XTC_XIR_NATIVE_INVALID;
    XrXirInvocationStatus status = xtc_xir_invocation_read_output(owner->invocation, limit, bytes, length);
    if (status == XR_XIR_INVOCATION_OK) return XTC_XIR_NATIVE_OK;
    return native_fail(owner, native_invocation_status(status), XTC_XIR_NATIVE_INVOCATION, status);
}
XR_FUNC XrCompileResources *xtc_xir_native_operation_resources(const XtcXirNativeOperation *owner) { return owner ? owner->resources : NULL; }
XR_FUNC XtcXirNativeOperationPhase xtc_xir_native_operation_phase(const XtcXirNativeOperation *owner) { return owner ? owner->phase : XTC_XIR_NATIVE_FAILED; }
XR_FUNC const XtcXirNativeOperationDiagnostic *xtc_xir_native_operation_diagnostic(const XtcXirNativeOperation *owner) { return owner ? &owner->first : NULL; }
XR_FUNC const XtcXirNativeOperationDiagnostic *xtc_xir_native_operation_cleanup_diagnostic(const XtcXirNativeOperation *owner) { return owner ? &owner->cleanup : NULL; }
