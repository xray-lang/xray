/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_namespace.h - Owned invalidation of explicit directory intervals
 */
#ifndef XTC_XIR_NAMESPACE_H
#define XTC_XIR_NAMESPACE_H
#include "../../base/xcompile_resources.h"

typedef enum XrXirNamespaceStatus {
    XR_XIR_NAMESPACE_OK, XR_XIR_NAMESPACE_INVALID, XR_XIR_NAMESPACE_UNRESOLVED,
    XR_XIR_NAMESPACE_UNSUPPORTED, XR_XIR_NAMESPACE_BUDGET, XR_XIR_NAMESPACE_OUT_OF_MEMORY,
    XR_XIR_NAMESPACE_IO, XR_XIR_NAMESPACE_BROKEN, XR_XIR_NAMESPACE_PENDING
} XrXirNamespaceStatus;
typedef enum XrXirNamespacePhase {
    XR_XIR_NAMESPACE_NEW, XR_XIR_NAMESPACE_ARMED, XR_XIR_NAMESPACE_FAILED, XR_XIR_NAMESPACE_CLOSING
} XrXirNamespacePhase;
typedef enum XrXirNamespaceScope { XR_XIR_NAMESPACE_DIRECTORY, XR_XIR_NAMESPACE_TREE } XrXirNamespaceScope;
typedef struct XrXirNamespaceRoot { const char *path; XrXirNamespaceScope scope; } XrXirNamespaceRoot;
typedef struct XrXirNamespaceLimits { uint32_t roots, directories, depth, path_bytes; } XrXirNamespaceLimits;
typedef struct XrXirNamespaceRequest {
    const XrXirNamespaceRoot *roots;
    uint32_t root_count;
    XrXirNamespaceLimits limits;
} XrXirNamespaceRequest;
typedef struct XrXirNamespaceRootFacts {
    const char *requested_path, *watched_path;
    XrXirNamespaceScope scope;
    bool initially_missing;
} XrXirNamespaceRootFacts;
typedef struct XrXirNamespaceDirectoryFacts {
    const char *path;
    uint64_t volume;
    uint8_t file_id[16];
    bool recursive;
} XrXirNamespaceDirectoryFacts;
typedef struct XrXirNamespaceFacts { uint32_t root_count, directory_count; bool armed; } XrXirNamespaceFacts;
typedef struct XrXirNamespaceDiagnostic { XrXirNamespaceStatus status; uint32_t os_error; } XrXirNamespaceDiagnostic;
typedef struct XrXirNamespace XrXirNamespace;

/* Copies the complete request on the mandatory original ledger, without I/O.
 * Output must be NULL and is preserved on failure. All owner calls are serial.
 * Windows local NTFS drive paths are admitted when arm is called. */
XR_FUNC XrXirNamespaceStatus xtc_xir_namespace_new(XrCompileResources *resources,
    const XrXirNamespaceRequest *request, XrXirNamespace **output);
/* Each directory is armed before enumeration. TREE includes existing child
 * directories; DIRECTORY does not. A missing root watches only its nearest
 * existing parent's direct entries. Any failed arm retains the owner for close.
 * No event is ignored, including writes made by this process. Writable output
 * and cache locations must be outside the roots admitted by a later recipe. */
XR_FUNC XrXirNamespaceStatus xtc_xir_namespace_arm(XrXirNamespace *owner);
/* A successful check describes only the interval of these explicit directories.
 * Breaks are sticky even if enumeration later returns the original names.
 * It grants no complete provider namespace, Target or future execution right. */
XR_FUNC XrXirNamespaceStatus xtc_xir_namespace_check(XrXirNamespace *owner);
/* Requests cancellation, then waits at most the finite total wait request.
 * PENDING preserves the owner and all asynchronous storage until actual I/O
 * completion; a cancellation result alone never permits freeing it. Unknown
 * completion or cleanup failures retain the owner and may be retried. No new
 * allocation or budget permission is needed. OK frees and nulls the owner;
 * a NULL owner is already closed. Read the first diagnostic before closing.
 * The OS scheduler does not promise a hard real-time wall-clock bound. */
XR_FUNC XrXirNamespaceStatus xtc_xir_namespace_close(XrXirNamespace **owner, uint32_t wait_ms);
XR_FUNC XrCompileResources *xtc_xir_namespace_resources(const XrXirNamespace *owner);
XR_FUNC XrXirNamespaceStatus xtc_xir_namespace_status(const XrXirNamespace *owner);
XR_FUNC XrXirNamespacePhase xtc_xir_namespace_phase(const XrXirNamespace *owner);
XR_FUNC const XrXirNamespaceDiagnostic *xtc_xir_namespace_diagnostic(const XrXirNamespace *owner);
XR_FUNC const XrXirNamespaceFacts *xtc_xir_namespace_facts(const XrXirNamespace *owner);
XR_FUNC const XrXirNamespaceRootFacts *xtc_xir_namespace_root(const XrXirNamespace *owner, uint32_t index);
XR_FUNC const XrXirNamespaceDirectoryFacts *xtc_xir_namespace_directory(const XrXirNamespace *owner, uint32_t index);
#endif // XTC_XIR_NAMESPACE_H
