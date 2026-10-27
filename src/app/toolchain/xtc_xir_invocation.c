/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_invocation.c - Owned trusted-host native commands and explicit inputs
 *
 * KEY CONCEPT:
 *   Frozen commands run once in an owned process group. Explicit inputs and
 *   output retain positive leases. The installed toolchain and OS loader are
 *   trusted local platform inputs, not a closed namespace or native sandbox.
 */
#include "xtc_xir_invocation.h"
#include "xtc_xir_file_lease.h"
#include "xtc_xir_sysroot_internal.h"
#include "../../base/xio_policy.h"
#include "../../base/xfileio.h"
#include "../../base/xsha256.h"
#include "../../os/os_fs.h"
#include "../../os/os_dir.h"
#include <string.h>
#ifdef XR_OS_WINDOWS
#include "../../base/xwindows_utf8.h"
#endif

typedef struct InvocationFile {
    XrXirInvocationFile facts;
    XtcXirFileLease *lease;
} InvocationFile;
typedef struct InvocationInputs {
    XtcXirFileLease *directories[8];
    const char *paths[8];
    char *owned_paths[8];
    bool missing[8];
#ifdef XR_OS_WINDOWS
    wchar_t *wide[8];
    size_t lengths[8];
#endif
} InvocationInputs;
struct XrXirInvocation {
    XrXirCompileContext context;
    XrOsIoPolicy io;
    XrXirInvocationStatus status;
    XrXirInvocationDiagnostic diagnostic;
    XrXirInvocationLimits limits;
    XrXirInvocationFacts facts;
    XtcXirHashProofCache *proof_cache;
    XrXirInvocationProviderFacts provider;
    XrToolchainProcess *process[3];
    XrProcessView commands[3];
    XrDependencyFacts *reports[3];
    InvocationFile *files;
    uint32_t capacity;
    char *prefix;
    XtcXirFileLease *directories[2];
    XtcXirFileLease *output_lease; /* Borrowed from the owned file table. */
    InvocationInputs inputs;
};
static bool invocation_fail(XrXirInvocation *owner, XrXirInvocationStatus status,
    XrXirInvocationFailureDomain domain, int code) {
    if (owner->status == XR_XIR_INVOCATION_OK) {
        owner->status = status;
        owner->diagnostic.domain = domain;
        owner->diagnostic.code = code;
    }
    return false;
}
static bool invocation_resource(XrXirInvocation *owner, XrCompileResourceStatus status) {
    if (status == XR_COMPILE_RESOURCE_OK) return true;
    return invocation_fail(owner, status == XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_INVOCATION_MEMORY_LIMIT :
        status == XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_INVOCATION_OVERFLOW :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_INVOCATION_OUT_OF_MEMORY :
        XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_RESOURCE, status);
}
static bool invocation_io(XrXirInvocation *owner, XrOsIoStatus status) {
    if (status == XR_OS_IO_OK) return true;
    XrXirInvocationStatus mapped;
    switch (status) {
    case XR_OS_IO_NOT_FOUND: mapped = XR_XIR_INVOCATION_UNRESOLVED; break;
    case XR_OS_IO_UNSUPPORTED: mapped = XR_XIR_INVOCATION_UNSUPPORTED; break;
    case XR_OS_IO_MEMORY_LIMIT: mapped = XR_XIR_INVOCATION_MEMORY_LIMIT; break;
    case XR_OS_IO_OVERFLOW: mapped = XR_XIR_INVOCATION_OVERFLOW; break;
    case XR_OS_IO_OUT_OF_MEMORY: mapped = XR_XIR_INVOCATION_OUT_OF_MEMORY; break;
    case XR_OS_IO_IO: mapped = XR_XIR_INVOCATION_IO; break;
    default: mapped = XR_XIR_INVOCATION_INVALID; break;
    }
    return invocation_fail(owner, mapped, XR_XIR_INVOCATION_FILESYSTEM, status);
}
static XrXirInvocationStatus invocation_target_status(XrXirTargetStatus status) {
    if (status == XR_XIR_TARGET_OK) return XR_XIR_INVOCATION_OK;
    XrXirInvocationStatus mapped;
    switch (status) {
    case XR_XIR_TARGET_UNRESOLVED: mapped = XR_XIR_INVOCATION_UNRESOLVED; break;
    case XR_XIR_TARGET_UNSUPPORTED: mapped = XR_XIR_INVOCATION_UNSUPPORTED; break;
    case XR_XIR_TARGET_BUDGET: mapped = XR_XIR_INVOCATION_BUDGET; break;
    case XR_XIR_TARGET_OUT_OF_MEMORY: mapped = XR_XIR_INVOCATION_OUT_OF_MEMORY; break;
    case XR_XIR_TARGET_MEMORY_LIMIT: mapped = XR_XIR_INVOCATION_MEMORY_LIMIT; break;
    case XR_XIR_TARGET_OVERFLOW: mapped = XR_XIR_INVOCATION_OVERFLOW; break;
    case XR_XIR_TARGET_IO: mapped = XR_XIR_INVOCATION_IO; break;
    default: mapped = XR_XIR_INVOCATION_INVALID; break;
    }
    return mapped;
}
static bool invocation_target(XrXirInvocation *owner, XrXirTargetStatus status) {
    return status == XR_XIR_TARGET_OK ||
        invocation_fail(owner, invocation_target_status(status), XR_XIR_INVOCATION_TARGET, status);
}
static bool invocation_process(XrXirInvocation *owner, XrProcessStatus status) {
    if (status == XTC_PROCESS_OK) return true;
    XrXirInvocationStatus mapped;
    switch (status) {
    case XTC_PROCESS_UNRESOLVED: mapped = XR_XIR_INVOCATION_UNRESOLVED; break;
    case XTC_PROCESS_UNSUPPORTED: mapped = XR_XIR_INVOCATION_UNSUPPORTED; break;
    case XTC_PROCESS_BUDGET: mapped = XR_XIR_INVOCATION_BUDGET; break;
    case XTC_PROCESS_OUT_OF_MEMORY: mapped = XR_XIR_INVOCATION_OUT_OF_MEMORY; break;
        case XTC_PROCESS_MEMORY_LIMIT: mapped = XR_XIR_INVOCATION_MEMORY_LIMIT; break;
        case XTC_PROCESS_OVERFLOW: mapped = XR_XIR_INVOCATION_OVERFLOW; break;
    case XTC_PROCESS_TIMEOUT: mapped = XR_XIR_INVOCATION_TIMEOUT; break;
    case XTC_PROCESS_CANCELLED: mapped = XR_XIR_INVOCATION_CANCELLED; break;
    case XTC_PROCESS_IO: mapped = XR_XIR_INVOCATION_IO; break;
    default: mapped = XR_XIR_INVOCATION_INVALID; break;
    }
    return invocation_fail(owner, mapped, XR_XIR_INVOCATION_PROCESS, status);
}
static bool invocation_sdk(XrXirInvocation *owner, XrXirRuntimeSdkStatus status) {
    if (status == XR_XIR_SDK_OK) return true;
    XrXirInvocationStatus mapped;
    switch (status) {
    case XR_XIR_SDK_UNRESOLVED: mapped = XR_XIR_INVOCATION_UNRESOLVED; break;
    case XR_XIR_SDK_UNSUPPORTED: mapped = XR_XIR_INVOCATION_UNSUPPORTED; break;
    case XR_XIR_SDK_BUDGET: mapped = XR_XIR_INVOCATION_BUDGET; break;
    case XR_XIR_SDK_OUT_OF_MEMORY: mapped = XR_XIR_INVOCATION_OUT_OF_MEMORY; break;
    case XR_XIR_SDK_MEMORY_LIMIT: mapped = XR_XIR_INVOCATION_MEMORY_LIMIT; break;
    case XR_XIR_SDK_OVERFLOW: mapped = XR_XIR_INVOCATION_OVERFLOW; break;
    case XR_XIR_SDK_IO: mapped = XR_XIR_INVOCATION_IO; break;
    default: mapped = XR_XIR_INVOCATION_INVALID; break;
    }
    return invocation_fail(owner, mapped, XR_XIR_INVOCATION_SDK, status);
}
static XrXirTargetStatus invocation_file_open(XrXirInvocation *owner,const char *path,XtcXirFileLease **output) {
#ifdef XR_OS_WINDOWS
    return xtc_xir_file_lease_open_with_proof(owner->context.resources,path,owner->proof_cache,output);
#else
    return xtc_xir_file_lease_open(owner->context.resources,path,output);
#endif
}
static bool invocation_same(XrXirInvocation *owner, const char *a, const char *b, bool *same) {
    if (!a || !b) return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
    for (;;) {

        unsigned char left = (unsigned char)*a++, right = (unsigned char)*b++;
        if (left != right || !left) { *same = left == right; return true; }
    }
}
static bool invocation_expect(XrXirInvocation *owner, const char *actual, const char *expected) {
    bool same = false;
    return invocation_same(owner, actual, expected, &same) &&
        (same || invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0));
}
static bool invocation_bytes(XrXirInvocation *owner, const void *a, const void *b, size_t length) {
    const uint8_t *left = a, *right = b;
    for (size_t i = 0; i < length; ++i) {

        if (left[i] != right[i]) return invocation_fail(owner, XR_XIR_INVOCATION_INPUT_MISMATCH,
            XR_XIR_INVOCATION_SELF, 0);
    }
    return true;
}
static bool invocation_length(XrXirInvocation *owner, const char *text, size_t *length) {
    if (!text) return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
    for (size_t i = 0;; ++i) {

        unsigned char byte = (unsigned char)text[i];
        if (!byte) { *length = i; return true; }
        if (i >= owner->limits.dependencies.path_bytes)
            return invocation_fail(owner, XR_XIR_INVOCATION_BUDGET, XR_XIR_INVOCATION_SELF, 0);
    }
}
static bool invocation_copy(XrXirInvocation *owner, void *destination, const void *source, size_t bytes) {
    if (owner->status != XR_XIR_INVOCATION_OK) return false;
    memcpy(destination, source, bytes); return true;
}
static bool invocation_file_reserve(XrXirInvocation *owner) {
    if (owner->facts.file_count == owner->limits.files)
        return invocation_fail(owner, XR_XIR_INVOCATION_BUDGET, XR_XIR_INVOCATION_SELF, 0);
    if (owner->facts.file_count == owner->capacity) {
        uint32_t capacity = owner->capacity > UINT32_MAX / 2 ? owner->limits.files :
            owner->capacity ? owner->capacity * 2 : 16;
        if (capacity > owner->limits.files) capacity = owner->limits.files;
        if ((uint64_t)capacity * sizeof(*owner->files) > SIZE_MAX)
            return invocation_fail(owner, XR_XIR_INVOCATION_OVERFLOW, XR_XIR_INVOCATION_SELF, 0);
        if (!invocation_resource(owner, xr_compile_resources_resize(owner->context.resources,
            (void **)&owner->files, (size_t)capacity * sizeof(*owner->files)))) return false;
        owner->capacity = capacity;
    }
    return true;
}
static bool invocation_file_publish(XrXirInvocation *owner, XrXirInvocationStage stage,
    XrXirInvocationFileKind kind, const char *path, uint64_t length, const uint8_t *digest,
    XtcXirFileLease *lease) {
    if (!(invocation_file_reserve(owner))) return false;
    InvocationFile *file = &owner->files[owner->facts.file_count];
    *file = (InvocationFile){{stage, kind, path, length, {0}}, lease};
    memcpy(file->facts.digest, digest, sizeof(file->facts.digest));
    if (kind == XR_XIR_INVOCATION_OUTPUT) owner->output_lease = lease;
    ++owner->facts.file_count;
    return true;
}
static bool invocation_file_add(XrXirInvocation *owner, XrXirInvocationStage stage,
    XrXirInvocationFileKind kind, XtcXirFileLease **lease) {
    const XtcXirFileFacts *facts = xtc_xir_file_lease_facts(*lease);
    if (!facts) return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
    if (!invocation_file_publish(owner, stage, kind, facts->path, facts->length, facts->digest, *lease)) return false;
    *lease = NULL;
    return true;
}
static bool invocation_platform(void) {
#ifdef XR_OS_WINDOWS
    return true;
#else
    return false;
#endif
}
static bool invocation_join_argument(XrXirInvocation *owner, const char *actual,
    const char *prefix, const char *suffix) {
    while (true) {

        unsigned char part = (unsigned char)*prefix++;
        if (!part) break;

        if ((unsigned char)*actual++ != part)
            return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
    }
    return invocation_expect(owner, actual, suffix);
}
static bool invocation_environment(XrXirInvocation *owner, const XrProcessView *view,
    const XrXirInvocationRequest *request) {
    static const char *const keys[] = {"SystemRoot", "TEMP", "TMP"};
    const char *values[] = {request->msvc.system_root, request->output_directory, request->output_directory};
    if (view->env_count != 3)
        return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
    for (unsigned i = 0; i < 3; ++i)
        if (!invocation_expect(owner, view->env_keys[i], keys[i]) ||
            !invocation_expect(owner, view->env_values[i], values[i])) return false;
    return true;
}
static bool invocation_compile_recipe(XrXirInvocation *owner,
    const XrXirInvocationRequest *request, unsigned stage) {
    owner->diagnostic.stage = (XrXirInvocationStage)stage;
    owner->diagnostic.pass = XR_XIR_INVOCATION_NO_PASS;
    static const char *const fixed[] = {"/nologo", "/std:c11", "/utf-8", "/experimental:c11atomics",
        "/MD", "/O2", "/W4", "/WX", "/DNDEBUG", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN",
        "/D_CRT_SECURE_NO_WARNINGS"};
    const XrProcessView *view = &owner->commands[stage];
    const XrXirInvocationCompile *compile = &request->compile[stage];
    if (view->completion_policy != XR_PROC_COMPLETE_ROOT || view->image_mode != XR_PROC_IMAGES_NONE ||
        view->argc != 26 + stage)
        return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
    for (unsigned i = 0; i < 12; ++i)
        if (!invocation_expect(owner, view->argv[i + 1], fixed[i])) return false;
    static const char *const directories[] = {"src", "include", "generated"};
    for (unsigned i = 0; i < 3; ++i) {
        char *path = NULL;
        if (!invocation_io(owner, xr_path_join_owned(&owner->io, xr_xir_runtime_sdk_root(request->sdk),
            directories[i], &path))) return false;
        bool okay = invocation_join_argument(owner, view->argv[14 + i], "/I", path);
        xr_compile_resources_free(path);
        if (!okay) return false;
    }
    if (!invocation_expect(owner, view->argv[13], "/X")) return false;
    const char *external[] = {request->msvc.vc_include, request->msvc.ucrt_include,
        request->msvc.shared_include, request->msvc.um_include};
    for (unsigned i = 0; i < 4; ++i)
        if (!invocation_join_argument(owner, view->argv[17 + i], "/I", external[i])) return false;
    if (!invocation_expect(owner, view->argv[21], "/c") ||
        !invocation_expect(owner, view->argv[22], compile->source) ||
        !invocation_join_argument(owner, view->argv[23], "/Fo", compile->object) ||
        !invocation_expect(owner, view->argv[24], "/sourceDependencies") ||
        !invocation_expect(owner, view->argv[25], compile->report)) return false;
    if (stage) {
        size_t length = 0;
        if (!invocation_length(owner, owner->prefix, &length)) return false;
        char symbol[73];
        if (length > 64) return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
        if (!invocation_copy(owner,  symbol, owner->prefix, length) ||
            !invocation_copy(owner,  symbol + length, "_program", sizeof("_program"))) return false;
        if (!invocation_join_argument(owner, view->argv[26], "/DXIR_SDK_PROGRAM_SYMBOL=", symbol)) return false;
    }
    return invocation_expect(owner, view->cwd, request->input_directory) && invocation_environment(owner, view, request);
}
static bool invocation_link_recipe(XrXirInvocation *owner, const XrXirInvocationRequest *request,
    const char *const sdk_libraries[5]) {
    owner->diagnostic.stage = XR_XIR_INVOCATION_LINK;
    owner->diagnostic.pass = XR_XIR_INVOCATION_NO_PASS;
    const XrProcessView *view = &owner->commands[2];
    if (view->completion_policy != XR_PROC_COMPLETE_ROOT || view->image_mode != XR_PROC_IMAGES_NONE ||
        request->library_count > XTC_PROCESS_MAX_ARGS - 14 || view->argc != 14 + request->library_count)
        return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
    if (!invocation_expect(owner, view->argv[1], "/nologo") ||
        !invocation_expect(owner, view->argv[2], "/incremental:no") ||
        !invocation_expect(owner, view->argv[3], "/NODEFAULTLIB") ||
        !invocation_expect(owner, view->argv[4], "/verbose:lib") ||
        !invocation_join_argument(owner, view->argv[5], "/out:", request->output) ||
        !invocation_join_argument(owner, view->argv[6], "/LINKREPROFULLPATHRSP:", request->link_report)) return false;
    for (unsigned i = 0; i < 2; ++i)
        if (!invocation_expect(owner, view->argv[7 + i], request->compile[i].object)) return false;
    for (unsigned i = 0; i < 5; ++i)
        if (!invocation_expect(owner, view->argv[9 + i], sdk_libraries[i])) return false;
    for (uint32_t i = 0; i < request->library_count; ++i) {

        if (request->libraries[i].kind != XR_XIR_INVOCATION_CRT && request->libraries[i].kind != XR_XIR_INVOCATION_SYSTEM)
            return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
        if (!invocation_expect(owner, view->argv[14 + i], request->libraries[i].path)) return false;
    }
    return invocation_expect(owner, view->cwd, request->input_directory) && invocation_environment(owner, view, request);
}
static bool invocation_child_path(XrXirInvocation *owner, const char *path, const char *directory) {
    char *parent = NULL;
    if (!invocation_io(owner, xr_path_dirname_owned(&owner->io, path, &parent))) return false;
    bool same = false;
    bool okay = invocation_same(owner, parent, directory, &same);
    xr_compile_resources_free(parent);
    if (!okay) return false;
    if (!same) return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
    size_t length = 0;
    if (!invocation_length(owner, path, &length)) return false;
    /* The parent has already been admitted as an absolute directory. The
     * remaining single component cannot redirect CREATE_NEW elsewhere. */
    size_t parent_length = 0;
    if (!invocation_length(owner, directory, &parent_length) || length <= parent_length + 1)
        return owner->status != XR_XIR_INVOCATION_OK ? false :
            invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
    unsigned char last = 0;
    for (size_t i = parent_length + 1; i < length; ++i) {

        unsigned char byte = (unsigned char)path[i];
        if (byte < 32 || byte == '/' || byte == '\\' || byte == ':' || byte == '*' || byte == '?' ||
            byte == '"' || byte == '<' || byte == '>' || byte == '|')
            return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
        last = byte;
    }
    return (last != '.' && last != ' ') ||
        invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
}
static bool invocation_empty_directory(XrXirInvocation *owner, const char *path) {
    XrDirIter *iterator = NULL;
    if (!invocation_io(owner, xr_os_io_dir_open(&owner->io, path, &iterator))) return false;
    XrDirEntry entry;
    XrOsIoStatus status = xr_os_io_dir_next(iterator, &entry);
    xr_os_io_dir_close(iterator);
    if (status != XR_OS_IO_END) return status == XR_OS_IO_OK ?
        invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0) : invocation_io(owner, status);
    return true;
}
static bool invocation_disjoint_directories(XrXirInvocation *owner) {
#ifdef XR_OS_WINDOWS
    const XtcXirDirectoryFacts *facts[2] = {
        xtc_xir_file_lease_directory_facts(owner->directories[0]),
        xtc_xir_file_lease_directory_facts(owner->directories[1])};
    size_t lengths[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        if (!facts[i]) return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
        for (;;) {

            if (!facts[i]->native_path[lengths[i]]) break;
            if (++lengths[i] > INT_MAX)
                return invocation_fail(owner, XR_XIR_INVOCATION_BUDGET, XR_XIR_INVOCATION_SELF, 0);
        }
    }
    unsigned shorter = lengths[0] <= lengths[1] ? 0 : 1, longer = shorter ^ 1;
    size_t common = lengths[shorter];
    if (!common) return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);

    int order = CompareStringOrdinal(facts[shorter]->native_path, (int)common,
        facts[longer]->native_path, (int)common, TRUE);
    if (!order) return invocation_io(owner, io_windows_status(GetLastError()));
    if (order != CSTR_EQUAL) return true;
    if (lengths[0] == lengths[1])
        return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);

    wchar_t last = facts[shorter]->native_path[common - 1], next = facts[longer]->native_path[common];
    return (last != L'\\' && last != L'/' && next != L'\\' && next != L'/') ||
        invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
#else
    return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
#endif
}
static void invocation_input_free(InvocationInputs *profile) {
    for (unsigned i = 0; i < 8; ++i) {
        xtc_xir_file_lease_free(profile->directories[i]);
        xr_compile_resources_free(profile->owned_paths[i]);
#ifdef XR_OS_WINDOWS
        xr_compile_resources_free(profile->wide[i]);
#endif
    }
}
static bool invocation_input_suffix(XrXirInvocation *owner, const char *path, const char *suffix, char **output) {
    size_t a = 0, b = 0;
    if (!invocation_length(owner, path, &a) || !invocation_length(owner, suffix, &b)) return false;
    if (a > SIZE_MAX - b - 1)
        return invocation_fail(owner, XR_XIR_INVOCATION_OVERFLOW, XR_XIR_INVOCATION_SELF, 0);
    if (a + b > owner->limits.dependencies.path_bytes)
        return invocation_fail(owner, XR_XIR_INVOCATION_BUDGET, XR_XIR_INVOCATION_SELF, 0);
    char *text = NULL;
    bool okay = invocation_resource(owner, xr_compile_resources_alloc(owner->context.resources, a + b + 1, (void **)&text));
    if (okay) okay = invocation_copy(owner,  text, path, a) && invocation_copy(owner,  text + a, suffix, b + 1);
    if (okay) *output = text; else xr_compile_resources_free(text);
    return okay;
}
static bool invocation_input_configs(XrXirInvocation *owner) {
    static const char expected[] =
        "<configuration>\r\n"
        "      <startup>\r\n"
        "        <requiredRuntime safemode=\"true\" imageVersion=\"v4.0.30319\"/>\r\n"
        "        <supportedRuntime version=\"v4.0.30319\"/>\r\n"
        "        <supportedRuntime version=\"v4.5\"/>\r\n"
        "      </startup>\r\n"
        "      <runtime>\r\n"
        "        <assemblyBinding xmlns=\"urn:schemas-microsoft-com:asm.v1\">\r\n"
        "          <publisherPolicy apply=\"no\" />\r\n"
        "        </assemblyBinding>\r\n"
        "      </runtime>\r\n"
        "</configuration>\r\n\r\n";
    _Static_assert(sizeof(expected) - 1 == 409, "The supported configuration is byte-exact");
    for (unsigned i = 0; i < 2; ++i) {
        unsigned stage = i ? 2 : 0;
        owner->diagnostic.stage = (XrXirInvocationStage)stage;
        owner->diagnostic.pass = XR_XIR_INVOCATION_NO_PASS;
        char *path = NULL; void *bytes = NULL; size_t length = 0; XtcXirFileLease *lease = NULL;
        bool okay = invocation_input_suffix(owner, owner->commands[stage].executable, ".config", &path);
        if (okay) okay = invocation_target(owner, xtc_xir_file_lease_open(owner->context.resources, path, &lease));
        if (okay && xtc_xir_file_lease_facts(lease)->length != sizeof(expected) - 1)
            okay = invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
        if (okay) okay = invocation_target(owner, xtc_xir_file_lease_read(lease, sizeof(expected) - 1, &bytes, &length));
        for (size_t at = 0; okay && at < length; ++at) {

            if (okay && ((const char *)bytes)[at] != expected[at])
                okay = invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
        }
        if (okay) okay = invocation_file_add(owner, (XrXirInvocationStage)stage, XR_XIR_INVOCATION_PROVIDER_CONFIG, &lease);
        xr_compile_resources_free(path); xr_compile_resources_free(bytes); xtc_xir_file_lease_free(lease);
        if (!okay) return false;
    }
    return true;
}
static bool invocation_input_fields(const XrXirInvocationRequest *request) {
    return request && request->input_directory && request->output_directory &&
        request->msvc.vc_include && request->msvc.ucrt_include && request->msvc.shared_include &&
        request->msvc.um_include && request->msvc.system_root;
}
#ifdef XR_OS_WINDOWS
static bool invocation_input_wide(XrXirInvocation *owner, const char *path,
    wchar_t **output, size_t *length) {
    wchar_t *wide = NULL;
    if (!invocation_io(owner, xr_win_utf8_path_owned(&owner->io, path, &wide))) return false;
    size_t count = 0;
    for (;;) {

        wchar_t c = wide[count];
        if (!c) { *output = wide; *length = count; return true; }
        if (c == L'/') {

            wide[count] = L'\\';
        }
        ++count;
    }
}
/* relation is equal, left ancestor, right ancestor, or disjoint. Both inputs
 * are already admitted absolute local paths, with normalized separators. */
static bool invocation_input_relation(XrXirInvocation *owner, const wchar_t *left, size_t a,
    const wchar_t *right, size_t b, int *relation) {
    size_t common = a < b ? a : b;
    if (!common || common > INT_MAX)
        return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);

    int order = CompareStringOrdinal(left, (int)common, right, (int)common, TRUE);
    if (!order) return invocation_io(owner, io_windows_status(GetLastError()));
    *relation = 3;
    if (order != CSTR_EQUAL) return true;
    if (a == b) { *relation = 0; return true; }
    const wchar_t *longer = a < b ? right : left;

    if (longer[common - 1] == L'\\' || longer[common] == L'\\') *relation = a < b ? 1 : 2;
    return true;
}
static bool invocation_input_equal(XrXirInvocation *owner, const char *left, const char *right, bool *same) {
    wchar_t *a = NULL, *b = NULL; size_t an = 0, bn = 0; int relation = 3;
    bool okay = invocation_input_wide(owner, left, &a, &an) && invocation_input_wide(owner, right, &b, &bn) &&
        invocation_input_relation(owner, a, an, b, bn, &relation);
    xr_compile_resources_free(a); xr_compile_resources_free(b);
    if (okay) *same = relation == 0;
    return okay;
}
static bool invocation_input_root(XrXirInvocation *owner, unsigned index, const char *path) {
    InvocationInputs *profile = &owner->inputs;
    XrXirTargetStatus status = xtc_xir_file_lease_directory_open(owner->context.resources, path,
        &profile->directories[index]);
    if (status == XR_XIR_TARGET_UNRESOLVED && (index == 2 || index == 3)) {
        char *parent = NULL;
        bool okay = invocation_io(owner, xr_path_dirname_owned(&owner->io, path, &parent));
        if (okay) okay = invocation_target(owner, xtc_xir_file_lease_directory_open(owner->context.resources,
            parent, &profile->directories[index]));
        xr_compile_resources_free(parent);
        if (!okay) return false;
        const char *basename = index == 2 ? "include" : index == 3 ? "generated" : "generated";
        const XtcXirDirectoryFacts *facts = xtc_xir_file_lease_directory_facts(profile->directories[index]);
        if (!invocation_io(owner, xr_path_join_owned(&owner->io, facts->path, basename,
            &profile->owned_paths[index]))) return false;
        profile->paths[index] = profile->owned_paths[index];
        profile->missing[index] = true;
    } else {
        if (!invocation_target(owner, status)) return false;
        profile->paths[index] = xtc_xir_file_lease_directory_facts(profile->directories[index])->path;
    }
    return invocation_input_wide(owner, profile->paths[index], &profile->wide[index], &profile->lengths[index]);
}
static bool invocation_input_join_root(XrXirInvocation *owner, unsigned index, const char *parent, const char *name) {
    char *path = NULL;
    bool okay = invocation_io(owner, xr_path_join_owned(&owner->io, parent, name, &path));
    if (okay) okay = invocation_input_root(owner, index, path);
    xr_compile_resources_free(path); return okay;
}
static bool invocation_input_disjoint(XrXirInvocation *owner, const char *output) {
    XtcXirFileLease *lease = NULL; wchar_t *wide = NULL; size_t length = 0;
    bool okay = invocation_target(owner, xtc_xir_file_lease_directory_open(owner->context.resources, output, &lease));
    if (okay) okay = invocation_input_wide(owner, xtc_xir_file_lease_directory_facts(lease)->path, &wide, &length);
    for (unsigned i = 0; okay && i < 8; ++i) {
        int relation = 0;
        okay = invocation_input_relation(owner, owner->inputs.wide[i], owner->inputs.lengths[i], wide, length, &relation);
        if (okay && relation != 3) okay = invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
        if (okay && owner->inputs.missing[i]) {
            wchar_t *parent = NULL; size_t parent_length = 0;
            okay = invocation_input_wide(owner,
                xtc_xir_file_lease_directory_facts(owner->inputs.directories[i])->path, &parent, &parent_length);
            if (okay) okay = invocation_input_relation(owner, parent, parent_length, wide, length, &relation);
            xr_compile_resources_free(parent);
            if (okay && relation != 3) okay = invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
        }
    }
    xr_compile_resources_free(wide); xtc_xir_file_lease_free(lease); return okay;
}
static bool invocation_inputs_derive(XrXirInvocation *owner, const XrXirInvocationRequest *request) {
    owner->diagnostic.stage = XR_XIR_INVOCATION_NO_STAGE;
    owner->diagnostic.pass = XR_XIR_INVOCATION_NO_PASS;
    if (!invocation_input_root(owner, 0, request->input_directory)) return false;
    static const char *const sdk_parts[] = {"src", "include", "generated"};
    for (unsigned i = 0; i < 3; ++i)
        if (!invocation_input_join_root(owner, i + 1, xr_xir_runtime_sdk_root(request->sdk), sdk_parts[i])) return false;
    const char *external[] = {request->msvc.vc_include, request->msvc.ucrt_include,
        request->msvc.shared_include, request->msvc.um_include};
    for (unsigned i = 0; i < 4; ++i) if (!invocation_input_root(owner, i + 4, external[i])) return false;
    bool same = false;
    if (!invocation_input_equal(owner, owner->commands[0].executable, owner->commands[1].executable, &same)) return false;
    if (!same) return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
    return invocation_input_disjoint(owner, request->output_directory);
}
static bool invocation_input_contains(XrXirInvocation *owner, const char *path) {
    wchar_t *wide = NULL; size_t length = 0;
    if (!invocation_input_wide(owner, path, &wide, &length)) return false;
    bool found = false;
    for (unsigned i = 0; !found && i < 8; ++i) {
        int relation = 3;
        if (!invocation_input_relation(owner, owner->inputs.wide[i], owner->inputs.lengths[i], wide, length, &relation)) break;
        found = relation == 1;
    }
    xr_compile_resources_free(wide);
    return owner->status == XR_XIR_INVOCATION_OK && (found ||
        invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0));
}
#else
static bool invocation_inputs_derive(XrXirInvocation *owner, const XrXirInvocationRequest *request) {
    (void)request; return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
}
static bool invocation_input_contains(XrXirInvocation *owner, const char *path) {
    (void)path; return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
}
#endif


static bool invocation_create(XrXirInvocation *owner, const XrXirInvocationRequest *request) {
    owner->diagnostic.stage = XR_XIR_INVOCATION_NO_STAGE;
    owner->diagnostic.pass = XR_XIR_INVOCATION_NO_PASS;
    const char *directories[2] = {request->input_directory, request->output_directory};
    for (unsigned i = 0; i < 2; ++i)
        if (!invocation_target(owner, xtc_xir_file_lease_directory_open(owner->context.resources,
            directories[i], &owner->directories[i]))) return false;
    if (!invocation_disjoint_directories(owner)) return false;
    for (unsigned i = 0; i < 2; ++i)
        if (!invocation_empty_directory(owner, directories[i])) return false;
    const char *paths[] = {request->compile[0].source, request->compile[1].source,
        request->compile[0].object, request->compile[1].object, request->compile[0].report,
        request->compile[1].report, request->link_report, request->output};
    for (unsigned i = 0; i < 8; ++i) {
        owner->diagnostic.stage = i >= 6 ? XR_XIR_INVOCATION_LINK : (XrXirInvocationStage)(i % 2);
        if (!invocation_child_path(owner, paths[i], directories[i < 2 ? 0 : 1])) return false;
    }
    const XrXirNativeProjectionSource *source = xr_compile_native_projection_source(request->projection);
    owner->diagnostic.stage = XR_XIR_INVOCATION_GENERATED;
    if (source->length > owner->limits.artifact_bytes)
        return invocation_fail(owner, XR_XIR_INVOCATION_BUDGET, XR_XIR_INVOCATION_SELF, 0);
    owner->diagnostic.stage = XR_XIR_INVOCATION_LAUNCHER;
    if (request->launcher_length > owner->limits.artifact_bytes)
        return invocation_fail(owner, XR_XIR_INVOCATION_BUDGET, XR_XIR_INVOCATION_SELF, 0);
    uint8_t launcher_digest[32]; XrSHA256Context hash;

    xr_sha256_init(&hash);

    xr_sha256_update(&hash, request->launcher, request->launcher_length);

    xr_sha256_final(&hash, launcher_digest);
    for (unsigned i = 0; i < 8; ++i) {
        owner->diagnostic.stage = i >= 6 ? XR_XIR_INVOCATION_LINK : (XrXirInvocationStage)(i % 2);
        const void *bytes = i == 0 ? source->text : i == 1 ? request->launcher : "";
        size_t length = i == 0 ? source->length : i == 1 ? request->launcher_length : 0;
        if (!invocation_io(owner, xr_os_io_write_new_file_sync(&owner->io, paths[i], bytes, length))) return false;
    }
    for (unsigned i = 0; i < 2; ++i) {
        owner->diagnostic.stage = (XrXirInvocationStage)i;
        XtcXirFileLease *lease = NULL;
        bool okay = invocation_target(owner, invocation_file_open(owner, paths[i], &lease));
        const XtcXirFileFacts *facts = okay ? xtc_xir_file_lease_facts(lease) : NULL;
        if (okay) okay = facts->length == (i ? request->launcher_length : source->length) &&
            invocation_bytes(owner, facts->digest, i ? launcher_digest : owner->facts.projection.generated_digest.bytes, 32);
        if (okay) okay = invocation_file_add(owner, (XrXirInvocationStage)i, XR_XIR_INVOCATION_SOURCE, &lease);
        xtc_xir_file_lease_free(lease);
        if (!okay) return owner->status != XR_XIR_INVOCATION_OK ? false :
            invocation_fail(owner, XR_XIR_INVOCATION_INPUT_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
    }
    return true;
}
static bool invocation_open_add(XrXirInvocation *owner, const char *path,
    XrXirInvocationStage stage, XrXirInvocationFileKind kind) {
    XtcXirFileLease *lease = NULL;
    bool okay = invocation_target(owner, invocation_file_open(owner, path, &lease));
    if (okay && (kind == XR_XIR_INVOCATION_OBJECT || kind == XR_XIR_INVOCATION_OUTPUT) &&
        xtc_xir_file_lease_facts(lease)->length > owner->limits.artifact_bytes)
        okay = invocation_fail(owner, XR_XIR_INVOCATION_BUDGET, XR_XIR_INVOCATION_SELF, 0);
    if (okay) okay = invocation_file_add(owner, stage, kind, &lease);
    xtc_xir_file_lease_free(lease);
    return okay;
}
static bool invocation_report(XrXirInvocation *owner, const char *path, unsigned stage,
    XrDependencyFacts **output) {
    XtcXirFileLease *lease = NULL;
    void *bytes = NULL;
    size_t length = 0;
    XrDependencyFacts *report = NULL;
    bool okay = invocation_target(owner, xtc_xir_file_lease_open(owner->context.resources, path, &lease));
    if (okay) okay = invocation_target(owner, xtc_xir_file_lease_read(lease,
        owner->limits.dependencies.frame_bytes, &bytes, &length));
    const XrProcessView *command = &owner->commands[stage];
    XrDependencyInput input = {stage == 2 ? XR_DEPENDENCY_MSVC_FULLPATH_RSP_UTF16LE : XR_DEPENDENCY_MSVC_SOURCE_1_2,
        bytes, length, command->argv, (uint32_t)command->argc};
    if (okay) okay = invocation_target(owner, xtc_dependencies_parse(owner->context.resources,
        &input, owner->limits.dependencies, &report));
    if (okay) okay = invocation_file_add(owner, (XrXirInvocationStage)stage,
        XR_XIR_INVOCATION_REPORT, &lease);
    xr_compile_resources_free(bytes);
    xtc_xir_file_lease_free(lease);
    if (okay) *output = report;
    else xtc_dependencies_free(report);
    return okay;
}
static bool invocation_file_find(XrXirInvocation *owner, const XtcXirFileFacts *facts,
    unsigned stage, XrXirInvocationFileKind kind, uint32_t *index) {
    for (uint32_t i = 0; i < owner->facts.file_count; ++i) {

        const XrXirInvocationFile *file = &owner->files[i].facts;
        if (file->stage != (XrXirInvocationStage)stage || file->kind != kind) continue;
        bool same = false;
        if (!invocation_same(owner, facts->path, file->path, &same)) return false;
        if (!same) continue;

        if (facts->length != file->length)
            return invocation_fail(owner, XR_XIR_INVOCATION_INPUT_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
        if (!invocation_bytes(owner, facts->digest, file->digest, 32)) return false;
        *index = i; return true;
    }
    *index = UINT32_MAX; return true;
}
static bool invocation_compile_inputs(XrXirInvocation *owner, const XrDependencyFacts *report,
    unsigned stage) {
    uint32_t count = xtc_dependencies_count(report);
    bool source_seen = false;
    for (uint32_t i = 0; owner->status == XR_XIR_INVOCATION_OK && i < count; ++i) {

        const XrDependencyRecord *record = xtc_dependencies_record(report, i);
        if (!record || (record->kind != XR_DEPENDENCY_SOURCE && record->kind != XR_DEPENDENCY_HEADER)) {
            invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0); break;
        }
        XrXirInvocationFileKind kind = record->kind == XR_DEPENDENCY_SOURCE ?
            XR_XIR_INVOCATION_SOURCE : XR_XIR_INVOCATION_HEADER;
        XtcXirFileLease *lease = NULL;
        bool okay = invocation_target(owner, invocation_file_open(owner, record->path, &lease));
        uint32_t index = UINT32_MAX;
        if (okay) okay = invocation_input_contains(owner, xtc_xir_file_lease_facts(lease)->path);
        if (okay) okay = invocation_file_find(owner, xtc_xir_file_lease_facts(lease), stage, kind, &index);
        if (okay && index == UINT32_MAX) {
            if (kind == XR_XIR_INVOCATION_SOURCE)
                okay = invocation_fail(owner, XR_XIR_INVOCATION_INPUT_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
            else okay = invocation_file_add(owner, (XrXirInvocationStage)stage, kind, &lease);
        }
        if (okay && kind == XR_XIR_INVOCATION_SOURCE) source_seen = true;
        xtc_xir_file_lease_free(lease);
        if (!okay) break;
    }
    if (owner->status == XR_XIR_INVOCATION_OK && !source_seen)
        invocation_fail(owner, XR_XIR_INVOCATION_INPUT_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
    return owner->status == XR_XIR_INVOCATION_OK;
}
static bool invocation_link_inputs(XrXirInvocation *owner, const XrDependencyFacts *report) {
    const XrProcessView *command = &owner->commands[2];
    uint32_t count = xtc_dependencies_count(report), expected = (uint32_t)command->argc - 7;
    if (count != expected)
        return invocation_fail(owner, XR_XIR_INVOCATION_INPUT_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
    uint8_t *seen = NULL;
    if (!invocation_resource(owner, xr_compile_resources_calloc(owner->context.resources,
        owner->facts.file_count, 1, (void **)&seen))) return false;
    for (uint32_t i = 0; owner->status == XR_XIR_INVOCATION_OK && i < count; ++i) {

        const XrDependencyRecord *record = xtc_dependencies_record(report, i);
        if (!record || record->kind != XR_DEPENDENCY_LINK_INPUT) {
            invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0); break;
        }
        XtcXirFileLease *actual = NULL;
        if (!invocation_target(owner, invocation_file_open(owner, record->path, &actual))) break;
        const XtcXirFileFacts *facts = xtc_xir_file_lease_facts(actual);
        bool matched = false;
        for (uint32_t j = 0; owner->status == XR_XIR_INVOCATION_OK && j < owner->facts.file_count; ++j) {

            const XrXirInvocationFile *other = &owner->files[j].facts;
            if (other->kind != XR_XIR_INVOCATION_OBJECT && other->kind != XR_XIR_INVOCATION_SDK_ARCHIVE &&
                other->kind != XR_XIR_INVOCATION_CRT && other->kind != XR_XIR_INVOCATION_SYSTEM) continue;
            if (seen[j]) continue;
            bool same = false;
            bool okay = invocation_same(owner, facts->path, other->path, &same);
            if (okay && same) {

                if (okay && facts->length != other->length)
                    okay = invocation_fail(owner, XR_XIR_INVOCATION_INPUT_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
                if (okay) okay = invocation_bytes(owner, facts->digest, other->digest, 32);
                if (okay) { seen[j] = 1; matched = true; }
            }
            if (!okay || same) break;
        }
        xtc_xir_file_lease_free(actual);
        if (owner->status == XR_XIR_INVOCATION_OK && !matched)
            invocation_fail(owner, XR_XIR_INVOCATION_INPUT_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
    }
    xr_compile_resources_free(seen);
    return owner->status == XR_XIR_INVOCATION_OK;
}
static bool invocation_run_process(XrXirInvocation *owner, const XrXirInvocationRequest *request,
    const XrToolchainProcess *process) {
    XrProcessResult result = {0};
    XrProcessStatus status = xtc_process_run(process, request->cancelled, request->cancel_context, &result);
    bool okay = invocation_process(owner, status);
    if (okay && result.exit_code) {
        owner->diagnostic.exit_code = result.exit_code;
        okay = invocation_fail(owner, XR_XIR_INVOCATION_CHILD_FAILED, XR_XIR_INVOCATION_PROCESS, XTC_PROCESS_OK);
    }
    if (okay && (result.stdout_bytes.truncated || result.stderr_bytes.truncated))
        okay = invocation_fail(owner, XR_XIR_INVOCATION_BUDGET, XR_XIR_INVOCATION_PROCESS, XTC_PROCESS_BUDGET);
    xtc_process_result_free(&result);
    return okay && owner->status == XR_XIR_INVOCATION_OK;
}
static bool invocation_stage(XrXirInvocation *owner, const XrXirInvocationRequest *request, unsigned stage) {
    owner->diagnostic.stage = (XrXirInvocationStage)stage;
    owner->diagnostic.pass = XR_XIR_INVOCATION_EXECUTE;
    const char *path = stage < 2 ? request->compile[stage].report : request->link_report;
    XrDependencyFacts *report = NULL;
    bool okay = invocation_run_process(owner, request, owner->process[stage]);
    if (okay) okay = invocation_report(owner, path, stage, &report);
    if (okay) okay = stage < 2 ? invocation_compile_inputs(owner, report, stage) : invocation_link_inputs(owner, report);
    if (okay) okay = invocation_open_add(owner, stage < 2 ? request->compile[stage].object : request->output,
        (XrXirInvocationStage)stage, stage < 2 ? XR_XIR_INVOCATION_OBJECT : XR_XIR_INVOCATION_OUTPUT);
    if (okay) owner->reports[stage] = report;
    else xtc_dependencies_free(report);
    return okay;
}
static bool invocation_arguments(XrXirInvocation *owner, const XrProcessView *view) {
    for (size_t i = 0; i < view->argc; ++i) {
        const char *argument = view->argv[i];
        for (;;) {

            unsigned char byte = (unsigned char)*argument++;
            if (!byte) break;
            if (byte == '@') return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
        }
    }
    return true;
}
static bool invocation_prepare(XrXirInvocation *owner, const XrXirInvocationRequest *request,
    const char **sdk_libraries) {
    static const char *const paths[5] = {"lib/xray_compile_resources.lib", "lib/xray_xir_admission.lib",
        "lib/xray_xir_declarations.lib", "lib/xray_xir_scalar.lib", "lib/xray_xir_runtime_host.lib"};
    owner->diagnostic.stage = XR_XIR_INVOCATION_LINK;
    owner->diagnostic.pass = XR_XIR_INVOCATION_NO_PASS;
    for (unsigned i = 0; i < 5; ++i)
        if (!invocation_sdk(owner, xr_xir_runtime_sdk_file(request->sdk, paths[i], &sdk_libraries[i]))) return false;
    const XrXirNativeProjectionFacts *facts = xr_compile_native_projection_facts(request->projection);
    const XrXirRuntimeSdkFacts *sdk = xr_xir_runtime_sdk_facts(request->sdk);
    if (!invocation_copy(owner,  &owner->facts.projection, facts, sizeof(*facts)) ||
        !invocation_copy(owner,  &owner->facts.sdk, sdk, sizeof(*sdk))) return false;
    size_t length = 0;
    if (!invocation_length(owner, facts->prefix, &length)) return false;
    if (!invocation_resource(owner, xr_compile_resources_alloc(owner->context.resources,
        length + 1, (void **)&owner->prefix)) || !invocation_copy(owner,  owner->prefix, facts->prefix, length + 1)) return false;
    owner->facts.projection.prefix = owner->prefix;
    for (unsigned i = 0; i < 3; ++i) {
        owner->diagnostic.stage = (XrXirInvocationStage)i;
        const XrToolchainProcess *source = i < 2 ? request->compile[i].process : request->link;
        XrProcessView view;
        if (!invocation_process(owner, xtc_process_view(source, &view))) return false;
        if (view.image_mode != XR_PROC_IMAGES_NONE || view.completion_policy != XR_PROC_COMPLETE_ROOT ||
            view.argc > XTC_PROCESS_MAX_ARGS || view.env_count > XTC_PROCESS_MAX_ENV)
            return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
        XrProcessSpec spec;
        xtc_process_spec_init(&spec, view.executable, view.timeout_ms);
        spec.environment_source = XTC_PROCESS_ENV_EXPLICIT;
        spec.cwd = view.cwd; spec.output_limit = view.output_limit;
        spec.completion_policy = view.completion_policy; spec.env_count = view.env_count;
        for (size_t j = 0; j < view.argc; ++j) spec.argv[j] = view.argv[j];
        for (size_t j = 0; j < view.env_count; ++j) {
            spec.env_keys[j] = view.env_keys[j]; spec.env_values[j] = view.env_values[j];
        }
        if (!invocation_process(owner, xtc_process_prepare(owner->context.resources, &spec, &owner->process[i])) ||
            !invocation_process(owner, xtc_process_view(owner->process[i], &owner->commands[i])) ||
            !invocation_arguments(owner, &owner->commands[i])) return false;
    }
    return invocation_compile_recipe(owner, request, 0) && invocation_compile_recipe(owner, request, 1) &&
        invocation_link_recipe(owner, request, sdk_libraries);
}
/* Pin the actual provider executable before any command runs. The path and
 * version describe those held bytes, not the OS loader's transitive DLL set. */
static bool invocation_provider_read(XrXirInvocation *owner, unsigned stage,
    XrXirInvocationProviderImageFacts *output) {
    owner->diagnostic.stage = (XrXirInvocationStage)stage;
    owner->diagnostic.pass = XR_XIR_INVOCATION_NO_PASS;
    XtcXirFileLease *lease = NULL; void *bytes = NULL; size_t length = 0;
    size_t limit = owner->limits.artifact_bytes > SIZE_MAX ? SIZE_MAX : (size_t)owner->limits.artifact_bytes;
    bool okay = invocation_target(owner, invocation_file_open(owner, owner->commands[stage].executable, &lease));
    if (okay) okay = invocation_target(owner, xtc_xir_file_lease_read(lease, limit, &bytes, &length));
    XtcXirPeVersion version = {0};
    XtcXirPeVersionStatus parsed = okay ? xtc_xir_pe_version_parse(bytes, length, &version) : XTC_XIR_PE_VERSION_OK;
    xr_compile_resources_free(bytes);
    if (okay && parsed != XTC_XIR_PE_VERSION_OK) {
        XrXirInvocationStatus status = parsed == XTC_XIR_PE_VERSION_COMPLEXITY_LIMIT ? XR_XIR_INVOCATION_BUDGET :
            parsed == XTC_XIR_PE_VERSION_NOT_FOUND ? XR_XIR_INVOCATION_UNRESOLVED :
            parsed == XTC_XIR_PE_VERSION_UNSUPPORTED ? XR_XIR_INVOCATION_UNSUPPORTED : XR_XIR_INVOCATION_INVALID;
        okay = invocation_fail(owner, status, XR_XIR_INVOCATION_PE_VERSION, parsed);
    }
    if (okay) {
        const XtcXirFileFacts *facts = xtc_xir_file_lease_facts(lease);
        output->path = facts->path; output->length = facts->length;
        memcpy(output->digest, facts->digest, sizeof(output->digest)); output->version = version;
        okay = invocation_file_add(owner, (XrXirInvocationStage)stage, XR_XIR_INVOCATION_PROVIDER_EXECUTABLE, &lease);
    }
    xtc_xir_file_lease_free(lease);
    return okay;
}
static bool invocation_providers(XrXirInvocation *owner) {
    if (!invocation_provider_read(owner, 0, &owner->provider.compiler) ||
        !invocation_provider_read(owner, 2, &owner->provider.linker)) return false;
    const XrXirInvocationProviderImageFacts *compiler = &owner->provider.compiler;
    /* Both compile recipes use the same frozen executable, pinned above. */
    return invocation_file_publish(owner, XR_XIR_INVOCATION_LAUNCHER,
        XR_XIR_INVOCATION_PROVIDER_EXECUTABLE, compiler->path, compiler->length, compiler->digest, NULL);
}
XR_FUNC XrXirInvocationStatus xtc_xir_invocation_run(const XrXirInvocationRequest *request,
    XrXirInvocation **output, XrXirInvocationDiagnostic *diagnostic) {
    XrXirInvocationDiagnostic initial = {XR_XIR_INVOCATION_NO_STAGE, XR_XIR_INVOCATION_NO_PASS,
        XR_XIR_INVOCATION_SELF, XR_XIR_INVOCATION_INVALID, 0};
    const XrXirCompileContext *context = request ? xr_compile_native_projection_context(request->projection) : NULL;
    if (!invocation_input_fields(request) || !output || *output || !context || !context->resources || !request->sdk ||
        xr_xir_runtime_sdk_resources(request->sdk) != context->resources ||
        xtc_process_resources(request->compile[0].process) != context->resources ||
        xtc_process_resources(request->compile[1].process) != context->resources ||
        xtc_process_resources(request->link) != context->resources ||
        !request->input_directory || !request->output_directory || !request->link_report || !request->output ||
        !request->launcher || !request->launcher_length ||
        !request->limits.dependencies.frame_bytes || !request->limits.dependencies.path_bytes || !request->limits.dependencies.records ||
        !request->limits.artifact_bytes || !request->limits.files || (request->library_count && !request->libraries) ||
        !request->compile[0].source || !request->compile[0].object || !request->compile[0].report ||
        !request->compile[1].source || !request->compile[1].object || !request->compile[1].report) {
        if (diagnostic) *diagnostic = initial;
        return XR_XIR_INVOCATION_INVALID;
    }
    const XrXirRuntimeSdkFacts *sdk = xr_xir_runtime_sdk_facts(request->sdk);
    const XrXirNativeProjectionFacts *projection = xr_compile_native_projection_facts(request->projection);
    if (!invocation_platform() || !sdk || !projection || sdk->architecture != 1 || sdk->crt != 2 || sdk->c_dialect != 11 ||
        projection->source.target.architecture != XR_XIR_ARCH_X86_64) {
        initial.code = XR_XIR_INVOCATION_UNSUPPORTED;
        if (diagnostic) *diagnostic = initial;
        return XR_XIR_INVOCATION_UNSUPPORTED;
    }
    XrXirInvocation *owner = NULL;
    XrCompileResourceStatus allocated = xr_compile_resources_calloc(context->resources, 1, sizeof(*owner), (void **)&owner);
    if (allocated != XR_COMPILE_RESOURCE_OK) {
        initial.domain = XR_XIR_INVOCATION_RESOURCE; initial.code = allocated;
        if (diagnostic) *diagnostic = initial;
        return allocated == XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_INVOCATION_MEMORY_LIMIT :
            allocated == XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_INVOCATION_OVERFLOW :
            allocated == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_INVOCATION_OUT_OF_MEMORY : XR_XIR_INVOCATION_INVALID;
    }
    owner->context.resources = context->resources;
    owner->diagnostic = initial;
    owner->diagnostic.code = XR_XIR_INVOCATION_OK;
    owner->io = xr_compile_io_policy(context->resources);
    bool okay = invocation_copy(owner,  &owner->context, context, sizeof(*context)) &&
        invocation_copy(owner,  &owner->limits, &request->limits, sizeof(request->limits));
#ifdef XR_OS_WINDOWS
    if(okay)okay=invocation_target(owner,xtc_xir_hash_proof_cache_new(context->resources,
        request->limits.files,&owner->proof_cache));
#endif
    const char *sdk_libraries[5] = {0};
    if (okay) okay = invocation_prepare(owner, request, sdk_libraries);
    if (okay) okay = invocation_inputs_derive(owner, request) &&
        invocation_providers(owner) && invocation_input_configs(owner);
    if (okay) {
        owner->diagnostic.stage = XR_XIR_INVOCATION_LINK;
        owner->diagnostic.pass = XR_XIR_INVOCATION_NO_PASS;
    }
    for (unsigned i = 0; okay && i < 5; ++i)
        okay = invocation_open_add(owner, sdk_libraries[i], XR_XIR_INVOCATION_LINK, XR_XIR_INVOCATION_SDK_ARCHIVE);
    for (uint32_t i = 0; okay && i < request->library_count; ++i)
        okay = invocation_open_add(owner, request->libraries[i].path, XR_XIR_INVOCATION_LINK, request->libraries[i].kind);
    if (okay) okay = invocation_create(owner, request);
    for (unsigned stage = 0; okay && stage < 2; ++stage) okay = invocation_stage(owner, request, stage);
    if (okay) okay = invocation_stage(owner, request, 2);

    XrXirInvocationStatus status = owner->status;
    if (diagnostic) *diagnostic = owner->diagnostic;
    if (okay) *output = owner;
    else xtc_xir_invocation_free(owner);
    return status;
}
XR_FUNC XrCompileResources *xtc_xir_invocation_resources(const XrXirInvocation *owner) {
    return owner ? owner->context.resources : NULL;
}
XR_FUNC const XrXirInvocationFacts *xtc_xir_invocation_facts(const XrXirInvocation *owner) {
    return owner ? &owner->facts : NULL;
}
XR_FUNC const XrXirInvocationProviderFacts *xtc_xir_invocation_provider(const XrXirInvocation *owner) {
    return owner ? &owner->provider : NULL;
}
XR_FUNC const XrProcessView *xtc_xir_invocation_command(const XrXirInvocation *owner,
    XrXirInvocationStage stage) {
    return owner && stage >= XR_XIR_INVOCATION_GENERATED && stage <= XR_XIR_INVOCATION_LINK ?
        &owner->commands[stage] : NULL;
}
XR_FUNC const XrXirInvocationFile *xtc_xir_invocation_file(const XrXirInvocation *owner, uint32_t index) {
    return owner && index < owner->facts.file_count ? &owner->files[index].facts : NULL;
}
XR_FUNC XrXirInvocationStatus xtc_xir_invocation_read_output(XrXirInvocation *owner,
    uint64_t limit, void **bytes, size_t *length) {
    if (!owner || !owner->output_lease || !limit || !bytes || *bytes || !length || *length)
        return XR_XIR_INVOCATION_INVALID;
    size_t bounded = limit > SIZE_MAX ? SIZE_MAX : (size_t)limit;
    return invocation_target_status(xtc_xir_file_lease_read(owner->output_lease, bounded, bytes, length));
}
XR_FUNC void xtc_xir_invocation_free(XrXirInvocation *owner) {
    if (!owner) return;
#ifdef XR_OS_WINDOWS
    xtc_xir_hash_proof_cache_free(owner->proof_cache);
#endif
    for (unsigned stage = 0; stage < 3; ++stage) {
        xtc_process_free(owner->process[stage]);
        xtc_dependencies_free(owner->reports[stage]);
    }
    for (uint32_t i = 0; i < owner->facts.file_count; ++i) xtc_xir_file_lease_free(owner->files[i].lease);
    xr_compile_resources_free(owner->files);
    invocation_input_free(&owner->inputs);
    xr_compile_resources_free(owner->prefix);
    for (unsigned i = 0; i < 2; ++i) xtc_xir_file_lease_free(owner->directories[i]);
    xr_compile_resources_free(owner);
}
