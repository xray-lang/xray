/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_isolate.h - Isolate: independent VM execution environment
 *
 * KEY CONCEPT:
 *   An Isolate is a complete, isolated instance of the Xray bytecode VM.
 *   Each Isolate has its own VM state, RC heap, stack, globals, and type registry.
 *   One Isolate per thread is the typical usage pattern.
 */

#ifndef XRAY_ISOLATE_H
#define XRAY_ISOLATE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "xray_export.h"

// An Isolate is a complete instance of the Xray bytecode VM with its own
// VM state, RC heap, stack, globals, type registry, and execution backend.
typedef struct XrayIsolate XrayIsolate;

typedef enum {
    XRAY_BACKEND_BYTECODE,  // Bytecode interpreter (default, fast startup)
} XrayBackendType;

typedef struct {
    /* === Backend === */
    XrayBackendType backend_type;

    /* === Memory === */
    size_t initial_heap_size;  // 0 = use default
    size_t max_heap_size;      // 0 = unlimited

    /* === Debug === */
    bool trace_execution;
    bool dump_bytecode;
    bool dump_ic_feedback;  // Dump IC type feedback after execution

    /* === User Data === */
    void *userdata;

    /* === Script Info (set at runtime) === */
    const char *script_file;  // Script path (for __file__)
    int script_argc;
    char **script_argv;
} XrayIsolateParams;

/* ========== Core API ========== */

// Create a minimal bytecode VM isolate. Pass NULL for default params.
// Returns NULL on failure.
XRAY_API XrayIsolate *xray_isolate_new(const XrayIsolateParams *params);

// Create a bytecode VM isolate with runtime ABI classes but no compiler/module loader.
// Used by bytecode-embedded runtimes that need coroutine/runtime-backed values.
XRAY_API XrayIsolate *xray_isolate_new_runtime(const XrayIsolateParams *params);

// Create a full bytecode VM isolate with compiler, classes, modules, reflection, etc.
XRAY_API XrayIsolate *xray_isolate_new_full(const XrayIsolateParams *params);

// Initialize params with bytecode VM defaults.
XRAY_API void xray_isolate_params_init(XrayIsolateParams *params);

// Destroy Isolate and free all resources. Safe to pass NULL.
XRAY_API void xray_isolate_delete(XrayIsolate *isolate);

// Execute source code. Returns 0 on success, -1 on error.
XRAY_API int xray_isolate_dostring(XrayIsolate *isolate, const char *source);

// Execute source file. Returns 0 on success, -1 on error.
XRAY_API int xray_isolate_dofile(XrayIsolate *isolate, const char *filename);

// Execute source file with debug support (DAP).
// Returns 0 on success, -1 on error. out_proto receives compiled proto for debugging.
XRAY_API int xray_isolate_dofile_debug(XrayIsolate *isolate, const char *filename,
                                       void **out_proto);

/* ========== Advanced API ========== */

// Get current backend type (determined at compile time via CMake)
XRAY_API XrayBackendType xray_isolate_get_backend(XrayIsolate *isolate);

XRAY_API void xray_isolate_set_userdata(XrayIsolate *isolate, void *userdata);
XRAY_API void *xray_isolate_get_userdata(XrayIsolate *isolate);

/* ========== Statistics and Debugging ========== */

XRAY_API void xray_isolate_get_stats(XrayIsolate *isolate, size_t *bytes_allocated,
                                     int *cycle_count);

XRAY_API void xray_isolate_collect_garbage(XrayIsolate *isolate);

XRAY_API void xray_isolate_set_trace(XrayIsolate *isolate, bool enable);

XRAY_API void xray_isolate_set_dump_bytecode(XrayIsolate *isolate, bool enable);

// Set script info. Accessible in script as: process.args, __file__, __dir__
XRAY_API void xray_isolate_set_script_info(XrayIsolate *isolate, const char *script_file, int argc,
                                           char **argv);

/* ========== Embedded Execution Policy ========== */

// Redirect this isolate's user output (the `print()` builtin / OP_PRINT)
// to `stream`. Pass NULL to restore the default (process stdout). The
// host owns `stream`; it must outlive the isolate or be reset before
// closing.
//
// Debug/trace output (--trace-execution, dumps, REPL prompts) keeps
// using the process stdout. Only user-visible script output is
// redirected.
XRAY_API void xray_isolate_set_stdout(XrayIsolate *isolate, FILE *stream);

// Bound the next bytecode execution to `timeout_ms` of wall-clock time.
// 0 or negative disables the bound. After execution callers should
// inspect xray_isolate_timed_out() to distinguish a normal failure from
// a deadline-triggered abort. The VM checks the deadline cooperatively
// at backward branches, so a tight native call may exceed the bound by
// the duration of one native frame.
XRAY_API void xray_isolate_set_deadline_ms(XrayIsolate *isolate, int64_t timeout_ms);

// Return true if the most recent execution was aborted because it
// crossed the deadline set by xray_isolate_set_deadline_ms().
XRAY_API bool xray_isolate_timed_out(XrayIsolate *isolate);

/* ========== Module Allowlist ========== */

// Restrict `import` statements run in this isolate to the modules
// named in `allowed[0..count-1]`. The array itself must outlive the
// isolate (typically a `static const char *const` table); the isolate
// only stores the pointer. Pass count=0 to drop the allowlist and
// permit every module (the default).
//
// Native (C) and script (.xr) modules are both filtered. Attempting
// to import a forbidden module returns null to the caller, which the
// VM surfaces as a runtime error at the `import` site.
XRAY_API void xray_isolate_set_module_allowlist(XrayIsolate *isolate, const char *const *allowed,
                                                size_t count);

#endif  // XRAY_ISOLATE_H
