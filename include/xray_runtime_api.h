/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_runtime_api.h - Verified artifact runtime facade
 *
 * KEY CONCEPT:
 *   A runtime owns a generation authority. A module is one verified semantic
 *   authority plus the verified TargetPlan generation it admits. Every entry
 *   point below refuses whatever it cannot prove: this facade never guesses an
 *   entry, never reinterprets an artifact, and never reports a success it did
 *   not execute.
 *
 * CAPABILITY BOUNDARY:
 *   Export names live in the semantic artifact, never in the TargetPlan, so
 *   both artifacts are required to resolve one. A module export is callable
 *   only when the closed scalar-i64 executor owns its verified rows and its
 *   generation reached ACTIVE. Every other
 *   export fails closed with a stable diagnostic instead of executing.
 *
 *   Exact scalar i64 source exports and their exact plan-required provider and
 *   deallocator-finalizer operations are registered as one bounded activation
 *   transaction after the complete plan and instruction program verify. The
 *   module becomes visible only after every registration is committed; a
 *   duplicate key, missing binding, or exhausted budget rolls the whole
 *   generation back. Other export representations remain unsupported and fail
 *   activation rather than reaching lookup or execution through a fallback.
 *   The distinct opaque program facade admits only the exact bounded
 *   direct-i64 graph described below; it is neither an export-name fallback
 *   nor a general graph executor.
 */

#ifndef XRAY_RUNTIME_API_H
#define XRAY_RUNTIME_API_H

#include "xray_export.h"
#include "xray_runtime_generation.h"
#include "xray_target_plan_load.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct XrRuntime XrRuntime;
typedef struct XrModule XrModule;
typedef struct XrProgram XrProgram;
typedef struct XrExport XrExport;

#define XR_RUNTIME_CONFIG_SCHEMA_VERSION UINT32_C(2)

typedef void *(*XrRuntimeAllocateProvider)(void *context, size_t size,
                                           size_t alignment);
/* This is the exact DEAllocates/Consumes-Owned operation from the verified
 * allocator provider contract. It is the module activation finalizer in this
 * facade; it is not an object-layout destructor. */
typedef void (*XrRuntimeDeallocateFinalizer)(void *context, void *allocation,
                                             size_t size, size_t alignment);
/* The hosted panic contract unwinds to the runtime boundary so assertPanics
 * can observe it without substituting the typed-error channel. */
typedef void (*XrRuntimePanicProvider)(void *context, const char *message,
                                      size_t message_size);

typedef struct XrRuntimeProviderBindings {
    XrRuntimeAllocateProvider allocate;
    void *allocate_context;
    XrRuntimeDeallocateFinalizer deallocate;
    void *deallocate_context;
    XrRuntimePanicProvider panic;
    void *panic_context;
} XrRuntimeProviderBindings;

typedef struct XrRuntimeActivationBudget {
    uint32_t max_active_entries;
    uint32_t max_active_provider_registrations;
    uint32_t max_active_finalizer_registrations;
    uint32_t reserved;
} XrRuntimeActivationBudget;

typedef struct XrRuntimeConfig {
    /* Exact XR_RUNTIME_CONFIG_SCHEMA_VERSION; reserved fields must be zero. */
    uint32_t schema_version;
    uint32_t reserved;
    XrRuntimeGenerationBudget generation;
    XrRuntimeActivationBudget activation;
    XrRuntimeProviderBindings providers;
} XrRuntimeConfig;

/*
 * The only representation the installed executor owns. A caller states the
 * kind of every value it passes and reads the kind of what it gets back, so a
 * widened executor cannot silently reinterpret an old caller's payload.
 */
typedef enum XrExportValueKind {
    XR_EXPORT_VALUE_I64 = 0,
    XR_EXPORT_VALUE_KIND_COUNT,
} XrExportValueKind;

typedef struct XrExportValue {
    uint32_t kind;
    uint32_t reserved;
    int64_t i64;
} XrExportValue;

/*
 * Creating a runtime creates the generation authority behind it and installs
 * an explicit process-local provider binding set. The bindings are candidates,
 * not published module authority: a verified plan must require their exact
 * provider contracts before one activation transaction can register them.
 * There is no implicit native fallback. Destroying a runtime requires every
 * artifact it loaded to be unloaded first.
 */
XRAY_API bool xr_runtime_create(const XrRuntimeConfig *config,
                                XrRuntime **runtime, char *diagnostic,
                                size_t diagnostic_size);
XRAY_API bool xr_runtime_destroy(XrRuntime **runtime, char *diagnostic,
                                 size_t diagnostic_size);

/*
 * Loads one module from an exact artifact pair. The semantic artifact is the
 * authority the target artifact must bind, so neither is optional and neither
 * is inferred from the other. Success means the generation reached ACTIVE
 * under the installed executor; a plan the executor does not own is rejected
 * here rather than at call time. Every exact source export and plan-required
 * provider/deallocator-finalizer operation is registered as one atomic batch
 * before the module handle is published, and no partially loaded module or
 * generation remains on any failure.
 */
XRAY_API bool xr_module_load_target_plan(
    XrRuntime *runtime, const uint8_t *semantic_artifact_bytes,
    size_t semantic_artifact_size, const uint8_t *target_artifact_bytes,
    size_t target_artifact_size, XrModule **module, char *diagnostic,
    size_t diagnostic_size);

/* Loads the bounded canonical program-graph capability from an arbitrarily
 * ordered XSM image vector plus one exact XTP. The images are canonicalized
 * only by their verified program-module rows. The returned opaque program is
 * not a module-local export namespace: it owns no name/index recovery route
 * and uses the runtime's existing generation, decoded-cache, and live-manifest
 * owners. The current executor admits only the exact verified direct-i64 graph
 * family; other program graphs fail during load. */
XRAY_API bool xr_program_load_target_plan(
    XrRuntime *runtime, const XrRuntimeArtifactImage *semantic_artifacts,
    uint32_t semantic_artifact_count,
    const uint8_t *target_artifact_bytes, size_t target_artifact_size,
    XrProgram **program, char *diagnostic, size_t diagnostic_size);

/* Executes only the unique entry_target_function in the live verified graph.
 * Every call rechecks the published manifest identity, program/module-set
 * fingerprints, GCI, TargetPlan, and decoded cache before dispatch. Multiple
 * execute calls may run concurrently. Unload is a lifecycle boundary rather
 * than dynamic reload: the caller must stop and join every execute call before
 * calling xr_program_unload; concurrent execute/unload is unsupported. */
XRAY_API bool xr_program_execute_direct_i64(
    const XrProgram *program, int64_t *result, char *diagnostic,
    size_t diagnostic_size);

/* Releases a quiescent program after all execute calls have been joined. */
XRAY_API bool xr_program_unload(XrProgram **program, char *diagnostic,
                                size_t diagnostic_size);

/*
 * Resolves a name against the module's verified source export table. The
 * returned handle is a loan owned by the module: it stays valid until that
 * module is unloaded and must not be freed by the caller. Resolution binds the
 * module's canonical entry cell to its verified TargetPlan and exact generation
 * identity during activation. Lookup performs no registration or mutation. An
 * unknown name and an export whose generation is not ACTIVE each fail closed
 * and yield no handle.
 */
XRAY_API bool xr_module_find_export(const XrModule *module,
                                    const char *export_name,
                                    const XrExport **export_handle,
                                    char *diagnostic, size_t diagnostic_size);

/*
 * Calls a resolved export. The argument count must equal the parameter count
 * the verified rows declare and every value kind must be one the executor
 * owns; a shorter, longer, or differently typed vector is refused rather than
 * truncated or zero filled. The entry cell validates the callee ABI, adapter,
 * executor, TargetPlan, binding, and generation fingerprints before it obtains
 * an in-flight pin. That pin is released exactly once on every exit, so the
 * module cannot unload underneath the call.
 */
XRAY_API bool xr_export_call(const XrExport *export_handle,
                             const XrExportValue *arguments,
                             uint32_t argument_count, XrExportValue *result,
                             char *diagnostic, size_t diagnostic_size);

/*
 * Drains, retires, and unloads a module, then releases its verified plan and
 * semantic authority. It fails closed while any in-flight call, callback,
 * destructor, or static-root pin remains, and every export handle the module
 * loaned out is invalid once it succeeds.
 */
XRAY_API bool xr_module_unload(XrModule **module, char *diagnostic,
                               size_t diagnostic_size);

#endif  // XRAY_RUNTIME_API_H
