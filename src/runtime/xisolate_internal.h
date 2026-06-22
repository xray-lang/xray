/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xisolate_internal.h - Internal structure definition for XrVMRuntime
 *
 * KEY CONCEPT:
 *   XrVMRuntime is the complete execution environment.
 *   It contains all runtime state: GC, type system, VM state, globals, etc.
 *   This header exposes internal structure for backend implementations only.
 *
 * WHY THIS DESIGN:
 *   - Single-Isolate model: All state in one place, no Context abstraction
 *   - Direct access: Backend code accesses fields directly for performance
 *   - Per-coroutine heap: Independent heaps with bulk deallocation
 *
 * RELATED MODULES:
 *   - xray_vm.h: Public API for VM lifecycle
 *   - xr_vm_state.h: VM execution state (stack, frames, globals)
 *   - xcoro_memory.h: Per-coroutine memory management
 */

#ifndef XISOLATE_INTERNAL_H
#define XISOLATE_INTERNAL_H

#include "xray_vm.h"
#include "../base/xforward_decl.h"  // Forward declarations
#include "value/xvalue.h"
#include "class/xclass.h"
#include "mem/xheap.h"  // heap allocation definitions
// Instance/Json pools removed, using per-coroutine heap
#include "../runtime/xexec_frame.h"  // VM state types (XrBcCallFrame, etc.)
#include "../runtime/xexec_state.h"  // XrVMState - VM execution state
#include "object/xnative_type.h"     // XR_NATIVE_TYPE_MAX
#include "core/xr_runtime_core.h"

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
XR_FUNC int xr_vm_init(XrVMRuntime *isolate);
XR_FUNC void xr_vm_cleanup(XrVMRuntime *isolate);

/* ========== Fast Macros ========== */

// Simplified design: Use Isolate directly, no ThreadLocalTop
// Isolate contains all execution state

/* ========== XrVMRuntime Internal Structure ========== */

// XrVMRuntime - Complete execution environment
//
// Simplified design: single Isolate model
// - Contains all runtime state
// - Independent heap, GC, global objects
// - No Context abstraction layer needed
struct XrVMRuntime {
    /* ========== Common State ========== */

    // Core object system
    XrayCoreClasses *core;  // Core classes (Object, Class, String, etc.)

    // VM-neutral runtime core: GC, system heap, string pool, config,
    // script metadata, weak registry, and extension type registry.
    XrRuntimeCore *core_rt;

    // Main coroutine (RC heap architecture)
    // - All coroutines (including main) use XrCoroHeap + XrCoroHeap
    // - Main coroutine: large heap (4MB), deferred cycle collection
    // - O(1) heap release on program exit
    struct XrCoroutine *main_coro;  // Main coroutine (owns large RC heap)
    struct XrTask *deferred_tasks;  // Runtime-owned Task shells awaiting isolate teardown
    size_t deferred_task_count;

    // Global state
    XrGlobalsTable *globals;  // Dynamic global variables table

    // Type system
    struct XrCompilerSession *compiler_session;  // Active toolchain session bridge.

    // Configuration
    XrVMConfig params;  // Creation parameters
    void (*lifecycle_cleanup)(struct XrVMRuntime *isolate);

    // Global object (simplified: embedded directly in Isolate)
    XrGlobalObject *global_object;  // Global object

    // Module system
    XrModuleRegistry *module_registry;  // Module registry
    XrModule *current_module;           // Currently loading module (for export collection)

    // Storage mode context (for class instance shared).
    // Relaxed atomic: each worker sets it immediately before the
    // instantiation that consumes-and-clears it, but multiple workers touch
    // the same isolate-level slot, so the accesses must be tear-free.
    _Atomic uint8_t current_storage_mode;  // 0=normal, 1=shared

    // Test mode: suppress [Uncaught Exception] stderr output
    bool suppress_exception_print;

    /* ========== Embedded execution policy (opt-in) ==========
     *
     * Host applications that embed the Xray VM (MCP runner, CLI eval,
     * future REPL sandboxes) need to redirect user output away from
     * the process stdout and bound execution time. These slots are
     * NULL/zero by default and only consulted by the few opcodes
     * that emit user-visible side effects.
     *
     * `user_stdout` — alternative FILE* for `print()` / `OP_PRINT`
     *   output. NULL = use the process `stdout`. Set via
     *   xray_vm_set_stdout(). Must outlive the isolate.
     *
     * `deadline_ns` — wall-clock deadline (CLOCK_MONOTONIC, ns). When
     *   non-zero, the VM checks at backward branches whether we have
     *   passed the deadline; if so it sets `deadline_exceeded` and
     *   aborts the running coroutine with a runtime error so the
     *   host can recover. Set via xray_vm_set_deadline_ms().
     *
     * `deadline_exceeded` — sticky flag observed after a timed
     *   execution returns; cleared each time a new deadline is set.
     */
    void *user_stdout; /* FILE*, see xr_isolate_stdout() */
    int64_t deadline_ns;
    bool deadline_exceeded;

    /* Module allowlist. When `allowlist_count > 0` the module loader
     * rejects any import whose name is not in `allowlist[0..count-1]`.
     * The host owns the array; the isolate only stores the pointer. */
    const char *const *module_allowlist;
    size_t module_allowlist_count;

    /* WeakMap / WeakSet registry. Lazily allocated by the first weak insert
     * and swept when a weakable target reaches RC zero. Kept opaque here so
     * the isolate core does not depend on container internals. */
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

    /* ========== stdlib per-isolate cache ========== */
    // Opaque pointer owned by stdlib/stdlib_cache.h. Holds memoised
    // values that reference per-isolate symbol IDs (e.g. the dynamic-
    // layout XrClass built once by io.stat() and the interned error-map
    // keys shared by json/yaml/toml/xml/csv parsers). Kept as `void *`
    // here so stdlib types don't leak into the core header; cast via
    // the accessor functions in `stdlib/stdlib_cache.h`.
    void *stdlib_cache;  // XrStdlibCache* (stdlib/stdlib_cache.h), lazily allocated

    /* ========== Prelude type marker registry ==========
     * Pointer to the process-wide constant XrPreludeSymbols table built
     * by stdlib/prelude/prelude.c. Populated during isolate init by
     * xr_load_module_prelude(). Read by the parser when resolving a
     * type-context identifier that is neither a primitive keyword nor a
     * user-defined class name. Kept as `void *` so the core header has
     * no dependency on stdlib types; consumers cast via
     * xr_prelude_get_symbols(). NULL means the prelude has not been
     * loaded (minimal-runtime isolates that skipped setup_full). */
    void *prelude_symbols;  // const XrPreludeSymbols* (stdlib/prelude/prelude.h)

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
// ========== Compilation and Execution API ==========

XR_FUNC XrProto *xr_compile_ast_with_source(XrCompilerSession *session, AstNode *ast,
                                            const char *source_file);
XR_FUNC XrProto *xr_compile_source_with_path(XrCompilerSession *session, const char *source,
                                             const char *source_file);

// xr_execute and xr_free_code are declared in xisolate_api.h.

/* ========== Thread Local Storage API ========== */

// Global thread-local Isolate pointer
// Each thread has its own Isolate instance
extern XR_THREAD_LOCAL XrVMRuntime *g_current_isolate;

// xray_vm_enter/xray_vm_exit declared in xisolate_api.h

#endif  // XISOLATE_INTERNAL_H
