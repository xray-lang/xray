/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xisolate_api.h - Lightweight VM runtime access interface
 *
 * KEY CONCEPT:
 *   Provides accessor functions for VM runtime subsystems.
 *   Modules can use this header instead of xisolate_internal.h
 *   when they only need pointer access, not full struct definition.
 *
 * WHY THIS DESIGN:
 *   - Breaks "star dependency" on xisolate_internal.h
 *   - Modules only include what they need
 *   - Forward declarations + accessors = minimal coupling
 */

#ifndef XISOLATE_API_H
#define XISOLATE_API_H

#include "../base/xforward_decl.h"
#include "../base/xdefs.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* ========== Subsystem Accessors ========== */

typedef struct XrRuntimeCore XrRuntimeCore;

// Runtime core
XR_FUNC XrRuntimeCore *xr_isolate_get_runtime_core(XrVMRuntime *X);

// Scheduler runtime
XR_FUNC XrRuntime *xr_isolate_get_scheduler_runtime(XrVMRuntime *X);

// Memory subsystem
XR_FUNC XrFixedHeap *xr_isolate_get_fixed_heap(XrVMRuntime *X);
XR_FUNC struct XrSystemHeap *xr_isolate_get_sys_heap(XrVMRuntime *X);
XR_FUNC struct XrCoroHeap *xr_isolate_get_heap(XrVMRuntime *X);

// Type subsystem (XrTypePool removed - now using XrType directly)
XR_FUNC XrTypeRegistry *xr_isolate_get_type_registry(XrVMRuntime *X);
XR_FUNC void xr_isolate_set_type_registry(XrVMRuntime *X, XrTypeRegistry *registry);
XR_FUNC void *xr_isolate_get_symbol_table(XrVMRuntime *isolate);

// Class subsystem
XR_FUNC struct XrayCoreClasses *xr_isolate_get_core_classes(XrVMRuntime *X);
XR_FUNC XrClass *xr_isolate_get_native_type_class(XrVMRuntime *X, uint8_t type_id);
XR_FUNC void xr_isolate_set_native_type_class(XrVMRuntime *X, uint8_t type_id, XrClass *cls);

// Module subsystem
XR_FUNC XrModuleRegistry *xr_isolate_get_module_registry(XrVMRuntime *X);
XR_FUNC void xr_isolate_set_module_registry(XrVMRuntime *X, XrModuleRegistry *registry);
XR_FUNC XrModule *xr_isolate_get_current_module(XrVMRuntime *X);
XR_FUNC void xr_isolate_set_current_module(XrVMRuntime *X, XrModule *mod);

// Globals
XR_FUNC XrGlobalsTable *xr_isolate_get_globals(XrVMRuntime *X);
XR_FUNC XrGlobalObject *xr_isolate_get_global_object(XrVMRuntime *X);
XR_FUNC struct XrGlobalStringPool *xr_isolate_get_string_pool(XrVMRuntime *X);
XR_FUNC struct XrStrBuf **xr_isolate_tmp_strbuf_slot(XrVMRuntime *X);

// Coroutine
XR_FUNC XrCoroutine *xr_isolate_get_main_coro(XrVMRuntime *X);
XR_FUNC void xr_isolate_set_main_coro(XrVMRuntime *X, XrCoroutine *coro);

// VM state
XR_FUNC XrVMState *xr_isolate_get_vm_state(XrVMRuntime *X);
XR_FUNC XrVMContext *xr_isolate_get_vm_ctx(XrVMRuntime *X);

// Storage mode
XR_FUNC uint8_t xr_isolate_get_storage_mode(XrVMRuntime *X);
XR_FUNC void xr_isolate_set_storage_mode(XrVMRuntime *X, uint8_t mode);

// Config
XR_FUNC void *xr_isolate_get_userdata(XrVMRuntime *X);
XR_FUNC struct XrayConfig *xr_isolate_get_config(XrVMRuntime *X);
XR_FUNC const char *xr_isolate_get_script_file(XrVMRuntime *X);
XR_FUNC int xr_isolate_get_script_argc(XrVMRuntime *X);
XR_FUNC char **xr_isolate_get_script_argv(XrVMRuntime *X);

// Debug
XR_FUNC void *xr_isolate_get_debug_state(XrVMRuntime *X);
XR_FUNC void xr_isolate_set_debug_state(XrVMRuntime *X, void *state);
XR_FUNC void *xr_isolate_get_debug_hooks(XrVMRuntime *X);
XR_FUNC void xr_isolate_set_debug_hooks(XrVMRuntime *X, void *hooks);

// Exception print suppression
XR_FUNC bool xr_isolate_get_suppress_exception_print(XrVMRuntime *X);
XR_FUNC void xr_isolate_set_suppress_exception_print(XrVMRuntime *X, bool suppress);

/* Resolve the effective FILE* for user-visible script output.
 * Returns the host-supplied stream (if any) or the process stdout.
 * Tolerates NULL isolate (returns stdout) so OP_PRINT code paths do not
 * need an extra branch. */
XR_FUNC FILE *xr_isolate_stdout(XrVMRuntime *X);

/* Wall-clock deadline check. Returns true and sets `X->deadline_exceeded`
 * when the configured deadline has passed; returns false (no syscalls)
 * when no deadline is active. The VM calls this at backward branches. */
XR_FUNC bool xr_isolate_check_deadline(XrVMRuntime *X);

/* True if the most recent execution aborted because the wall-clock
 * deadline was exceeded. Cleared by xray_vm_set_deadline_ms(). */
XR_FUNC bool xr_isolate_timed_out(XrVMRuntime *X);

/* Decide whether `module_name` may be imported by `X`. When no
 * allowlist is configured every module is allowed. */
XR_FUNC bool xr_isolate_module_allowed(XrVMRuntime *X, const char *module_name);

/* ========== Compilation & Execution ========== */

struct XrProto;
struct AstNode;
struct XrCompilerSession;
XR_FUNC XrProto *xr_compile_ast_with_source(struct XrCompilerSession *session, struct AstNode *ast,
                                            const char *source_file);
XR_FUNC XrProto *xr_compile_source_with_path(struct XrCompilerSession *session, const char *source,
                                             const char *source_file);
XR_FUNC int xr_execute(XrVMRuntime *isolate, struct XrProto *code);
XR_FUNC void xr_free_code(XrVMRuntime *isolate, struct XrProto *proto);

/* ========== Error Reporting ========== */

XR_FUNC void xr_runtime_error(XrVMRuntime *isolate, const char *fmt, ...);

/* ========== Extension Type System (for dlopen packages) ========== */

struct XrObjHeader;  // forward declaration (full definition in xobj_header.h)

// Callback typedefs (distinct from GC-internal types to avoid conflicts)
typedef void (*XrExtDestroyFn)(struct XrObjHeader *obj, void *gc);
typedef void (*XrExtTraverseFn)(void *gc, struct XrObjHeader *obj);

// Allocate a dynamic GC type ID for an extension type.
// Returns 0 on failure (all slots exhausted).
XR_FUNC uint8_t xr_alloc_extension_type(XrVMRuntime *isolate, const char *name);

// Register destroy callback (also sets ext_finalize_bitmap).
XR_FUNC void xr_register_extension_destroy(XrVMRuntime *isolate, uint8_t type_id,
                                           XrExtDestroyFn destroy_fn);

// Register traverse callback (also sets ext_has_refs_bitmap).
XR_FUNC void xr_register_extension_traverse(XrVMRuntime *isolate, uint8_t type_id,
                                            XrExtTraverseFn traverse_fn);

// Accessors for GC code
XR_FUNC uint64_t xr_isolate_get_ext_finalize_bitmap(XrVMRuntime *isolate);
XR_FUNC uint64_t xr_isolate_get_ext_has_refs_bitmap(XrVMRuntime *isolate);
XR_FUNC XrExtDestroyFn xr_isolate_get_ext_destroy(XrVMRuntime *isolate, uint8_t type_id);
XR_FUNC XrExtTraverseFn xr_isolate_get_ext_traverse(XrVMRuntime *isolate, uint8_t type_id);

/* ========== Thread Local API ========== */

XR_FUNC XrVMRuntime *xray_vm_current(void);

// Enter/exit isolate context for current thread
XR_FUNC void xray_vm_enter(XrVMRuntime *isolate);
XR_FUNC void xray_vm_exit(void);

#endif  // XISOLATE_API_H
