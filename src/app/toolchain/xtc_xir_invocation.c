/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_invocation.c - Same-ledger native observations and positive leases
 *
 * KEY CONCEPT:
 *   Replaying frozen commands checks observed inputs while their positive
 *   leases remain held. It does not close the namespace of failed lookups.
 */
#include "xtc_xir_invocation.h"
#include "xtc_xir_file_lease.h"
#include "xtc_xir_images.h"
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
struct XrXirInvocation {
    XrXirCompileContext context;
    XrOsIoPolicy io;
    XrXirInvocationStatus status;
    XrXirInvocationDiagnostic diagnostic;
    XrXirInvocationLimits limits;
    XrXirInvocationFacts facts;
    XrToolchainProcess *process[3];
    XrProcessView commands[3];
    XrXirImageCollector *images[3];
    XrDependencyFacts *reports[3];
    InvocationFile *files;
    uint32_t capacity;
    char *prefix;
    XtcXirFileLease *directories[2];
    XtcXirFileLease *output_lease; /* Borrowed from the owned file table. */
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
    return invocation_fail(owner, status == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_INVOCATION_BUDGET :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_INVOCATION_OUT_OF_MEMORY :
        XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_RESOURCE, status);
}
static bool invocation_work(XrXirInvocation *owner, uint64_t amount) {
    return owner->status == XR_XIR_INVOCATION_OK &&
        invocation_resource(owner, xr_compile_resources_work(owner->context.resources, amount));
}
static bool invocation_io(XrXirInvocation *owner, XrOsIoStatus status) {
    if (status == XR_OS_IO_OK) return true;
    XrXirInvocationStatus mapped;
    switch (status) {
    case XR_OS_IO_NOT_FOUND: mapped = XR_XIR_INVOCATION_UNRESOLVED; break;
    case XR_OS_IO_UNSUPPORTED: mapped = XR_XIR_INVOCATION_UNSUPPORTED; break;
    case XR_OS_IO_BUDGET: mapped = XR_XIR_INVOCATION_BUDGET; break;
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
    case XR_XIR_TARGET_IO: mapped = XR_XIR_INVOCATION_IO; break;
    default: mapped = XR_XIR_INVOCATION_INVALID; break;
    }
    return mapped;
}
static bool invocation_target(XrXirInvocation *owner, XrXirTargetStatus status) {
    return status == XR_XIR_TARGET_OK ||
        invocation_fail(owner, invocation_target_status(status), XR_XIR_INVOCATION_TARGET, status);
}
static bool invocation_namespace(XrXirInvocation *owner, XrXirNamespaceStatus status) {
    if (status == XR_XIR_NAMESPACE_OK) return true;
    XrXirInvocationStatus mapped;
    switch (status) {
    case XR_XIR_NAMESPACE_UNRESOLVED: mapped = XR_XIR_INVOCATION_UNRESOLVED; break;
    case XR_XIR_NAMESPACE_UNSUPPORTED: mapped = XR_XIR_INVOCATION_UNSUPPORTED; break;
    case XR_XIR_NAMESPACE_BUDGET: mapped = XR_XIR_INVOCATION_BUDGET; break;
    case XR_XIR_NAMESPACE_OUT_OF_MEMORY: mapped = XR_XIR_INVOCATION_OUT_OF_MEMORY; break;
    case XR_XIR_NAMESPACE_IO: mapped = XR_XIR_INVOCATION_IO; break;
    case XR_XIR_NAMESPACE_BROKEN: mapped = XR_XIR_INVOCATION_BROKEN; break;
    default: mapped = XR_XIR_INVOCATION_INVALID; break;
    }
    return invocation_fail(owner, mapped, XR_XIR_INVOCATION_NAMESPACE, status);
}
static bool invocation_process(XrXirInvocation *owner, XrProcessStatus status) {
    if (status == XTC_PROCESS_OK) return true;
    XrXirInvocationStatus mapped;
    switch (status) {
    case XTC_PROCESS_UNRESOLVED: mapped = XR_XIR_INVOCATION_UNRESOLVED; break;
    case XTC_PROCESS_UNSUPPORTED: mapped = XR_XIR_INVOCATION_UNSUPPORTED; break;
    case XTC_PROCESS_BUDGET: mapped = XR_XIR_INVOCATION_BUDGET; break;
    case XTC_PROCESS_OUT_OF_MEMORY: mapped = XR_XIR_INVOCATION_OUT_OF_MEMORY; break;
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
    case XR_XIR_SDK_IO: mapped = XR_XIR_INVOCATION_IO; break;
    default: mapped = XR_XIR_INVOCATION_INVALID; break;
    }
    return invocation_fail(owner, mapped, XR_XIR_INVOCATION_SDK, status);
}
static bool invocation_same(XrXirInvocation *owner, const char *a, const char *b, bool *same) {
    if (!a || !b) return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
    for (;;) {
        if (!invocation_work(owner, 2)) return false;
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
        if (!invocation_work(owner, 2)) return false;
        if (left[i] != right[i]) return invocation_fail(owner, XR_XIR_INVOCATION_REPLAY_MISMATCH,
            XR_XIR_INVOCATION_SELF, 0);
    }
    return true;
}
static bool invocation_length(XrXirInvocation *owner, const char *text, size_t *length) {
    if (!text) return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
    for (size_t i = 0;; ++i) {
        if (!invocation_work(owner, 1)) return false;
        unsigned char byte = (unsigned char)text[i];
        if (!byte) { *length = i; return true; }
        if (i >= owner->limits.dependencies.path_bytes)
            return invocation_fail(owner, XR_XIR_INVOCATION_BUDGET, XR_XIR_INVOCATION_SELF, 0);
    }
}
static bool invocation_copy(XrXirInvocation *owner, void *destination, const void *source, size_t bytes) {
    if (!invocation_work(owner, bytes)) return false;
    memcpy(destination, source, bytes); return true;
}
static bool invocation_file_add(XrXirInvocation *owner, XrXirInvocationStage stage,
    XrXirInvocationFileKind kind, XtcXirFileLease **lease) {
    if (owner->facts.file_count == owner->limits.files)
        return invocation_fail(owner, XR_XIR_INVOCATION_BUDGET, XR_XIR_INVOCATION_SELF, 0);
    if (owner->facts.file_count == owner->capacity) {
        uint32_t capacity = owner->capacity > UINT32_MAX / 2 ? owner->limits.files :
            owner->capacity ? owner->capacity * 2 : 16;
        if (capacity > owner->limits.files) capacity = owner->limits.files;
        if ((uint64_t)capacity * sizeof(*owner->files) > SIZE_MAX)
            return invocation_fail(owner, XR_XIR_INVOCATION_BUDGET, XR_XIR_INVOCATION_SELF, 0);
        if (!invocation_resource(owner, xr_compile_resources_resize(owner->context.resources,
            (void **)&owner->files, (size_t)capacity * sizeof(*owner->files)))) return false;
        owner->capacity = capacity;
    }
    const XtcXirFileFacts *facts = xtc_xir_file_lease_facts(*lease);
    if (!facts) return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
    if (!invocation_work(owner, sizeof(InvocationFile) + sizeof(facts->digest))) return false;
    InvocationFile *file = &owner->files[owner->facts.file_count];
    *file = (InvocationFile){{stage, kind, facts->path, facts->length, {0}}, *lease};
    memcpy(file->facts.digest, facts->digest, sizeof(facts->digest));
    if (kind == XR_XIR_INVOCATION_OUTPUT) owner->output_lease = *lease;
    *lease = NULL; ++owner->facts.file_count;
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
        if (!invocation_work(owner, 1)) return false;
        unsigned char part = (unsigned char)*prefix++;
        if (!part) break;
        if (!invocation_work(owner, 1)) return false;
        if ((unsigned char)*actual++ != part)
            return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
    }
    return invocation_expect(owner, actual, suffix);
}
static bool invocation_environment(XrXirInvocation *owner, const XrProcessView *view) {
#ifdef XR_OS_WINDOWS
    static const wchar_t *const forbidden[] = {L"CL", L"_CL_", L"LINK", L"_LINK_"};
    for (size_t i = 0; i < view->env_count; ++i) {
        wchar_t *key = NULL;
        if (!invocation_io(owner, xr_win_utf8_path_owned(&owner->io, view->env_keys[i], &key))) return false;
        size_t units = 0;
        for (;;) {
            if (!invocation_work(owner, sizeof(wchar_t))) break;
            if (!key[units]) break;
            ++units;
        }
        for (size_t j = 0; owner->status == XR_XIR_INVOCATION_OK && j < 4; ++j) {
            size_t length = j == 0 ? 2 : j == 3 ? 6 : 4;
            if (!invocation_work(owner, (units + length) * sizeof(wchar_t) + 1)) break;
            int order = CompareStringOrdinal(key, (int)units, forbidden[j], (int)length, TRUE);
            if (!order) { invocation_io(owner, io_windows_status(GetLastError())); break; }
            if (order == CSTR_EQUAL) {
                if (!invocation_work(owner, 1)) break;
                if (view->env_values[i][0]) invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED,
                    XR_XIR_INVOCATION_SELF, 0);
            }
        }
        xr_compile_resources_free(key);
        if (owner->status != XR_XIR_INVOCATION_OK) return false;
    }
    return true;
#else
    (void)view;
    return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
#endif
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
    if (view->argc != 21 + stage)
        return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
    for (unsigned i = 0; i < 12; ++i)
        if (!invocation_expect(owner, view->argv[i + 1], fixed[i])) return false;
    static const char *const directories[] = {"src", "include", "generated"};
    for (unsigned i = 0; i < 3; ++i) {
        char *path = NULL;
        if (!invocation_io(owner, xr_path_join_owned(&owner->io, xr_xir_runtime_sdk_root(request->sdk),
            directories[i], &path))) return false;
        bool okay = invocation_join_argument(owner, view->argv[13 + i], "/I", path);
        xr_compile_resources_free(path);
        if (!okay) return false;
    }
    if (!invocation_expect(owner, view->argv[16], "/c") ||
        !invocation_expect(owner, view->argv[17], compile->source) ||
        !invocation_join_argument(owner, view->argv[18], "/Fo", compile->object) ||
        !invocation_expect(owner, view->argv[19], "/sourceDependencies") ||
        !invocation_expect(owner, view->argv[20], compile->report)) return false;
    if (stage) {
        size_t length = 0;
        if (!invocation_length(owner, owner->prefix, &length)) return false;
        char symbol[73];
        if (length > 64) return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
        if (!invocation_copy(owner, symbol, owner->prefix, length) ||
            !invocation_copy(owner, symbol + length, "_program", sizeof("_program"))) return false;
        if (!invocation_join_argument(owner, view->argv[21], "/DXIR_SDK_PROGRAM_SYMBOL=", symbol)) return false;
    }
    return invocation_expect(owner, view->cwd, request->input_directory) && invocation_environment(owner, view);
}
static bool invocation_link_recipe(XrXirInvocation *owner, const XrXirInvocationRequest *request,
    const char *const sdk_libraries[5]) {
    owner->diagnostic.stage = XR_XIR_INVOCATION_LINK;
    owner->diagnostic.pass = XR_XIR_INVOCATION_NO_PASS;
    const XrProcessView *view = &owner->commands[2];
    if (request->library_count > XTC_PROCESS_MAX_ARGS - 14 || view->argc != 14 + request->library_count)
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
        if (!invocation_work(owner, sizeof(request->libraries[i].kind))) return false;
        if (request->libraries[i].kind != XR_XIR_INVOCATION_CRT && request->libraries[i].kind != XR_XIR_INVOCATION_SYSTEM)
            return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
        if (!invocation_expect(owner, view->argv[14 + i], request->libraries[i].path)) return false;
    }
    return invocation_expect(owner, view->cwd, request->input_directory) && invocation_environment(owner, view);
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
        if (!invocation_work(owner, 1)) return false;
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
            if (!invocation_work(owner, sizeof(wchar_t))) return false;
            if (!facts[i]->native_path[lengths[i]]) break;
            if (++lengths[i] > INT_MAX)
                return invocation_fail(owner, XR_XIR_INVOCATION_BUDGET, XR_XIR_INVOCATION_SELF, 0);
        }
    }
    unsigned shorter = lengths[0] <= lengths[1] ? 0 : 1, longer = shorter ^ 1;
    size_t common = lengths[shorter];
    if (!common) return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
    if (!invocation_work(owner, 2 * common * sizeof(wchar_t) + 1)) return false;
    int order = CompareStringOrdinal(facts[shorter]->native_path, (int)common,
        facts[longer]->native_path, (int)common, TRUE);
    if (!order) return invocation_io(owner, io_windows_status(GetLastError()));
    if (order != CSTR_EQUAL) return true;
    if (lengths[0] == lengths[1])
        return invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
    if (!invocation_work(owner, 2 * sizeof(wchar_t))) return false;
    wchar_t last = facts[shorter]->native_path[common - 1], next = facts[longer]->native_path[common];
    return (last != L'\\' && last != L'/' && next != L'\\' && next != L'/') ||
        invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0);
#else
    return invocation_fail(owner, XR_XIR_INVOCATION_UNSUPPORTED, XR_XIR_INVOCATION_SELF, 0);
#endif
}
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
    if (!invocation_work(owner, 1)) return false;
    xr_sha256_init(&hash);
    if (!invocation_work(owner, request->launcher_length)) return false;
    xr_sha256_update(&hash, request->launcher, request->launcher_length);
    if (!invocation_work(owner, 1)) return false;
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
        bool okay = invocation_target(owner, xtc_xir_file_lease_open(owner->context.resources, paths[i], &lease));
        const XtcXirFileFacts *facts = okay ? xtc_xir_file_lease_facts(lease) : NULL;
        if (okay) okay = facts->length == (i ? request->launcher_length : source->length) &&
            invocation_bytes(owner, facts->digest, i ? launcher_digest : owner->facts.projection.generated_digest.bytes, 32);
        if (okay) okay = invocation_file_add(owner, (XrXirInvocationStage)i, XR_XIR_INVOCATION_SOURCE, &lease);
        xtc_xir_file_lease_free(lease);
        if (!okay) return owner->status != XR_XIR_INVOCATION_OK ? false :
            invocation_fail(owner, XR_XIR_INVOCATION_REPLAY_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
    }
    return true;
}
static bool invocation_open_add(XrXirInvocation *owner, const char *path,
    XrXirInvocationStage stage, XrXirInvocationFileKind kind) {
    XtcXirFileLease *lease = NULL;
    bool okay = invocation_target(owner, xtc_xir_file_lease_open(owner->context.resources, path, &lease));
    if (okay && (kind == XR_XIR_INVOCATION_OBJECT || kind == XR_XIR_INVOCATION_OUTPUT) &&
        xtc_xir_file_lease_facts(lease)->length > owner->limits.artifact_bytes)
        okay = invocation_fail(owner, XR_XIR_INVOCATION_BUDGET, XR_XIR_INVOCATION_SELF, 0);
    if (okay) okay = invocation_file_add(owner, stage, kind, &lease);
    xtc_xir_file_lease_free(lease);
    return okay;
}
static bool invocation_report(XrXirInvocation *owner, const char *path, unsigned stage,
    bool final, XrDependencyFacts **output) {
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
    if (okay && final) okay = invocation_file_add(owner, (XrXirInvocationStage)stage,
        XR_XIR_INVOCATION_REPORT, &lease);
    /* Initial report leases end here; replay must be able to rewrite them. */
    xr_compile_resources_free(bytes);
    xtc_xir_file_lease_free(lease);
    if (okay) *output = report;
    else xtc_dependencies_free(report);
    return okay;
}
static bool invocation_file_find(XrXirInvocation *owner, const XtcXirFileFacts *facts,
    unsigned stage, XrXirInvocationFileKind kind, uint32_t *index) {
    for (uint32_t i = 0; i < owner->facts.file_count; ++i) {
        if (!invocation_work(owner, sizeof(owner->files[i].facts.stage) + sizeof(owner->files[i].facts.kind))) return false;
        const XrXirInvocationFile *file = &owner->files[i].facts;
        if (file->stage != (XrXirInvocationStage)stage || file->kind != kind) continue;
        bool same = false;
        if (!invocation_same(owner, facts->path, file->path, &same)) return false;
        if (!same) continue;
        if (!invocation_work(owner, 2 * sizeof(uint64_t))) return false;
        if (facts->length != file->length)
            return invocation_fail(owner, XR_XIR_INVOCATION_REPLAY_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
        if (!invocation_bytes(owner, facts->digest, file->digest, 32)) return false;
        *index = i; return true;
    }
    *index = UINT32_MAX; return true;
}
static bool invocation_compile_inputs(XrXirInvocation *owner, const XrDependencyFacts *report,
    unsigned stage, bool replay) {
    uint32_t count = xtc_dependencies_count(report), file_count = owner->facts.file_count;
    uint8_t *seen = NULL;
    if (replay && !invocation_resource(owner, xr_compile_resources_calloc(owner->context.resources,
        file_count, 1, (void **)&seen))) return false;
    bool source_seen = false;
    for (uint32_t i = 0; owner->status == XR_XIR_INVOCATION_OK && i < count; ++i) {
        if (!invocation_work(owner, 1)) break;
        const XrDependencyRecord *record = xtc_dependencies_record(report, i);
        if (!record || (record->kind != XR_DEPENDENCY_SOURCE && record->kind != XR_DEPENDENCY_HEADER)) {
            invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0); break;
        }
        XrXirInvocationFileKind kind = record->kind == XR_DEPENDENCY_SOURCE ?
            XR_XIR_INVOCATION_SOURCE : XR_XIR_INVOCATION_HEADER;
        XtcXirFileLease *lease = NULL;
        bool okay = invocation_target(owner, xtc_xir_file_lease_open(owner->context.resources, record->path, &lease));
        uint32_t index = UINT32_MAX;
        if (okay) okay = invocation_file_find(owner, xtc_xir_file_lease_facts(lease), stage, kind, &index);
        if (okay && index == UINT32_MAX) {
            if (replay || kind == XR_XIR_INVOCATION_SOURCE)
                okay = invocation_fail(owner, XR_XIR_INVOCATION_REPLAY_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
            else okay = invocation_file_add(owner, (XrXirInvocationStage)stage, kind, &lease);
        }
        if (okay && replay && invocation_work(owner, 1)) seen[index] = 1;
        if (okay && kind == XR_XIR_INVOCATION_SOURCE) source_seen = true;
        xtc_xir_file_lease_free(lease);
        if (!okay) break;
    }
    if (owner->status == XR_XIR_INVOCATION_OK && !source_seen)
        invocation_fail(owner, XR_XIR_INVOCATION_REPLAY_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
    if (replay) for (uint32_t i = 0; owner->status == XR_XIR_INVOCATION_OK && i < file_count; ++i) {
        if (!invocation_work(owner, sizeof(owner->files[i].facts.stage) + sizeof(owner->files[i].facts.kind) + 1)) break;
        const XrXirInvocationFile *file = &owner->files[i].facts;
        if (file->stage == (XrXirInvocationStage)stage &&
            (file->kind == XR_XIR_INVOCATION_SOURCE || file->kind == XR_XIR_INVOCATION_HEADER) && !seen[i])
            invocation_fail(owner, XR_XIR_INVOCATION_REPLAY_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
    }
    xr_compile_resources_free(seen);
    return owner->status == XR_XIR_INVOCATION_OK;
}
static bool invocation_link_inputs(XrXirInvocation *owner, const XrDependencyFacts *report) {
    const XrProcessView *command = &owner->commands[2];
    uint32_t count = xtc_dependencies_count(report), expected = (uint32_t)command->argc - 7;
    if (count != expected)
        return invocation_fail(owner, XR_XIR_INVOCATION_REPLAY_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
    uint8_t *seen = NULL;
    if (!invocation_resource(owner, xr_compile_resources_calloc(owner->context.resources,
        owner->facts.file_count, 1, (void **)&seen))) return false;
    for (uint32_t i = 0; owner->status == XR_XIR_INVOCATION_OK && i < count; ++i) {
        if (!invocation_work(owner, 1)) break;
        const XrDependencyRecord *record = xtc_dependencies_record(report, i);
        if (!record || record->kind != XR_DEPENDENCY_LINK_INPUT) {
            invocation_fail(owner, XR_XIR_INVOCATION_INVALID, XR_XIR_INVOCATION_SELF, 0); break;
        }
        XtcXirFileLease *actual = NULL;
        if (!invocation_target(owner, xtc_xir_file_lease_open(owner->context.resources, record->path, &actual))) break;
        const XtcXirFileFacts *facts = xtc_xir_file_lease_facts(actual);
        bool matched = false;
        for (uint32_t j = 0; owner->status == XR_XIR_INVOCATION_OK && j < owner->facts.file_count; ++j) {
            if (!invocation_work(owner, 1 + sizeof(owner->files[j].facts.kind))) break;
            const XrXirInvocationFile *other = &owner->files[j].facts;
            if (other->kind != XR_XIR_INVOCATION_OBJECT && other->kind != XR_XIR_INVOCATION_SDK_ARCHIVE &&
                other->kind != XR_XIR_INVOCATION_CRT && other->kind != XR_XIR_INVOCATION_SYSTEM) continue;
            if (seen[j]) continue;
            bool same = false;
            bool okay = invocation_same(owner, facts->path, other->path, &same);
            if (okay && same) {
                okay = invocation_work(owner, 2 * sizeof(uint64_t));
                if (okay && facts->length != other->length)
                    okay = invocation_fail(owner, XR_XIR_INVOCATION_REPLAY_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
                if (okay) okay = invocation_bytes(owner, facts->digest, other->digest, 32);
                if (okay && invocation_work(owner, 1)) { seen[j] = 1; matched = true; }
            }
            if (!okay || same) break;
        }
        xtc_xir_file_lease_free(actual);
        if (owner->status == XR_XIR_INVOCATION_OK && !matched)
            invocation_fail(owner, XR_XIR_INVOCATION_REPLAY_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
    }
    xr_compile_resources_free(seen);
    return owner->status == XR_XIR_INVOCATION_OK;
}
static bool invocation_image_inputs(XrXirInvocation *owner, const XrXirImageCollector *replay, unsigned stage) {
    const XrXirImageCollector *first = owner->images[stage];
    uint32_t count = xtc_xir_images_count(first);
    if (count != xtc_xir_images_count(replay))
        return invocation_fail(owner, XR_XIR_INVOCATION_REPLAY_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
    for (uint32_t i = 0; i < count; ++i) {
        const XrXirImageFile *image = xtc_xir_images_file(first, i);
        bool found = false;
        for (uint32_t j = 0; j < count; ++j) {
            const XrXirImageFile *other = xtc_xir_images_file(replay, j);
            bool same = false;
            if (!invocation_same(owner, image->path, other->path, &same)) return false;
            if (!same) continue;
            if (!invocation_work(owner, 2 * (sizeof(uint32_t) + sizeof(uint64_t)))) return false;
            if (image->kind_mask != other->kind_mask || image->length != other->length)
                return invocation_fail(owner, XR_XIR_INVOCATION_REPLAY_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
            if (!invocation_bytes(owner, image->digest, other->digest, 32)) return false;
            found = true; break;
        }
        if (!found) return invocation_fail(owner, XR_XIR_INVOCATION_REPLAY_MISMATCH, XR_XIR_INVOCATION_SELF, 0);
    }
    return true;
}
static bool invocation_run_process(XrXirInvocation *owner, const XrXirInvocationRequest *request,
    const XrToolchainProcess *process, XrXirImageCollector *images) {
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
    if (okay) okay = invocation_namespace(owner, xtc_xir_namespace_check(request->namespace_owner));
    if (okay) okay = invocation_target(owner, xtc_xir_images_seal(images));
    if (okay && invocation_work(owner, 1)) ++owner->facts.completed_runs;
    return okay && owner->status == XR_XIR_INVOCATION_OK;
}
static bool invocation_stage(XrXirInvocation *owner, const XrXirInvocationRequest *request, unsigned stage) {
    owner->diagnostic.stage = (XrXirInvocationStage)stage;
    owner->diagnostic.pass = XR_XIR_INVOCATION_OBSERVE;
    const char *path = stage < 2 ? request->compile[stage].report : request->link_report;
    XrDependencyFacts *observed = NULL, *replayed = NULL;
    XrXirImageCollector *images = NULL;
    XrToolchainProcess *process = NULL;
    bool okay = invocation_run_process(owner, request, owner->process[stage], owner->images[stage]);
    if (okay) okay = invocation_report(owner, path, stage, false, &observed);
    if (okay) okay = stage < 2 ? invocation_compile_inputs(owner, observed, stage, false) : invocation_link_inputs(owner, observed);
    if (okay) {
        owner->diagnostic.pass = XR_XIR_INVOCATION_REPLAY;
        okay = invocation_target(owner, xtc_xir_images_new(owner->context.resources, &images));
    }
    if (okay) {
        XrProcImageObserver observer = xtc_xir_images_observer(images);
        okay = invocation_process(owner, xtc_process_clone_observed(owner->process[stage], &observer, &process));
    }
    if (okay) okay = invocation_run_process(owner, request, process, images);
    if (okay) okay = invocation_image_inputs(owner, images, stage);
    if (okay) okay = invocation_report(owner, path, stage, true, &replayed);
    if (okay) okay = stage < 2 ? invocation_compile_inputs(owner, replayed, stage, true) : invocation_link_inputs(owner, replayed);
    if (okay) okay = invocation_open_add(owner, stage < 2 ? request->compile[stage].object : request->output,
        (XrXirInvocationStage)stage, stage < 2 ? XR_XIR_INVOCATION_OBJECT : XR_XIR_INVOCATION_OUTPUT);
    xtc_process_free(process);
    xtc_xir_images_free(images);
    xtc_dependencies_free(observed);
    if (okay) owner->reports[stage] = replayed;
    else xtc_dependencies_free(replayed);
    return okay;
}
static bool invocation_arguments(XrXirInvocation *owner, const XrProcessView *view) {
    for (size_t i = 0; i < view->argc; ++i) {
        const char *argument = view->argv[i];
        for (;;) {
            if (!invocation_work(owner, 1)) return false;
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
    if (!invocation_copy(owner, &owner->facts.projection, facts, sizeof(*facts)) ||
        !invocation_copy(owner, &owner->facts.sdk, sdk, sizeof(*sdk))) return false;
    size_t length = 0;
    if (!invocation_length(owner, facts->prefix, &length)) return false;
    if (!invocation_resource(owner, xr_compile_resources_alloc(owner->context.resources,
        length + 1, (void **)&owner->prefix)) || !invocation_copy(owner, owner->prefix, facts->prefix, length + 1)) return false;
    owner->facts.projection.prefix = owner->prefix;
    for (unsigned i = 0; i < 3; ++i) {
        owner->diagnostic.stage = (XrXirInvocationStage)i;
        if (!invocation_target(owner, xtc_xir_images_new(owner->context.resources, &owner->images[i]))) return false;
        XrProcImageObserver observer = xtc_xir_images_observer(owner->images[i]);
        const XrToolchainProcess *source = i < 2 ? request->compile[i].process : request->link;
        if (!invocation_process(owner, xtc_process_clone_observed(source, &observer, &owner->process[i])) ||
            !invocation_process(owner, xtc_process_view(owner->process[i], &owner->commands[i])) ||
            !invocation_arguments(owner, &owner->commands[i])) return false;
    }
    return invocation_compile_recipe(owner, request, 0) && invocation_compile_recipe(owner, request, 1) &&
        invocation_link_recipe(owner, request, sdk_libraries);
}
static bool invocation_finish(XrXirInvocation *owner) {
    for (unsigned stage = 0; stage < 3; ++stage) {
        owner->diagnostic.stage = (XrXirInvocationStage)stage;
        owner->diagnostic.pass = XR_XIR_INVOCATION_NO_PASS;
        uint32_t count = xtc_xir_images_count(owner->images[stage]);
        for (uint32_t i = 0; i < count; ++i) {
            const XrXirImageFile *image = xtc_xir_images_file(owner->images[stage], i);
            if (!invocation_open_add(owner, image->path, (XrXirInvocationStage)stage, XR_XIR_INVOCATION_PROVIDER_IMAGE)) return false;
        }
    }
    if (!invocation_work(owner, sizeof(owner->facts.kind))) return false;
    owner->facts.kind = XR_XIR_INVOCATION_LOCKED_REPLAY_FACTS;
    return true;
}
XR_FUNC XrXirInvocationStatus xtc_xir_invocation_run(const XrXirInvocationRequest *request,
    XrXirInvocation **output, XrXirInvocationDiagnostic *diagnostic) {
    XrXirInvocationDiagnostic initial = {XR_XIR_INVOCATION_NO_STAGE, XR_XIR_INVOCATION_NO_PASS,
        XR_XIR_INVOCATION_SELF, XR_XIR_INVOCATION_INVALID, 0};
    const XrXirCompileContext *context = request ? xr_compile_native_projection_context(request->projection) : NULL;
    if (!request || !output || *output || !context || !context->resources || !request->sdk ||
        xr_xir_runtime_sdk_resources(request->sdk) != context->resources ||
        !request->namespace_owner ||
        xtc_xir_namespace_resources(request->namespace_owner) != context->resources ||
        xtc_xir_namespace_phase(request->namespace_owner) != XR_XIR_NAMESPACE_NEW ||
        xtc_xir_namespace_status(request->namespace_owner) != XR_XIR_NAMESPACE_OK ||
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
        return allocated == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_INVOCATION_OUT_OF_MEMORY : XR_XIR_INVOCATION_BUDGET;
    }
    owner->context.resources = context->resources;
    owner->diagnostic = initial;
    owner->diagnostic.code = XR_XIR_INVOCATION_OK;
    owner->io = xr_compile_io_policy(context->resources);
    bool okay = invocation_copy(owner, &owner->context, context, sizeof(*context)) &&
        invocation_copy(owner, &owner->limits, &request->limits, sizeof(request->limits));
    const char *sdk_libraries[5] = {0};
    if (okay) okay = invocation_prepare(owner, request, sdk_libraries);
    if (okay) {
        owner->diagnostic.stage = XR_XIR_INVOCATION_LINK;
        owner->diagnostic.pass = XR_XIR_INVOCATION_NO_PASS;
    }
    for (unsigned i = 0; okay && i < 5; ++i)
        okay = invocation_open_add(owner, sdk_libraries[i], XR_XIR_INVOCATION_LINK, XR_XIR_INVOCATION_SDK_ARCHIVE);
    for (uint32_t i = 0; okay && i < request->library_count; ++i)
        okay = invocation_open_add(owner, request->libraries[i].path, XR_XIR_INVOCATION_LINK, request->libraries[i].kind);
    if (okay) okay = invocation_create(owner, request);
    if (okay) {
        owner->diagnostic.stage = XR_XIR_INVOCATION_NO_STAGE;
        owner->diagnostic.pass = XR_XIR_INVOCATION_NO_PASS;
        okay = invocation_namespace(owner, xtc_xir_namespace_arm(request->namespace_owner));
    }
    for (unsigned stage = 0; okay && stage < 2; ++stage) okay = invocation_stage(owner, request, stage);
    if (okay) okay = invocation_stage(owner, request, 2);
    if (okay) okay = invocation_finish(owner);
    if (okay) {
        owner->diagnostic.stage = XR_XIR_INVOCATION_NO_STAGE;
        owner->diagnostic.pass = XR_XIR_INVOCATION_NO_PASS;
        okay = invocation_namespace(owner, xtc_xir_namespace_check(request->namespace_owner));
    }
    if (okay) okay = invocation_work(owner, sizeof(owner));
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
    for (unsigned stage = 0; stage < 3; ++stage) {
        xtc_process_free(owner->process[stage]);
        xtc_dependencies_free(owner->reports[stage]);
        xtc_xir_images_free(owner->images[stage]);
    }
    for (uint32_t i = 0; i < owner->facts.file_count; ++i) xtc_xir_file_lease_free(owner->files[i].lease);
    xr_compile_resources_free(owner->files);
    xr_compile_resources_free(owner->prefix);
    for (unsigned i = 0; i < 2; ++i) xtc_xir_file_lease_free(owner->directories[i]);
    xr_compile_resources_free(owner);
}
