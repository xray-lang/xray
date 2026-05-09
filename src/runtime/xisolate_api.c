/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xisolate_api.c - Lightweight Isolate access interface implementation
 */

#include "xisolate_api.h"
#include "xisolate_internal.h"
#include "../coro/xcoroutine.h"
#include "../base/xlog.h"

/* ========== Memory Subsystem ========== */

XrGC *xr_isolate_get_gc(XrayIsolate *X) {
    return X ? &X->gc : NULL;
}

struct XrSystemHeap *xr_isolate_get_sys_heap(XrayIsolate *X) {
    return X ? X->sys_heap : NULL;
}

struct XrCoroGC *xr_isolate_get_coro_gc(XrayIsolate *X) {
    if (!X || !X->main_coro)
        return NULL;
    return ((XrCoroutine *) X->main_coro)->coro_gc;
}

/* ========== Type Subsystem ========== */

// xr_isolate_get_type_pool removed - now using XrType directly

XrTypeRegistry *xr_isolate_get_type_registry(XrayIsolate *X) {
    return X ? X->type_registry : NULL;
}

void xr_isolate_set_type_registry(XrayIsolate *X, XrTypeRegistry *registry) {
    if (X)
        X->type_registry = registry;
}

// xr_isolate_get_symbol_table defined in api/xisolate.c

/* ========== Class Subsystem ========== */

struct XrayCoreClasses *xr_isolate_get_core_classes(XrayIsolate *X) {
    return X ? X->core : NULL;
}

XrClass *xr_isolate_get_native_type_class(XrayIsolate *X, uint8_t type_id) {
    if (!X || type_id >= XR_NATIVE_TYPE_MAX)
        return NULL;
    return X->native_type_classes[type_id];
}

void xr_isolate_set_native_type_class(XrayIsolate *X, uint8_t type_id, XrClass *cls) {
    if (X && type_id < XR_NATIVE_TYPE_MAX) {
        X->native_type_classes[type_id] = cls;
    }
}

/* ========== Module Subsystem ========== */

XrModuleRegistry *xr_isolate_get_module_registry(XrayIsolate *X) {
    return X ? X->module_registry : NULL;
}

void xr_isolate_set_module_registry(XrayIsolate *X, XrModuleRegistry *registry) {
    if (X)
        X->module_registry = registry;
}

XrModule *xr_isolate_get_current_module(XrayIsolate *X) {
    return X ? X->current_module : NULL;
}

void xr_isolate_set_current_module(XrayIsolate *X, XrModule *mod) {
    if (X) {
        X->current_module = mod;
    }
}

/* ========== Globals ========== */

XrGlobalsTable *xr_isolate_get_globals(XrayIsolate *X) {
    return X ? X->globals : NULL;
}

XrGlobalObject *xr_isolate_get_global_object(XrayIsolate *X) {
    return X ? X->global_object : NULL;
}

struct XrGlobalStringPool *xr_isolate_get_string_pool(XrayIsolate *X) {
    return X ? X->global_string_pool : NULL;
}

/* ========== Coroutine ========== */

XrCoroutine *xr_isolate_get_main_coro(XrayIsolate *X) {
    return X ? X->main_coro : NULL;
}

void xr_isolate_set_main_coro(XrayIsolate *X, XrCoroutine *coro) {
    if (X)
        X->main_coro = coro;
}

/* ========== VM State ========== */

XrVMState *xr_isolate_get_vm_state(XrayIsolate *X) {
    return X ? &X->vm : NULL;
}

XrVMContext *xr_isolate_get_vm_ctx(XrayIsolate *X) {
    return X ? &X->vm_ctx : NULL;
}

/* ========== Storage Mode ========== */

uint8_t xr_isolate_get_storage_mode(XrayIsolate *X) {
    return X ? X->current_storage_mode : 0;
}

void xr_isolate_set_storage_mode(XrayIsolate *X, uint8_t mode) {
    if (X) {
        X->current_storage_mode = mode;
    }
}

/* ========== Config ========== */

void *xr_isolate_get_userdata(XrayIsolate *X) {
    return X ? X->userdata : NULL;
}

struct XrayConfig *xr_isolate_get_config(XrayIsolate *X) {
    return X ? X->config : NULL;
}

uint32_t xr_isolate_get_init_flags(XrayIsolate *X) {
    return X ? X->init_flags : 0;
}

const char *xr_isolate_get_script_file(XrayIsolate *X) {
    return X ? X->params.script_file : NULL;
}

/* ========== Parser Arena ========== */

struct XrArena *xr_isolate_get_current_arena(XrayIsolate *X) {
    return X ? X->current_arena : NULL;
}

void xr_isolate_set_current_arena(XrayIsolate *X, struct XrArena *arena) {
    if (X) {
        X->current_arena = arena;
    }
}

/* ========== AST Node ID ========== */

uint32_t xr_isolate_next_ast_node_id(XrayIsolate *X) {
    if (!X)
        return 0;
    return ++X->next_ast_node_id;
}

void xr_isolate_reset_ast_node_ids(XrayIsolate *X) {
    if (X)
        X->next_ast_node_id = 0;
}

/* ========== Compile-time String Pool ========== */

struct XrCompileStringPool *xr_isolate_get_string_pool_compile(XrayIsolate *X) {
    return X ? X->compile_string_pool : NULL;
}

void xr_isolate_set_string_pool_compile(XrayIsolate *X, struct XrCompileStringPool *pool) {
    if (X) {
        X->compile_string_pool = pool;
    }
}

/* ========== Type System (Compiler) ========== */

XrTypeInferContext *xr_isolate_get_type_infer_context(XrayIsolate *X) {
    return X ? X->type_infer_context : NULL;
}

XrTypeTable *xr_isolate_get_type_table(XrayIsolate *X) {
    return X ? X->type_table : NULL;
}

struct XrTypePool *xr_isolate_get_analyzer_pool(XrayIsolate *X) {
    return X ? X->analyzer_pool : NULL;
}

/* ========== Memory Tracker ========== */

struct XrMemoryTracker *xr_isolate_get_memory_tracker(XrayIsolate *X) {
    return X ? X->memory_tracker : NULL;
}

/* ========== Debug ========== */

void *xr_isolate_get_debug_state(XrayIsolate *X) {
    return X ? X->debug_state : NULL;
}

void xr_isolate_set_debug_state(XrayIsolate *X, void *state) {
    if (X) {
        X->debug_state = state;
    }
}

void *xr_isolate_get_debug_hooks(XrayIsolate *X) {
    return X ? X->debug_hooks : NULL;
}

void xr_isolate_set_debug_hooks(XrayIsolate *X, void *hooks) {
    if (X) {
        X->debug_hooks = hooks;
    }
}

struct XrSourceCache *xr_isolate_get_source_cache(XrayIsolate *X) {
    return X ? X->source_cache : NULL;
}

void xr_isolate_set_source_cache(XrayIsolate *X, struct XrSourceCache *cache) {
    if (X) {
        X->source_cache = cache;
    }
}

/* ========== REPL ========== */

struct XrReplSymbolTable *xr_isolate_get_repl_symbols(XrayIsolate *X) {
    return X ? X->repl_symbols : NULL;
}

void xr_isolate_set_repl_symbols(XrayIsolate *X, struct XrReplSymbolTable *syms) {
    if (X) {
        X->repl_symbols = syms;
    }
}

/* ========== Exception Print Suppression ========== */

bool xr_isolate_get_suppress_exception_print(XrayIsolate *X) {
    return X ? X->suppress_exception_print : false;
}

void xr_isolate_set_suppress_exception_print(XrayIsolate *X, bool suppress) {
    if (X) {
        X->suppress_exception_print = suppress;
    }
}

/* ========== Extension Type System ========== */

uint8_t xr_alloc_extension_type(XrayIsolate *isolate, const char *name) {
    if (!isolate)
        return 0;
    uint8_t id = isolate->ext_type_next;
    if (id >= XGC_MAX_TYPES) {
        xr_log_warning("ext_type", "extension type slots exhausted (max %d)", XGC_MAX_TYPES);
        return 0;
    }
    isolate->ext_type_next = id + 1;
    isolate->ext_type_names[id] = name;
    return id;
}

void xr_register_extension_destroy(XrayIsolate *isolate, uint8_t type_id,
                                   XrExtDestroyFn destroy_fn) {
    if (!isolate || type_id >= XGC_MAX_TYPES)
        return;
    isolate->ext_destroy_funcs[type_id] = (XrGCDestroyFn) destroy_fn;
    isolate->ext_finalize_bitmap |= (1ULL << type_id);
}

void xr_register_extension_traverse(XrayIsolate *isolate, uint8_t type_id,
                                    XrExtTraverseFn traverse_fn) {
    if (!isolate || type_id >= XGC_MAX_TYPES)
        return;
    isolate->ext_traverse_funcs[type_id] = (void *) traverse_fn;
    isolate->ext_has_refs_bitmap |= (1ULL << type_id);
}

uint64_t xr_isolate_get_ext_finalize_bitmap(XrayIsolate *isolate) {
    return isolate ? isolate->ext_finalize_bitmap : 0;
}

uint64_t xr_isolate_get_ext_has_refs_bitmap(XrayIsolate *isolate) {
    return isolate ? isolate->ext_has_refs_bitmap : 0;
}

XrExtDestroyFn xr_isolate_get_ext_destroy(XrayIsolate *isolate, uint8_t type_id) {
    if (!isolate || type_id >= XGC_MAX_TYPES)
        return NULL;
    return (XrExtDestroyFn) isolate->ext_destroy_funcs[type_id];
}

XrExtTraverseFn xr_isolate_get_ext_traverse(XrayIsolate *isolate, uint8_t type_id) {
    if (!isolate || type_id >= XGC_MAX_TYPES)
        return NULL;
    return (XrExtTraverseFn) isolate->ext_traverse_funcs[type_id];
}

/* ========== Thread Local API ========== */

XrayIsolate *xray_isolate_current(void) {
    return g_current_isolate;
}
