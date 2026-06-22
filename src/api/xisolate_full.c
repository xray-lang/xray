/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xisolate_full.c - Full VM construction (heavy subsystems)
 *
 * KEY CONCEPT:
 *   Implements the explicit full VM constructor that initializes compiler,
 *   analyzer, classes, modules, reflection, regex, etc.
 *
 * WHY THIS DESIGN:
 *   This is a separate .o so that bytecode-bundled executables never link
 *   the full compiler/frontend path.
 */

#include "../base/xlog.h"

#include "../runtime/xisolate_internal.h"
#include "../base/xchecks.h"
#include "../runtime/mem/xobj_destroy_ops.h"
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
#include "../runtime/xstdlib_bridge.h"
#include "../runtime/class/xreflect_api.h"
#include "../runtime/object/builtins/xjson_builtins.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../coro/xscope_transfer.h"

#include "../base/xmalloc.h"
#include "../../stdlib/stdlib_cache.h"
#include "../base/xglobal_indices.h"
#include "../frontend/analyzer/xanalyzer_native_types.h"
#include "../toolchain/xcompiler_session.h"
#include <stdio.h>
#include <string.h>

/* ========== Full Init Callback ========== */

static int isolate_init_full(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "isolate_init_full: NULL isolate");

    // Process-level type singletons (idempotent, safe to call multiple times)
    xr_type_global_init();

    XrCompilerSessionConfig compiler_cfg = {
        .vm_host = isolate,
    };
    isolate->compiler_session = xr_compiler_session_new(&compiler_cfg);
    if (!isolate->compiler_session)
        return -1;
    xr_compiler_session_install_analyzer_pool(isolate->compiler_session);
    if (!xr_compiler_session_analyzer_pool(isolate->compiler_session))
        return -1;

    // Symbol table
    isolate->core_rt->symbol_table = xr_symbol_table_create();
    if (!isolate->core_rt->symbol_table)
        return -1;
    xr_symbol_table_init_builtins((XrSymbolTable *) isolate->core_rt->symbol_table);

    // Type registry (must be before core_init, which registers classes)
    xr_registry_init(isolate);
    xr_runtime_core_enable_full_destroy_ops(isolate->core_rt);

    // Core class system (creates Object, String, Array, etc.)
    xr_core_init(isolate);
    xr_scope_transfer_enable_core(isolate->core_rt);

    // Reflection API (needs core->objectClass, so must be after core_init)
    xr_reflect_api_init(isolate);

    // Json utility class (static methods: Json.keys(), Json.has(), etc.)
    xr_json_api_init(isolate);

    // Global object + core classes + builtins
    isolate->global_object = xr_global_object_create(isolate);
    if (!isolate->global_object)
        return -1;
    if (!xr_global_register_all_core_classes((XrGlobalObject *) isolate->global_object, isolate)) {
        xr_log_warning("isolate", "failed to register core classes");
        return -1;
    }
    if (!xr_global_register_all_builtin_functions((XrGlobalObject *) isolate->global_object)) {
        xr_log_warning("isolate", "failed to register builtin functions");
        return -1;
    }

    // Module system
    xr_module_system_init(isolate);

    // Auto-load the prelude module so that built-in type names (Array,
    // Map, Json, BigInt, ...) resolve via the unified prelude symbol
    // table without requiring the user to write `import prelude`. Going
    // through xr_module_import here ensures the registry caches the
    // module exactly once, so a later explicit import hits the cache.
    (void) xr_module_import(isolate, "prelude");

    // Compiler hooks for import
    xr_module_set_compiler_hooks(isolate, isolate->compiler_session, xr_parse_with_source,
                                 xr_compile_ast_with_source, xr_compile_source_with_path,
                                 xr_program_destroy);

    // Native XrClasses for Logger / DateTime / Regex / NetConn /
    // NetListener are registered up front inside the prelude module
    // loader (xr_prelude_register_all_native_types), so user code can
    // write `let dt: DateTime = ...` and `r.test(...)` without a
    // separate stdlib import.

    // Source cache is owned by the compiler session and only borrowed by VM error display.
    if (!xr_compiler_session_ensure_source_cache(isolate->compiler_session))
        return -1;

    // Register core classes to VM builtins array (must be after all classes created).
    // init_globals() in xr_vm_init ran before full VM initialization.
    if (isolate->core) {
        if (isolate->core->reflectClass)
            isolate->vm.builtins[XR_GLOBAL_VAR_REFLECT] =
                xr_value_from_class(isolate->core->reflectClass);
        if (isolate->core->arrayClass)
            isolate->vm.builtins[XR_GLOBAL_VAR_ARRAY] =
                xr_value_from_class(isolate->core->arrayClass);
        if (isolate->core->setClass)
            isolate->vm.builtins[XR_GLOBAL_VAR_SET] = xr_value_from_class(isolate->core->setClass);
        if (isolate->core->mapClass)
            isolate->vm.builtins[XR_GLOBAL_VAR_MAP] = xr_value_from_class(isolate->core->mapClass);
        if (isolate->core->stringClass)
            isolate->vm.builtins[XR_GLOBAL_VAR_STRING] =
                xr_value_from_class(isolate->core->stringClass);
        if (isolate->core->jsonClass)
            isolate->vm.builtins[XR_GLOBAL_VAR_JSON] =
                xr_value_from_class(isolate->core->jsonClass);
        if (isolate->core_rt->native_type_classes[XR_TWORKQUEUE])
            isolate->vm.builtins[XR_GLOBAL_VAR_WORKQUEUE] =
                xr_value_from_class(isolate->core_rt->native_type_classes[XR_TWORKQUEUE]);
        if (isolate->core_rt->native_type_classes[XR_TRESULTGROUP])
            isolate->vm.builtins[XR_GLOBAL_VAR_RESULTGROUP] =
                xr_value_from_class(isolate->core_rt->native_type_classes[XR_TRESULTGROUP]);
        if (isolate->vm.builtin_count < XR_USER_GLOBALS_START)
            isolate->vm.builtin_count = XR_USER_GLOBALS_START;
    }

#if XR_DEBUG
    // Verify C-registered methods match .xr declarations
    xa_native_verify_protocol(isolate);
#endif

    return 0;
}

/* ========== Full Cleanup Callback ========== */

static void isolate_cleanup_full(XrVMRuntime *isolate) {
    // Stdlib per-isolate cache. Must be freed before the module registry so
    // any dynamic-layout class it holds is released while the owning
    // isolate state is still intact.
    xr_stdlib_cache_free(isolate);

    if (isolate->module_registry) {
        xr_module_system_free(isolate);
    }

    if (isolate->global_object) {
        xr_global_object_destroy((XrGlobalObject *) isolate->global_object);
        isolate->global_object = NULL;
    }

    if (isolate->core) {
        xr_core_free(isolate);
        isolate->core = NULL;
    }

    if (isolate->core_rt->type_registry) {
        xr_registry_free(isolate);
        isolate->core_rt->type_registry = NULL;
    }

    if (isolate->core_rt->symbol_table) {
        xr_symbol_table_destroy((XrSymbolTable *) isolate->core_rt->symbol_table);
        isolate->core_rt->symbol_table = NULL;
    }

    if (isolate->compiler_session) {
        xr_compiler_session_delete(isolate->compiler_session);
        isolate->compiler_session = NULL;
    }
}

/* ========== Public: Create Full VM Runtime ========== */

XrVMRuntime *xray_vm_new_full(const XrVMConfig *params) {
    XrVMRuntime *isolate = xray_vm_new(params);
    if (!isolate)
        return NULL;

    if (isolate_init_full(isolate) != 0) {
        isolate_cleanup_full(isolate);
        xray_vm_delete(isolate);
        return NULL;
    }
    isolate->lifecycle_cleanup = isolate_cleanup_full;
    return isolate;
}
