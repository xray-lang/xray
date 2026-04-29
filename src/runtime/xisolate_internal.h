/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xisolate_internal.h - Internal structure definition for XrayIsolate
 *
 * KEY CONCEPT:
 *   XrayIsolate is the complete execution environment.
 *   It contains all runtime state: GC, type system, VM state, globals, etc.
 *   This header exposes internal structure for backend implementations only.
 *
 * WHY THIS DESIGN:
 *   - Single-Isolate model: All state in one place, no Context abstraction
 *   - Direct access: Backend code accesses fields directly for performance
 *   - Per-coroutine GC: Independent heaps with bulk deallocation
 *
 * RELATED MODULES:
 *   - xray_isolate.h: Public API for Isolate lifecycle
 *   - xr_vm_state.h: VM execution state (stack, frames, globals)
 *   - xcoro_memory.h: Per-coroutine memory management
 */

#ifndef XISOLATE_INTERNAL_H
#define XISOLATE_INTERNAL_H

#include "xray_isolate.h"
#include "../base/xforward_decl.h"  // Forward declarations
#include "value/xvalue.h"
#include "class/xclass.h"
#include "gc/xgc.h"  // GC core definitions
// Instance/Json pools removed, using per-coroutine heap
#include "../runtime/xexec_frame.h"  // VM state types (XrBcCallFrame, etc.)
#include "../runtime/xexec_state.h"  // XrVMState - VM execution state
#include "object/xnative_type.h"     // XR_NATIVE_TYPE_MAX

// Thread-local storage class is provided by base/xdefs.h as
// XR_THREAD_LOCAL. Use it directly here; do not redefine the macro.

// xray_runtime_interface.h removed - single backend, no abstraction layer
// VM type definitions in core/xforward_decl.h
#include "../base/xconfig.h"  // XrayConfig configuration

/* ========== Forward Declarations ========== */

typedef struct XrayCoreClasses XrayCoreClasses;
typedef struct XrGlobalStringPool XrGlobalStringPool;
// Other forward declarations moved to xr_forward_decl.h

/* ========== VM Engine API (compile-time static linking) ========== */

// VM initialization and cleanup
XR_FUNC int xr_vm_init(XrayIsolate *isolate);
XR_FUNC void xr_vm_cleanup(XrayIsolate *isolate);

/* ========== Fast Macros ========== */

// Simplified design: Use Isolate directly, no ThreadLocalTop
// Isolate contains all execution state

/* ========== XrayIsolate Internal Structure ========== */

// XrayIsolate - Complete execution environment
//
// Simplified design: single Isolate model
// - Contains all runtime state
// - Independent heap, GC, global objects
// - No Context abstraction layer needed
struct XrayIsolate {
    /* ========== Common State ========== */

    // Core object system
    XrayCoreClasses *core;          // Core classes (Object, Class, String, etc.)
    XrTypeRegistry *type_registry;  // Type registry for reflection

    // Memory management - Per-Coroutine GC
    // - Runtime objects allocated on coroutine-private heap
    // - System objects (coroutines, classes, modules) on system heap
    // - Entire heap freed when coroutine exits
    XrGC gc;                          // GC instance (manages fixedgc list only)
    struct XrSystemHeap *sys_heap;    // System heap (coroutine pool, class Arena)
    XrMemoryTracker *memory_tracker;  // Memory allocation tracker

    // Main coroutine (unified GC architecture)
    // - All coroutines (including main) use XrCoroGC + XrCoroHeap
    // - Main coroutine: large heap (4MB), deferred GC (max_gen_gcs=100)
    // - O(1) heap release on program exit
    struct XrCoroutine *main_coro;  // Main coroutine (owns large heap GC)

    // Global state
    XrGlobalsTable *globals;                 // Dynamic global variables table
    XrGlobalStringPool *global_string_pool;  // Global string pool (read-only)

    // Type system
    XrTypeInferContext *type_infer_context;  // Type inference context
    XrTypeTable *type_table;                 // Compiler type table
    struct XrTypePool *analyzer_pool;        // Static analyzer type pool (multi-instance safe)

    // Symbol system
    XrSymbolTable *symbol_table;  // Per-Isolate symbol table

    // Configuration
    XrayIsolateParams params;  // Creation parameters
    XrayConfig *config;        // Global configuration
    void *userdata;            // User data pointer
    uint32_t init_flags;       // Which subsystems were initialized (XR_INIT_*)

    // Global object (simplified: embedded directly in Isolate)
    XrGlobalObject *global_object;  // Global object

    // Module system
    XrModuleRegistry *module_registry;  // Module registry
    XrModule *current_module;           // Currently loading module (for export collection)

    // Storage mode context (for class instance shared)
    uint8_t current_storage_mode;  // 0=normal, 1=shared

    // Test mode: suppress [Uncaught Exception] stderr output
    bool suppress_exception_print;

    // Current arena for AST allocation (set by parser, NULL = use malloc)
    struct XrArena *current_arena;

    // Native type mapping table
    XrClass *native_type_classes[XR_NATIVE_TYPE_MAX];  // GC type ID -> XrClass mapping

    // Per-isolate shape registry (hidden classes)
    struct XrShape **shape_entries;
    uint16_t shape_count;
    uint16_t shape_capacity;

    // Per-isolate root shape cache (for json creation)
    struct XrShape *root_shape_cache[32];

    // Per-isolate active type pool (replaces XR_THREAD_LOCAL g_current_pool)
    struct XrTypePool *current_type_pool;

    /* ========== VM Engine State ========== */

    // VM state uses independent type XrVMState (defined in xr_vm_state.h)
    // Embedded directly in Isolate for zero-overhead access
    XrVMState vm;

    /* ========== Unified VM Context (multi-core support) ========== */
    // vm_ctx provides unified execution context access interface
    //
    // Single-thread mode: vm_ctx points to fields embedded in vm
    // Multi-thread mode: Worker has independent vm_ctx
    //
    // run() accesses execution state via vm_ctx for single/multi-thread unification
    XrVMContext vm_ctx;

    /* ========== Debug Info (placed after VM to avoid stack pollution) ========== */

    // Source code cache (for error display)
    struct XrSourceCache *source_cache;

    /* ========== REPL State ========== */
    struct XrReplSymbolTable *repl_symbols;  // persistent symbol table for REPL incremental mode

    /* ========== Debug State (DAP integration) ========== */
    void *debug_state;  // XrDebugState* for debugger integration
    void *debug_hooks;  // XrDebugHooks* for VM callback interface

    /* ========== Cluster (optional, enabled with XR_HAS_CLUSTER) ========== */
    void *cluster;  // XrCluster* (stdlib/cluster), NULL if not started

    /*
     * Distributed channel hook vtable. Populated by
     * xr_cluster_channel_install_hooks when the cluster module starts;
     * reset to NULL by xr_cluster_channel_uninstall_hooks. Kept as
     * `void *` so that the core header avoids a dependency on the
     * coroutine-subsystem struct XrChannelDistHooks — callers cast via
     * the cluster/coro accessor sites that already pull in
     * xchannel.h. NULL means "no cluster attached, route through
     * in-process channels only".
     */
    void *channel_dist_hooks;  // XrChannelDistHooks*

    /* ========== Extension Type System (dlopen packages) ========== */
    // Dynamic type allocation: next type ID to assign (starts at XR_TTASK + 1)
    uint8_t ext_type_next;
    // Per-type name strings (only extension slots used)
    const char *ext_type_names[XGC_MAX_TYPES];
    // Runtime bitmaps (OR'd with compile-time constants in GC hot path)
    uint64_t ext_finalize_bitmap;  // types needing finalization
    uint64_t ext_has_refs_bitmap;  // types needing GC traversal
    // Per-type callbacks (only extension slots used)
    XrGCDestroyFn ext_destroy_funcs[XGC_MAX_TYPES];
    void *ext_traverse_funcs[XGC_MAX_TYPES];  // XrGCTraverseFn equivalent

    /* ========== stdlib per-isolate cache ========== */
    // Opaque pointer owned by stdlib/stdlib_cache.h. Holds memoised
    // values that reference per-isolate symbol IDs (e.g. the XrShape
    // built once by io.stat() and the interned error-map keys shared by
    // json/yaml/toml/xml/csv parsers). Kept as `void *` here so stdlib
    // types don't leak into the core header; cast via the accessor
    // functions in `stdlib/stdlib_cache.h`.
    void *stdlib_cache;  // XrStdlibCache* (stdlib/stdlib_cache.h), lazily allocated

    /* ========== VM profiler (opt-in, isolate-local) ==========
     * Bytecode execution counters collected during this isolate's
     * lifetime. NULL unless the build was configured with
     * -DXR_ENABLE_VM_PROFILER=ON; in that case the field is
     * lazily allocated by the VM dispatch entry. Per-isolate so
     * concurrent isolates (LSP / DAP / embedded scripting hosts)
     * never share counters. Cast through `struct VMProfiler *` to
     * avoid pulling the profiler internals into this header. */
    void *profiler; /* VMProfiler*, see vm/xvm_profiler.h */
};

/* ========== VM State Access ========== */

// Concise design: Access isolate->vm directly
//
// Isolate contains execution state directly.
// All code uses:
//   isolate->vm.stack
//   isolate->vm.stack_top
//   isolate->vm.frame_count
//   ...
//
// JIT fields (if enabled) are in the same struct:
//   #ifdef XRAY_HAS_JIT
//   isolate->vm.jit              // XirJitState* (compile queue, code cache)
//   isolate->vm.jit_threshold    // Tier 1 trigger threshold
//   #endif // ========== Compilation and Execution API ==========

// Compile AST to bytecode (no source file info)
XR_FUNC XrProto *xr_compile_ast(XrayIsolate *isolate, AstNode *ast);

// xr_compile_ast_with_source, xr_execute, xr_free_code, xr_compile_source_with_path
// declared in xisolate_api.h

#ifdef XRAY_HAS_JIT
// JIT compile hot function (VM+JIT config only)
// Returns compiled native function pointer, or NULL on failure
// Implemented in: src/backends/jit/xr_jit_compiler.c
typedef XrValue (*XrNativeFunc)(XrayIsolate *, XrValue *, int);
XR_FUNC XrNativeFunc xr_jit_compile(XrayIsolate *isolate, XrProto *proto);

// Check if function should be JIT compiled
// Returns true if should compile, false to continue interpreting
XR_FUNC bool xr_jit_should_compile(XrayIsolate *isolate, XrProto *proto);
#endif  // ========== Internal Helper Functions ==========

// Initialize common state (called by xray_isolate_new)
// Returns 0 on success, -1 on failure
XR_FUNC int xray_isolate_init_common(XrayIsolate *isolate);

// Cleanup common state (called by xray_isolate_delete)
XR_FUNC void xray_isolate_cleanup_common(XrayIsolate *isolate);

/* ========== Thread Local Storage API ========== */

// Global thread-local Isolate pointer
// Each thread has its own Isolate instance
extern XR_THREAD_LOCAL XrayIsolate *g_current_isolate;

// Get current thread's Isolate (fast inline version)
// Returns NULL if not set
// Performance: ~2ns (direct TLS access)
static inline XrayIsolate *xray_isolate_get_current(void) {
    return g_current_isolate;
}

// xray_isolate_enter/xray_isolate_exit declared in xisolate_api.h

#endif  // XISOLATE_INTERNAL_H
