/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xisolate_full.c - Full runtime initialization (heavy subsystems)
 *
 * KEY CONCEPT:
 *   Implements init_extra/cleanup_extra callbacks that initialize
 *   compiler, analyzer, classes, modules, reflection, regex, etc.
 *   xray_isolate_setup_full() sets these callbacks on params.
 *
 * WHY THIS DESIGN:
 *   This is a separate .o so that bytecode-bundled executables
 *   (which use XR_INIT_RUNTIME) never link this file, keeping
 *   the binary small (~600KB vs ~2MB).
 */

#include "../base/xlog.h"

#include "../runtime/xisolate_internal.h"
#include "../base/xchecks.h"
#include "../runtime/value/xtype_pool.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xtype_names.h"
#include "../runtime/class/xclass_system.h"
#include "xglobal_object.h"
#include "../base/xconfig.h"
#include "../frontend/parser/xparse.h"
#include "../frontend/parser/xast.h"
#include "../runtime/class/xreflect_registry.h"
#include "../module/xmodule.h"
#include "../base/xsource_cache.h"
#include "../runtime/xstdlib_bridge.h"
#include "xrepl.h"
#include "../runtime/class/xreflect_api.h"
#include "../runtime/object/builtins/xjson_builtins.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../base/xmalloc.h"
#include "../base/xglobal_indices.h"
#include <stdio.h>
#include <string.h>

/* ========== Full Init Callback ========== */

static int isolate_init_full(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "isolate_init_full: NULL isolate");
    // Config
    isolate->config = xr_malloc(sizeof(XrayConfig));
    if (isolate->config) {
        xr_config_init((XrayConfig*)isolate->config);
    }

    // Process-level type singletons (idempotent, safe to call multiple times)
    xr_type_global_init();

    // Analyzer type pool
    isolate->analyzer_pool = xr_type_pool_new();
    if (isolate->analyzer_pool) {
        xr_type_set_current_pool(isolate->analyzer_pool,
            &isolate->analyzer_pool->next_type_id);
    }

    // Symbol table
    isolate->symbol_table = xr_symbol_table_create();
    if (isolate->symbol_table) {
        xr_symbol_table_init_builtins((XrSymbolTable*)isolate->symbol_table);
    }

    // Type registry (must be before core_init, which registers classes)
    xr_registry_init(isolate);

    // Core class system (creates Object, String, Array, etc.)
    xr_core_init(isolate);

    // Reflection API (needs core->objectClass, so must be after core_init)
    xr_reflect_api_init(isolate);

    // Json utility class (static methods: Json.keys(), Json.has(), etc.)
    xr_json_api_init(isolate);

    // Global object + core classes + builtins
    isolate->global_object = xr_global_object_create(isolate);
    if (isolate->global_object) {
        if (!xr_global_register_all_core_classes(
                (XrGlobalObject*)isolate->global_object, isolate)) {
            xr_log_warning("isolate", "failed to register core classes");
            return -1;
        }
        if (!xr_global_register_all_builtin_functions(
                (XrGlobalObject*)isolate->global_object)) {
            xr_log_warning("isolate", "failed to register builtin functions");
            return -1;
        }
    }

    // Module system
    xr_module_system_init(isolate);

    // Compiler hooks for import
    {
        typedef void *(*GenFn3)(void*, const char*, const char*);
        typedef void *(*GenFn3b)(void*, void*, const char*);
        typedef void  (*GenFn2)(void*, void*);
        xr_module_set_compiler_hooks(
            (GenFn3)xr_parse_with_source,
            (GenFn3b)xr_compile_ast_with_source,
            (GenFn3)xr_compile_source_with_path,
            (GenFn2)xr_ast_free);
    }

    // Regex
    xr_regex_init_native_type(isolate);

    // Source cache
    isolate->source_cache = xr_source_cache_new();

    // Register core classes to VM builtins array (must be after all classes created)
    // init_globals() in xr_vm_init ran before init_extra, so isolate->core was NULL.
    if (isolate->core) {
        if (isolate->core->reflectClass)
            isolate->vm.builtins[XR_GLOBAL_VAR_REFLECT] = xr_value_from_class(isolate->core->reflectClass);
        if (isolate->core->arrayClass)
            isolate->vm.builtins[XR_GLOBAL_VAR_ARRAY] = xr_value_from_class(isolate->core->arrayClass);
        if (isolate->core->setClass)
            isolate->vm.builtins[XR_GLOBAL_VAR_SET] = xr_value_from_class(isolate->core->setClass);
        if (isolate->core->mapClass)
            isolate->vm.builtins[XR_GLOBAL_VAR_MAP] = xr_value_from_class(isolate->core->mapClass);
        if (isolate->core->stringClass)
            isolate->vm.builtins[XR_GLOBAL_VAR_STRING] = xr_value_from_class(isolate->core->stringClass);
        if (isolate->core->jsonClass)
            isolate->vm.builtins[XR_GLOBAL_VAR_JSON] = xr_value_from_class(isolate->core->jsonClass);
        if (isolate->vm.builtin_count < XR_USER_GLOBALS_START)
            isolate->vm.builtin_count = XR_USER_GLOBALS_START;
    }

    return 0;
}

/* ========== Full Cleanup Callback ========== */

static void isolate_cleanup_full(XrayIsolate *isolate) {

    if (isolate->source_cache) {
        xr_source_cache_free(isolate->source_cache);
        isolate->source_cache = NULL;
    }

    if (isolate->module_registry) {
        xr_module_system_free(isolate);
    }

    if (isolate->global_object) {
        xr_global_object_destroy((XrGlobalObject*)isolate->global_object);
        isolate->global_object = NULL;
    }

    if (isolate->core) {
        xr_core_free(isolate);
        isolate->core = NULL;
    }

    if (isolate->type_registry) {
        xr_registry_free(isolate);
        isolate->type_registry = NULL;
    }

    if (isolate->symbol_table) {
        xr_symbol_table_destroy((XrSymbolTable*)isolate->symbol_table);
        isolate->symbol_table = NULL;
    }

    if (isolate->analyzer_pool) {
        xr_type_pool_free(isolate->analyzer_pool);
        isolate->analyzer_pool = NULL;
    }

    // Free REPL symbol table
    if (isolate->repl_symbols) {
        xr_repl_symbols_free(isolate->repl_symbols);
        isolate->repl_symbols = NULL;
    }
}

/* ========== Public: Setup Full Runtime ========== */

void xray_isolate_setup_full(XrayIsolateParams *params) {
    if (!params) return;
    params->init_extra = isolate_init_full;
    params->cleanup_extra = isolate_cleanup_full;
}

