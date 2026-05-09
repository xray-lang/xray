/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xisolate_api.h - Lightweight Isolate access interface
 *
 * KEY CONCEPT:
 *   Provides accessor functions for Isolate subsystems.
 *   Modules can use this header instead of xray_isolate_internal.h
 *   when they only need pointer access, not full struct definition.
 *
 * WHY THIS DESIGN:
 *   - Breaks "star dependency" on xray_isolate_internal.h
 *   - Modules only include what they need
 *   - Forward declarations + accessors = minimal coupling
 */

#ifndef XISOLATE_API_H
#define XISOLATE_API_H

#include "../base/xforward_decl.h"
#include "../base/xdefs.h"
#include <stdint.h>
#include <stdbool.h>

/* ========== Subsystem Accessors ========== */

// Memory subsystem
XR_FUNC XrGC *xr_isolate_get_gc(XrayIsolate *X);
XR_FUNC struct XrSystemHeap *xr_isolate_get_sys_heap(XrayIsolate *X);
XR_FUNC struct XrCoroGC *xr_isolate_get_coro_gc(XrayIsolate *X);

// Type subsystem (XrTypePool removed - now using XrType directly)
XR_FUNC XrTypeRegistry *xr_isolate_get_type_registry(XrayIsolate *X);
XR_FUNC void xr_isolate_set_type_registry(XrayIsolate *X, XrTypeRegistry *registry);
XR_FUNC void *xr_isolate_get_symbol_table(XrayIsolate *isolate);

// Class subsystem
XR_FUNC struct XrayCoreClasses *xr_isolate_get_core_classes(XrayIsolate *X);
XR_FUNC XrClass *xr_isolate_get_native_type_class(XrayIsolate *X, uint8_t type_id);
XR_FUNC void xr_isolate_set_native_type_class(XrayIsolate *X, uint8_t type_id, XrClass *cls);

// Module subsystem
XR_FUNC XrModuleRegistry *xr_isolate_get_module_registry(XrayIsolate *X);
XR_FUNC void xr_isolate_set_module_registry(XrayIsolate *X, XrModuleRegistry *registry);
XR_FUNC XrModule *xr_isolate_get_current_module(XrayIsolate *X);
XR_FUNC void xr_isolate_set_current_module(XrayIsolate *X, XrModule *mod);

// Globals
XR_FUNC XrGlobalsTable *xr_isolate_get_globals(XrayIsolate *X);
XR_FUNC XrGlobalObject *xr_isolate_get_global_object(XrayIsolate *X);
XR_FUNC struct XrGlobalStringPool *xr_isolate_get_string_pool(XrayIsolate *X);

// Coroutine
XR_FUNC XrCoroutine *xr_isolate_get_main_coro(XrayIsolate *X);
XR_FUNC void xr_isolate_set_main_coro(XrayIsolate *X, XrCoroutine *coro);

// VM state
XR_FUNC XrVMState *xr_isolate_get_vm_state(XrayIsolate *X);
XR_FUNC XrVMContext *xr_isolate_get_vm_ctx(XrayIsolate *X);

// Storage mode
XR_FUNC uint8_t xr_isolate_get_storage_mode(XrayIsolate *X);
XR_FUNC void xr_isolate_set_storage_mode(XrayIsolate *X, uint8_t mode);

// Config
XR_FUNC void *xr_isolate_get_userdata(XrayIsolate *X);
XR_FUNC struct XrayConfig *xr_isolate_get_config(XrayIsolate *X);
XR_FUNC uint32_t xr_isolate_get_init_flags(XrayIsolate *X);
XR_FUNC const char *xr_isolate_get_script_file(XrayIsolate *X);

// Parser arena
XR_FUNC struct XrArena *xr_isolate_get_current_arena(XrayIsolate *X);
XR_FUNC void xr_isolate_set_current_arena(XrayIsolate *X, struct XrArena *arena);

// AST node ID counter (monotonic, unique per compilation unit)
XR_FUNC uint32_t xr_isolate_next_ast_node_id(XrayIsolate *X);
XR_FUNC void xr_isolate_reset_ast_node_ids(XrayIsolate *X);

// Compile-time string pool (deduplication during parsing)
XR_FUNC struct XrCompileStringPool *xr_isolate_get_string_pool_compile(XrayIsolate *X);
XR_FUNC void xr_isolate_set_string_pool_compile(XrayIsolate *X, struct XrCompileStringPool *pool);

// Type system (compiler)
XR_FUNC XrTypeInferContext *xr_isolate_get_type_infer_context(XrayIsolate *X);
XR_FUNC XrTypeTable *xr_isolate_get_type_table(XrayIsolate *X);
XR_FUNC struct XrTypePool *xr_isolate_get_analyzer_pool(XrayIsolate *X);

// Memory tracker
XR_FUNC struct XrMemoryTracker *xr_isolate_get_memory_tracker(XrayIsolate *X);

// Debug
XR_FUNC void *xr_isolate_get_debug_state(XrayIsolate *X);
XR_FUNC void xr_isolate_set_debug_state(XrayIsolate *X, void *state);
XR_FUNC void *xr_isolate_get_debug_hooks(XrayIsolate *X);
XR_FUNC void xr_isolate_set_debug_hooks(XrayIsolate *X, void *hooks);
XR_FUNC struct XrSourceCache *xr_isolate_get_source_cache(XrayIsolate *X);
XR_FUNC void xr_isolate_set_source_cache(XrayIsolate *X, struct XrSourceCache *cache);

// REPL
XR_FUNC struct XrReplSymbolTable *xr_isolate_get_repl_symbols(XrayIsolate *X);
XR_FUNC void xr_isolate_set_repl_symbols(XrayIsolate *X, struct XrReplSymbolTable *syms);

// Exception print suppression
XR_FUNC bool xr_isolate_get_suppress_exception_print(XrayIsolate *X);
XR_FUNC void xr_isolate_set_suppress_exception_print(XrayIsolate *X, bool suppress);

/* ========== Compilation & Execution ========== */

struct XrProto;
struct AstNode;
XR_FUNC XrProto *xr_compile_ast_with_source(XrayIsolate *isolate, struct AstNode *ast,
                                            const char *source_file);
XR_FUNC XrProto *xr_compile_source_with_path(XrayIsolate *isolate, const char *source,
                                             const char *source_file);
XR_FUNC int xr_execute(XrayIsolate *isolate, struct XrProto *code);
XR_FUNC void xr_free_code(XrayIsolate *isolate, struct XrProto *proto);

/* ========== Error Reporting ========== */

XR_FUNC void xr_runtime_error(XrayIsolate *isolate, const char *fmt, ...);

/* ========== Extension Type System (for dlopen packages) ========== */

struct XrGCHeader;  // forward declaration (full definition in xgc_header.h)

// Callback typedefs (distinct from GC-internal types to avoid conflicts)
typedef void (*XrExtDestroyFn)(struct XrGCHeader *obj, void *gc);
typedef void (*XrExtTraverseFn)(void *gc, struct XrGCHeader *obj);

// Allocate a dynamic GC type ID for an extension type.
// Returns 0 on failure (all slots exhausted).
XR_FUNC uint8_t xr_alloc_extension_type(XrayIsolate *isolate, const char *name);

// Register destroy callback (also sets ext_finalize_bitmap).
XR_FUNC void xr_register_extension_destroy(XrayIsolate *isolate, uint8_t type_id,
                                           XrExtDestroyFn destroy_fn);

// Register traverse callback (also sets ext_has_refs_bitmap).
XR_FUNC void xr_register_extension_traverse(XrayIsolate *isolate, uint8_t type_id,
                                            XrExtTraverseFn traverse_fn);

// Accessors for GC code
XR_FUNC uint64_t xr_isolate_get_ext_finalize_bitmap(XrayIsolate *isolate);
XR_FUNC uint64_t xr_isolate_get_ext_has_refs_bitmap(XrayIsolate *isolate);
XR_FUNC XrExtDestroyFn xr_isolate_get_ext_destroy(XrayIsolate *isolate, uint8_t type_id);
XR_FUNC XrExtTraverseFn xr_isolate_get_ext_traverse(XrayIsolate *isolate, uint8_t type_id);

/* ========== Thread Local API ========== */

XR_FUNC XrayIsolate *xray_isolate_current(void);

// Enter/exit isolate context for current thread
XR_FUNC void xray_isolate_enter(XrayIsolate *isolate);
XR_FUNC void xray_isolate_exit(void);

#endif  // XISOLATE_API_H
