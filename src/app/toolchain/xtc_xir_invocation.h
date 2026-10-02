/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_invocation.h - Owned observations from locked native command replay
 */
#ifndef XTC_XIR_INVOCATION_H
#define XTC_XIR_INVOCATION_H
#include "xtc_process.h"
#include "xtc_dependencies.h"
#include "xtc_xir_namespace.h"
#include "xtc_xir_pe_version.h"
#include "../../aot/program/xr_xir_native_projection.h"
#include "../../toolchain/xr_xir_runtime_sdk.h"

typedef enum XrXirInvocationStatus {
    XR_XIR_INVOCATION_OK, XR_XIR_INVOCATION_INVALID,
    XR_XIR_INVOCATION_UNRESOLVED, XR_XIR_INVOCATION_UNSUPPORTED,
    XR_XIR_INVOCATION_BUDGET, XR_XIR_INVOCATION_OUT_OF_MEMORY,
    XR_XIR_INVOCATION_IO, XR_XIR_INVOCATION_TIMEOUT,
    XR_XIR_INVOCATION_CANCELLED, XR_XIR_INVOCATION_CHILD_FAILED,
    XR_XIR_INVOCATION_REPLAY_MISMATCH, XR_XIR_INVOCATION_BROKEN
} XrXirInvocationStatus;
typedef enum XrXirInvocationStage {
    XR_XIR_INVOCATION_GENERATED, XR_XIR_INVOCATION_LAUNCHER,
    XR_XIR_INVOCATION_LINK, XR_XIR_INVOCATION_NO_STAGE
} XrXirInvocationStage;
typedef enum XrXirInvocationPass {
    XR_XIR_INVOCATION_OBSERVE, XR_XIR_INVOCATION_REPLAY,
    XR_XIR_INVOCATION_NO_PASS
} XrXirInvocationPass;
typedef enum XrXirInvocationFailureDomain {
    XR_XIR_INVOCATION_SELF, XR_XIR_INVOCATION_RESOURCE,
    XR_XIR_INVOCATION_FILESYSTEM, XR_XIR_INVOCATION_TARGET,
    XR_XIR_INVOCATION_PROCESS, XR_XIR_INVOCATION_SDK, XR_XIR_INVOCATION_NAMESPACE,
    XR_XIR_INVOCATION_PE_VERSION
} XrXirInvocationFailureDomain;
typedef struct XrXirInvocationDiagnostic {
    XrXirInvocationStage stage;
    XrXirInvocationPass pass;
    XrXirInvocationFailureDomain domain;
    int code, exit_code;
} XrXirInvocationDiagnostic;
typedef enum XrXirInvocationFileKind {
    XR_XIR_INVOCATION_SOURCE, XR_XIR_INVOCATION_OBJECT,
    XR_XIR_INVOCATION_SDK_ARCHIVE, XR_XIR_INVOCATION_CRT,
    XR_XIR_INVOCATION_SYSTEM, XR_XIR_INVOCATION_REPORT,
    XR_XIR_INVOCATION_OUTPUT, XR_XIR_INVOCATION_HEADER,
    XR_XIR_INVOCATION_PROVIDER_IMAGE, XR_XIR_INVOCATION_PROVIDER_CONFIG
} XrXirInvocationFileKind;
typedef struct XrXirInvocationLibrary {
    const char *path;
    /* Only CRT or SYSTEM is accepted here; SDK paths come from its owner. */
    XrXirInvocationFileKind kind;
} XrXirInvocationLibrary;
typedef struct XrXirInvocationCompile {
    const XrToolchainProcess *process;
    const char *source, *object, *report;
} XrXirInvocationCompile;
typedef struct XrXirInvocationLimits {
    XrDependencyLimits dependencies;
    uint64_t artifact_bytes;
    uint32_t files;
} XrXirInvocationLimits;
typedef struct XrXirMsvcRecipe {
    const char *vc_include, *ucrt_include, *shared_include, *um_include, *system_root;
} XrXirMsvcRecipe;
typedef struct XrXirInvocationRequest {
    const XrXirNativeProjection *projection;
    const XrXirRuntimeSdk *sdk;
    XrXirMsvcRecipe msvc;
    XrXirNamespace *namespace_owner;
    XrXirInvocationCompile compile[2];
    const XrToolchainProcess *link;
    const char *input_directory, *output_directory, *link_report, *output;
    const void *launcher;
    size_t launcher_length;
    const XrXirInvocationLibrary *libraries;
    uint32_t library_count;
    XrXirInvocationLimits limits;
    XrProcessCancelled cancelled;
    void *cancel_context;
} XrXirInvocationRequest;
typedef struct XrXirInvocationFile {
    XrXirInvocationStage stage;
    XrXirInvocationFileKind kind;
    const char *path;
    uint64_t length;
    uint8_t digest[32];
} XrXirInvocationFile;
typedef enum XrXirInvocationFactKind {
    XR_XIR_INVOCATION_LOCKED_REPLAY_FACTS = 1
} XrXirInvocationFactKind;
typedef struct XrXirInvocationFacts {
    XrXirInvocationFactKind kind;
    uint32_t completed_runs, file_count;
    XrXirNativeProjectionFacts projection;
    XrXirRuntimeSdkFacts sdk;
} XrXirInvocationFacts;
typedef struct XrXirInvocationProviderImageFacts {
    const char *path;
    uint64_t length;
    uint8_t digest[32];
    XtcXirPeVersion version;
    uint32_t observed_image_index;
} XrXirInvocationProviderImageFacts;
typedef struct XrXirInvocationProviderFacts {
    XrXirInvocationProviderImageFacts compiler, linker;
    uint32_t launcher_compiler_image_index;
} XrXirInvocationProviderFacts;
typedef struct XrXirInvocation XrXirInvocation;

/* Derives the same twelve roots that run validates. Only projection, SDK,
 * three prepared processes, msvc and input/output directories are consumed.
 * namespace_owner and *output must both be NULL. This copies a NEW namespace
 * on the producer ledger without arming it or executing any command. The
 * caller owns finite close/drain. Failure preserves output. */
XR_FUNC XrXirInvocationStatus xtc_xir_invocation_namespace_new(const XrXirInvocationRequest *request,
    const XrXirNamespaceLimits *limits, XrXirNamespace **output, XrXirInvocationDiagnostic *diagnostic);

/* Windows x64 MSVC C11/MD only, with /X and seven ordered include roots.
 * Every actual prepared environment contains exactly SystemRoot, TEMP and TMP
 * in that order; no historical environment-source enum is inferred from it.
 * The synchronous request borrows all producers;
 * their actual ledgers must match before any allocation. The caller supplies
 * two empty, nonoverlapping private directories. Sources are direct children
 * of input_directory; objects, reports and the executable are direct children
 * of output_directory. All commands use input_directory as their frozen cwd.
 * The mandatory same-ledger namespace owner must be NEW and is borrowed. It
 * is armed after source creation and checked after each successful run and
 * before publication. The caller must close/drain it, retaining the surrounding
 * operation and directories while close is PENDING, before public publication.
 * Every artifact is initially CREATE_NEW; existing paths are never overwritten.
 * The caller owns both directories and must later reclaim disk artifacts even
 * on failure. Free releases only this owner's memory and file/image leases.
 * Success records six completed runs and equal observed inputs under positive
 * leases. It grants no Target, namespace or future execution authority.
 * Output must be empty and is unchanged on failure. Diagnostic retains the
 * first typed failure and requires no additional allocation. */
XR_FUNC XrXirInvocationStatus xtc_xir_invocation_run(const XrXirInvocationRequest *request,
    XrXirInvocation **output, XrXirInvocationDiagnostic *diagnostic);
/* All borrows remain valid after request producers are destroyed, until free.
 * Queries neither allocate nor consume work. */
XR_FUNC XrCompileResources *xtc_xir_invocation_resources(const XrXirInvocation *owner);
XR_FUNC const XrXirInvocationFacts *xtc_xir_invocation_facts(const XrXirInvocation *owner);
/* Compiler and linker refer to the GENERATED and LINK observed collectors;
 * launcher_compiler_image_index refers to LAUNCHER's matching compiler row.
 * Version values come from those held image bytes, bounded by artifact_bytes,
 * before the final namespace check. Paths borrow this owner's collectors.
 * These self-reported versions grant no provider or Binding authority. */
XR_FUNC const XrXirInvocationProviderFacts *xtc_xir_invocation_provider(const XrXirInvocation *owner);
XR_FUNC const XrProcessView *xtc_xir_invocation_command(const XrXirInvocation *owner,
    XrXirInvocationStage stage);
XR_FUNC const XrXirInvocationFile *xtc_xir_invocation_file(const XrXirInvocation *owner,
    uint32_t index);
/* Reads the held final OUTPUT lease; no path reopen or second hash. Both outputs
 * must be empty and are preserved on failure. Calls and free are serial.
 * Returned bytes independently retain the original ledger and are released by
 * xr_compile_resources_free, even after all producers and this owner die. */
XR_FUNC XrXirInvocationStatus xtc_xir_invocation_read_output(XrXirInvocation *owner,
    uint64_t limit, void **bytes, size_t *length);
XR_FUNC void xtc_xir_invocation_free(XrXirInvocation *owner);
#endif // XTC_XIR_INVOCATION_H
