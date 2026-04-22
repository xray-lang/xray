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
 *   An Isolate is a complete, isolated instance of the Xray VM.
 *   Each Isolate has its own heap, GC, stack, globals, and type registry.
 *   One Isolate per thread is the typical usage pattern.
 */

#ifndef XRAY_ISOLATE_H
#define XRAY_ISOLATE_H

#include <stddef.h>
#include <stdbool.h>
#include "xray_export.h"

// An Isolate is a complete instance of the Xray VM with its own
// heap, GC, stack, globals, type registry, and execution backend.
typedef struct XrayIsolate XrayIsolate;

typedef enum {
    XRAY_BACKEND_BYTECODE,    // Bytecode interpreter (default, fast startup)
    XRAY_BACKEND_LLVM_JIT,    // LLVM JIT compiler (future)
    XRAY_BACKEND_MIXED        // Mixed mode: Bytecode + JIT for hot code (future)
} XrayBackendType;

/* ========== Init Flags ========== */

// Subsystem init flags for XrayIsolateParams.init_flags.
// Combine with bitwise OR. 0 means XR_INIT_FULL (all subsystems).
#define XR_INIT_VM           (1 << 0)
#define XR_INIT_GC           (1 << 1)
#define XR_INIT_COMPILER     (1 << 2)
#define XR_INIT_MODULES      (1 << 3)
#define XR_INIT_STDLIB       (1 << 4)
#define XR_INIT_REFLECTION   (1 << 5)
#define XR_INIT_CLASSES      (1 << 6)
#define XR_INIT_REGEX        (1 << 7)
#define XR_INIT_SYMBOLS      (1 << 8)
#define XR_INIT_CONFIG       (1 << 9)
#define XR_INIT_ANALYZER     (1 << 10)
#define XR_INIT_SOURCE_CACHE (1 << 11)

#define XR_INIT_RUNTIME      (XR_INIT_VM | XR_INIT_GC)
#define XR_INIT_FULL         (0xFFFF)

typedef struct {
    /* === Init Flags === */
    unsigned int init_flags;        // 0 = XR_INIT_FULL (all subsystems)

    /* === Backend === */
    XrayBackendType backend_type;

    /* === JIT (for LLVM/Mixed backends) === */
    bool enable_jit;
    int jit_threshold;              // Call count before JIT (default: 100)

    /* === Memory === */
    size_t initial_heap_size;       // 0 = use default
    size_t max_heap_size;           // 0 = unlimited

    /* === GC === */
    bool enable_gc;                 // Default: true
    size_t gc_threshold;

    /* === Debug === */
    bool trace_execution;
    bool trace_gc;
    bool dump_bytecode;
    bool dump_ic_feedback;          // Dump IC type feedback after execution

    /* === User Data === */
    void *userdata;

    /* === Script Info (set at runtime) === */
    const char *script_file;        // Script path (for __file__)
    int script_argc;
    char **script_argv;

    /* === Extension callbacks (set by xray_isolate_setup_full) === */
    int  (*init_extra)(struct XrayIsolate *isolate);     // Heavy subsystem init
    void (*cleanup_extra)(struct XrayIsolate *isolate);  // Heavy subsystem cleanup
} XrayIsolateParams;

/* ========== Core API ========== */

// Create a new Isolate. Pass NULL for default params.
// Returns NULL on failure.
XRAY_API XrayIsolate* xray_isolate_new(const XrayIsolateParams *params);

// Initialize params with defaults: Bytecode backend, GC enabled, JIT disabled
XRAY_API void xray_isolate_params_init(XrayIsolateParams *params);

// Setup full runtime (compiler, classes, modules, reflection, regex, etc.)
// Call after params_init, before xray_isolate_new.
// Bytecode-bundled executables skip this for minimal binary size.
XRAY_API void xray_isolate_setup_full(XrayIsolateParams *params);

// Destroy Isolate and free all resources. Safe to pass NULL.
XRAY_API void xray_isolate_delete(XrayIsolate *isolate);

// Execute source code. Returns 0 on success, -1 on error.
XRAY_API int xray_isolate_dostring(XrayIsolate *isolate, const char *source);

// Execute source file. Returns 0 on success, -1 on error.
XRAY_API int xray_isolate_dofile(XrayIsolate *isolate, const char *filename);

// Execute source file with debug support (DAP).
// Returns 0 on success, -1 on error. out_proto receives compiled proto for debugging.
XRAY_API int xray_isolate_dofile_debug(XrayIsolate *isolate, const char *filename, void **out_proto);

/* ========== Advanced API ========== */

// Get current backend type (determined at compile time via CMake)
XRAY_API XrayBackendType xray_isolate_get_backend(XrayIsolate *isolate);

XRAY_API void xray_isolate_set_userdata(XrayIsolate *isolate, void *userdata);
XRAY_API void* xray_isolate_get_userdata(XrayIsolate *isolate);

/* ========== Statistics and Debugging ========== */

XRAY_API void xray_isolate_get_stats(XrayIsolate *isolate,
                                     size_t *bytes_allocated,
                                     int *gc_count);

XRAY_API void xray_isolate_collect_garbage(XrayIsolate *isolate);

XRAY_API void xray_isolate_set_trace(XrayIsolate *isolate, bool enable);

XRAY_API void xray_isolate_set_dump_bytecode(XrayIsolate *isolate, bool enable);

// Set script info. Accessible in script as: process.args, __file__, __dir__
XRAY_API void xray_isolate_set_script_info(XrayIsolate *isolate,
                                           const char *script_file,
                                           int argc,
                                           char **argv);

#endif // XRAY_ISOLATE_H
