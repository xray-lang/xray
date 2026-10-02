/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_namespace.c - Explicit directory guards on one compiler ledger
 *
 * KEY CONCEPT:
 *   Pending kernel requests own their storage until completion is observed.
 */
#include "xtc_xir_namespace.h"
#include "xtc_xir_file_lease.h"
#include "../../base/xfileio.h"
#include "../../base/xio_policy.h"
#include "../../os/os_dir.h"
#include <string.h>
#ifdef XR_OS_WINDOWS
#include "../../base/xwindows_utf8.h"
#include <winioctl.h>
#endif

typedef struct NamespaceDirectory {
    XtcXirFileLease *lease;
    XrXirNamespaceDirectoryFacts facts;
    uint32_t depth;
    bool expanded;
#ifdef XR_OS_WINDOWS
    HANDLE handle;
    OVERLAPPED overlapped;
    REQUEST_OPLOCK_INPUT_BUFFER input;
    REQUEST_OPLOCK_OUTPUT_BUFFER output;
    bool issued, completed, cancellation_requested;
#endif
} NamespaceDirectory;
struct XrXirNamespace {
    XrCompileResources *resources;
    XrOsIoPolicy io;
    XrXirNamespaceLimits limits;
    XrXirNamespacePhase phase;
    XrXirNamespaceDiagnostic diagnostic;
    XrXirNamespaceFacts facts;
    XrXirNamespaceRootFacts *roots;
    NamespaceDirectory **directories;
    uint32_t capacity;
};
static bool namespace_fail(XrXirNamespace *owner, XrXirNamespaceStatus status, uint32_t error) {
    if (owner->diagnostic.status == XR_XIR_NAMESPACE_OK) {
        owner->diagnostic = (XrXirNamespaceDiagnostic){.status=status, .os_error=error};
        owner->facts.armed = false;
        if (owner->phase != XR_XIR_NAMESPACE_CLOSING) owner->phase = XR_XIR_NAMESPACE_FAILED;
    }
    return false;
}
static XrXirNamespaceStatus namespace_resource_status(XrCompileResourceStatus status) {
    return status == XR_COMPILE_RESOURCE_OK ? XR_XIR_NAMESPACE_OK :
        status == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_NAMESPACE_BUDGET :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_NAMESPACE_OUT_OF_MEMORY : XR_XIR_NAMESPACE_INVALID;
}
static bool namespace_resource(XrXirNamespace *owner, XrCompileResourceStatus status) {
    return status == XR_COMPILE_RESOURCE_OK || namespace_fail(owner, namespace_resource_status(status), 0);
}
static bool namespace_work(XrXirNamespace *owner, uint64_t work) {
    return owner->diagnostic.status == XR_XIR_NAMESPACE_OK &&
        namespace_resource(owner, xr_compile_resources_work(owner->resources, work));
}
static bool namespace_io(XrXirNamespace *owner, XrOsIoStatus status) {
    if (status == XR_OS_IO_OK) return true;
    XrXirNamespaceStatus result = XR_XIR_NAMESPACE_INVALID;
    switch (status) {
    case XR_OS_IO_NOT_FOUND: result = XR_XIR_NAMESPACE_UNRESOLVED; break;
    case XR_OS_IO_UNSUPPORTED: result = XR_XIR_NAMESPACE_UNSUPPORTED; break;
    case XR_OS_IO_BUDGET: result = XR_XIR_NAMESPACE_BUDGET; break;
    case XR_OS_IO_OUT_OF_MEMORY: result = XR_XIR_NAMESPACE_OUT_OF_MEMORY; break;
    case XR_OS_IO_IO: result = XR_XIR_NAMESPACE_IO; break;
    default: break;
    }
    return namespace_fail(owner, result, 0);
}
static bool namespace_target(XrXirNamespace *owner, XrXirTargetStatus status) {
    if (status == XR_XIR_TARGET_OK) return true;
    XrXirNamespaceStatus result = XR_XIR_NAMESPACE_INVALID;
    switch (status) {
    case XR_XIR_TARGET_UNRESOLVED: result = XR_XIR_NAMESPACE_UNRESOLVED; break;
    case XR_XIR_TARGET_UNSUPPORTED: result = XR_XIR_NAMESPACE_UNSUPPORTED; break;
    case XR_XIR_TARGET_BUDGET: result = XR_XIR_NAMESPACE_BUDGET; break;
    case XR_XIR_TARGET_OUT_OF_MEMORY: result = XR_XIR_NAMESPACE_OUT_OF_MEMORY; break;
    case XR_XIR_TARGET_IO: result = XR_XIR_NAMESPACE_IO; break;
    default: break;
    }
    return namespace_fail(owner, result, 0);
}
static bool namespace_length(XrXirNamespace *owner, const char *text, size_t *length) {
    for (size_t i = 0;; ++i) {
        if (!namespace_work(owner, 1)) return false;
        if (!text[i]) { *length = i; return true; }
        if (i >= owner->limits.path_bytes) return namespace_fail(owner, XR_XIR_NAMESPACE_BUDGET, 0);
    }
}
static bool namespace_copy_text(XrXirNamespace *owner, const char *text, char **output) {
    size_t length = 0;
    if (!namespace_length(owner, text, &length)) return false;
    void *copy = NULL;
    if (!namespace_resource(owner, xr_compile_resources_alloc(owner->resources, length + 1, &copy))) return false;
    if (!namespace_work(owner, length + 1)) { xr_compile_resources_free(copy); return false; }
    memcpy(copy, text, length + 1); *output = copy; return true;
}
static void namespace_free(XrXirNamespace *owner) {
    for (uint32_t i = 0; i < owner->facts.directory_count; ++i) {
        NamespaceDirectory *node = owner->directories[i];
        xtc_xir_file_lease_free(node->lease); xr_compile_resources_free(node);
    }
    for (uint32_t i = 0; i < owner->facts.root_count; ++i)
        xr_compile_resources_free((void *)owner->roots[i].requested_path);
    xr_compile_resources_free(owner->directories);
    xr_compile_resources_free(owner->roots); xr_compile_resources_free(owner);
}
XR_FUNC XrXirNamespaceStatus xtc_xir_namespace_new(XrCompileResources *resources,
    const XrXirNamespaceRequest *request, XrXirNamespace **output) {
    if (!resources || !request || !output || *output || !request->roots || !request->root_count ||
        !request->limits.roots || !request->limits.directories || !request->limits.path_bytes)
        return XR_XIR_NAMESPACE_INVALID;
    if (request->root_count > request->limits.roots ||
        (uint64_t)request->root_count * sizeof(XrXirNamespaceRootFacts) > SIZE_MAX)
        return XR_XIR_NAMESPACE_BUDGET;
    XrXirNamespace *owner = NULL;
    XrCompileResourceStatus made = xr_compile_resources_calloc(resources, 1, sizeof(*owner), (void **)&owner);
    if (made != XR_COMPILE_RESOURCE_OK) return namespace_resource_status(made);
    owner->resources = resources; owner->io = xr_compile_io_policy(resources);
    if (!namespace_work(owner, sizeof(request->limits))) goto done;
    owner->limits = request->limits;
    if (!namespace_resource(owner, xr_compile_resources_calloc(resources, request->root_count,
        sizeof(*owner->roots), (void **)&owner->roots))) goto done;
    for (uint32_t i = 0; i < request->root_count; ++i) {
        if (!namespace_work(owner, sizeof(XrXirNamespaceRoot))) break;
        XrXirNamespaceRoot root = request->roots[i];
        if (!root.path || (root.scope != XR_XIR_NAMESPACE_DIRECTORY && root.scope != XR_XIR_NAMESPACE_TREE)) {
            namespace_fail(owner, XR_XIR_NAMESPACE_INVALID, 0); break;
        }
        char *path = NULL;
        if (!namespace_copy_text(owner, root.path, &path)) break;
        owner->roots[i] = (XrXirNamespaceRootFacts){path, NULL, root.scope, false};
        ++owner->facts.root_count;
    }
done:
    if (owner->diagnostic.status == XR_XIR_NAMESPACE_OK && namespace_work(owner, sizeof(owner))) {
        *output = owner; return XR_XIR_NAMESPACE_OK;
    }
    XrXirNamespaceStatus status = owner->diagnostic.status;
    namespace_free(owner); return status;
}
#ifdef XR_OS_WINDOWS
#include "xtc_xir_namespace_windows.inc.c"
#endif
XR_FUNC XrXirNamespaceStatus xtc_xir_namespace_arm(XrXirNamespace *owner) {
    if (!owner) return XR_XIR_NAMESPACE_INVALID;
    if (owner->diagnostic.status != XR_XIR_NAMESPACE_OK) return owner->diagnostic.status;
    if (owner->phase != XR_XIR_NAMESPACE_NEW) return XR_XIR_NAMESPACE_INVALID;
#ifdef XR_OS_WINDOWS
    for (uint32_t i = 0; i < owner->facts.root_count; ++i)
        if (!namespace_root_arm(owner, &owner->roots[i])) return owner->diagnostic.status;
    /* A previously direct root may become recursive through another root.
     * Visit the shallowest outstanding node, including earlier table entries. */
    for (;;) {
        NamespaceDirectory *next = NULL;
        for (uint32_t i = 0; i < owner->facts.directory_count; ++i) {
            NamespaceDirectory *node = owner->directories[i];
            if (!namespace_work(owner, 2 + sizeof(node->depth))) return owner->diagnostic.status;
            if (node->facts.recursive && !node->expanded && (!next || node->depth < next->depth)) next = node;
        }
        if (!next) break;
        if (!namespace_expand(owner, next)) return owner->diagnostic.status;
    }
    for (uint32_t i = 0; i < owner->facts.directory_count; ++i)
        if (!namespace_observe(owner, owner->directories[i], i, 0, false)) return owner->diagnostic.status;
    if (namespace_work(owner, sizeof(owner->phase) + sizeof(owner->facts.armed))) {
        owner->phase = XR_XIR_NAMESPACE_ARMED; owner->facts.armed = true;
    }
#else
    namespace_fail(owner, XR_XIR_NAMESPACE_UNSUPPORTED, 0);
#endif
    return owner->diagnostic.status;
}
XR_FUNC XrXirNamespaceStatus xtc_xir_namespace_check(XrXirNamespace *owner) {
    if (!owner) return XR_XIR_NAMESPACE_INVALID;
    if (owner->diagnostic.status != XR_XIR_NAMESPACE_OK) return owner->diagnostic.status;
    if (owner->phase != XR_XIR_NAMESPACE_ARMED) return XR_XIR_NAMESPACE_INVALID;
#ifdef XR_OS_WINDOWS
    for (uint32_t i = 0; i < owner->facts.directory_count; ++i)
        if (!namespace_observe(owner, owner->directories[i], i, 0, false)) break;
#else
    namespace_fail(owner, XR_XIR_NAMESPACE_UNSUPPORTED, 0);
#endif
    return owner->diagnostic.status;
}
XR_FUNC XrXirNamespaceStatus xtc_xir_namespace_close(XrXirNamespace **slot, uint32_t wait_ms) {
    if (!slot || wait_ms == UINT32_MAX) return XR_XIR_NAMESPACE_INVALID;
    XrXirNamespace *owner = *slot;
    if (!owner) return XR_XIR_NAMESPACE_OK;
    owner->phase = XR_XIR_NAMESPACE_CLOSING; owner->facts.armed = false;
#ifdef XR_OS_WINDOWS
    if (!namespace_drain(owner, wait_ms)) return XR_XIR_NAMESPACE_PENDING;
#endif
    namespace_free(owner); *slot = NULL; return XR_XIR_NAMESPACE_OK;
}
XR_FUNC XrCompileResources *xtc_xir_namespace_resources(const XrXirNamespace *owner) { return owner ? owner->resources : NULL; }
XR_FUNC XrXirNamespaceStatus xtc_xir_namespace_status(const XrXirNamespace *owner) {
    return owner ? owner->diagnostic.status : XR_XIR_NAMESPACE_INVALID;
}
XR_FUNC XrXirNamespacePhase xtc_xir_namespace_phase(const XrXirNamespace *owner) {
    return owner ? owner->phase : XR_XIR_NAMESPACE_FAILED;
}
XR_FUNC const XrXirNamespaceDiagnostic *xtc_xir_namespace_diagnostic(const XrXirNamespace *owner) {
    return owner ? &owner->diagnostic : NULL;
}
XR_FUNC const XrXirNamespaceFacts *xtc_xir_namespace_facts(const XrXirNamespace *owner) { return owner ? &owner->facts : NULL; }
XR_FUNC const XrXirNamespaceRootFacts *xtc_xir_namespace_root(const XrXirNamespace *owner, uint32_t index) {
    return owner && index < owner->facts.root_count ? &owner->roots[index] : NULL;
}
XR_FUNC const XrXirNamespaceDirectoryFacts *xtc_xir_namespace_directory(const XrXirNamespace *owner, uint32_t index) {
    return owner && index < owner->facts.directory_count ? &owner->directories[index]->facts : NULL;
}
