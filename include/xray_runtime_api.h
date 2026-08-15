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
 *   both artifacts are required to resolve one. The only installed executor is
 *   the closed scalar i64 route, so a resolved export is callable only when its
 *   verified rows are scalar i64 and its generation reached ACTIVE. Every other
 *   export fails closed with a stable diagnostic instead of executing.
 *
 *   Today those two facts do not overlap, and this header does not pretend
 *   otherwise. Publishing a source export requires source-namespace shared
 *   storage, and the installed activation gate admits only a sole scalar i64
 *   function with no storage authority at all. So a module this runtime can
 *   load publishes no export, and a module that publishes one cannot load.
 *   Lookup and call below are written against the real verified tables and
 *   the runtime's canonical entry cell rather than stubbed. They refuse rather
 *   than fabricate an entry: until the general typed executor is installed,
 *   `xr_module_find_export` reports that the module publishes no such name and
 *   `xr_export_call` remains structurally unreachable.
 */

#ifndef XRAY_RUNTIME_API_H
#define XRAY_RUNTIME_API_H

#include "xray_export.h"
#include "xray_runtime_generation.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct XrRuntime XrRuntime;
typedef struct XrModule XrModule;
typedef struct XrExport XrExport;

/*
 * There is no separate facade schema constant. The budget this runtime accepts
 * is the generation budget, and its own schema version is the one the
 * generation authority validates, so nothing here negotiates a second one.
 */

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
 * Creating a runtime creates the generation authority behind it, so the budget
 * is the same hard budget that authority checks. Destroying a runtime requires
 * every module it loaded to be unloaded first.
 */
XRAY_API bool xr_runtime_create(const XrRuntimeGenerationBudget *budget,
                                XrRuntime **runtime, char *diagnostic,
                                size_t diagnostic_size);
XRAY_API bool xr_runtime_destroy(XrRuntime **runtime, char *diagnostic,
                                 size_t diagnostic_size);

/*
 * Loads one module from an exact artifact pair. The semantic artifact is the
 * authority the target artifact must bind, so neither is optional and neither
 * is inferred from the other. Success means the generation reached ACTIVE
 * under the installed executor; a plan the executor does not own is rejected
 * here rather than at call time, and no partially loaded module is returned.
 */
XRAY_API bool xr_module_load_target_plan(
    XrRuntime *runtime, const uint8_t *semantic_artifact_bytes,
    size_t semantic_artifact_size, const uint8_t *target_artifact_bytes,
    size_t target_artifact_size, XrModule **module, char *diagnostic,
    size_t diagnostic_size);

/*
 * Resolves a name against the module's verified source export table. The
 * returned handle is a loan owned by the module: it stays valid until that
 * module is unloaded and must not be freed by the caller. Resolution binds the
 * module's canonical entry cell to its verified TargetPlan and exact generation
 * identity. An unknown name, an export outside the installed execution family,
 * and an export whose generation is not ACTIVE each fail closed and yield no
 * handle.
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
