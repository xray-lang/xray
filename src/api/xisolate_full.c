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
 *   Implements the sole VM constructor. Base runtime allocation and heavy
 *   compiler, analyzer, class, and module initialization succeed or fail as
 *   one operation.
 */

#include "../base/xlog.h"

#include "../runtime/xisolate_internal.h"
#include "../base/xchecks.h"
#include "../runtime/mem/xobj_destroy_ops.h"
#include "../runtime/value/xtype_pool.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xtype_names.h"
#include "../runtime/class/xclass_system.h"
#include "../runtime/class/xinstance.h"
#include "../runtime/object/xarray.h"
#include "../runtime/object/xstring.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/xglobals_table.h"
#include "../runtime/xisolate_api.h"
#include "xglobal_object.h"
#include "../base/xconfig.h"
#include "../frontend/parser/xparse.h"
#include "../frontend/parser/xast.h"
#include "../runtime/class/xtype_registry.h"
#include "../module/xmodule.h"
#include "../runtime/xstdlib_bridge.h"
#include "../runtime/object/builtins/xjson_builtins.h"
#include "../runtime/mem/xcycle_detector.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../vm/xvm_internal.h"
#include "../vm/xvm_profiler.h"
#include "../coro/xscope_transfer.h"

#include "../base/xmalloc.h"
#include "../../stdlib/stdlib_cache.h"
#include "../base/xglobal_indices.h"
#include "../frontend/analyzer/xanalyzer_native_types.h"
#include "../toolchain/xcompiler_session.h"
#include "../os/os_fs.h"
#include <stdio.h>
#include <string.h>

void xr_isolate_register_runtime_prelude_enums(XrVMRuntime *isolate);

static bool isolate_config_is_valid(const XrVMConfig *params) {
    if (!params)
        return false;
    if (params->script_argc < 0 || (params->script_argc > 0 && !params->script_argv))
        return false;
    for (int i = 0; i < params->script_argc; i++) {
        if (!params->script_argv[i])
            return false;
    }
    return true;
}

static bool isolate_materialize_script_info(XrVMRuntime *isolate, const char *script_file,
                                            int argc, char **argv) {
    if (!isolate || !isolate->core_rt || !isolate->core || !isolate->core->processClass)
        return false;

    xr_script_info_set(&isolate->core_rt->script_info, script_file, argc, argv);

    char abs_path[XR_PATH_MAX];
    char dir_path[XR_PATH_MAX];
    XrString *main_str = NULL;
    XrString *dir_str = NULL;

    if (script_file && xr_fs_realpath(script_file, abs_path, sizeof(abs_path))) {
        main_str = xr_string_intern(isolate, abs_path, strlen(abs_path), 0);
        snprintf(dir_path, sizeof(dir_path), "%s", abs_path);
        char *last_slash = strrchr(dir_path, '/');
        if (last_slash) {
            *last_slash = '\0';
            dir_str = xr_string_intern(isolate, dir_path, strlen(dir_path), 0);
        } else if (xr_fs_getcwd(dir_path, sizeof(dir_path))) {
            dir_str = xr_string_intern(isolate, dir_path, strlen(dir_path), 0);
        }
    } else if (script_file) {
        main_str = xr_string_intern(isolate, script_file, strlen(script_file), 0);
    }
    if (script_file && !main_str)
        return false;

    XrArray *args_array =
        xr_array_with_capacity_in(&isolate->core_rt->root_alloc, argc, XR_ELEM_ANY);
    if (!args_array)
        return false;
    for (int i = 0; i < argc; i++) {
        XrString *arg_str = xr_string_intern(isolate, argv[i], strlen(argv[i]), 0);
        if (!arg_str)
            return false;
        xr_array_push(args_array, xr_string_value(arg_str));
    }

    XrInstance *process = xr_instance_new(isolate, isolate->core->processClass);
    if (!process)
        return false;
    xr_instance_set_field_fast(process, PROCESS_FIELD_FILE,
                               main_str ? xr_string_value(main_str) : xr_null());
    xr_instance_set_field_fast(process, PROCESS_FIELD_ARGS, xr_value_from_array(args_array));
    xr_instance_set_field_fast(process, PROCESS_FIELD_DIR,
                               dir_str ? xr_string_value(dir_str) : xr_null());
    isolate->vm.builtins[XR_GLOBAL_VAR_PROCESS] = xr_value_from_instance(process);
    isolate->vm.builtins[XR_GLOBAL_VAR_FILE] = main_str ? xr_string_value(main_str) : xr_null();
    isolate->vm.builtins[XR_GLOBAL_VAR_DIR] = dir_str ? xr_string_value(dir_str) : xr_null();
    if (isolate->vm.builtin_count < XR_USER_GLOBALS_START)
        isolate->vm.builtin_count = XR_USER_GLOBALS_START;
    return true;
}

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

    // Route Map/Set instance key hash/equality through user hash()/operator ==.
    xr_value_install_instance_hooks();

    // Core class system (creates Object, String, Array, etc.)
    xr_core_init(isolate);
    xr_scope_transfer_enable_core(isolate->core_rt);

    // Json utility class (static methods: Json.keys(), Json.containsKey(), etc.)
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

    // Native XrClasses for Regex / NetConn / NetListener are registered up
    // front inside stdlib loaders. Pure stdlib classes such as
    // datetime.DateTime must be imported from their module.

    // Source cache is owned by the compiler session and only borrowed by VM error display.
    if (!xr_compiler_session_ensure_source_cache(isolate->compiler_session))
        return -1;

    // Register core classes to VM builtins array (must be after all classes created).
    // init_globals() in xr_execution_engine_init ran before full VM initialization.
    if (isolate->core) {
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
        if (isolate->core_rt->native_type_classes[XR_TCOUNTDOWNLATCH])
            isolate->vm.builtins[XR_GLOBAL_VAR_COUNTDOWNLATCH] =
                xr_value_from_class(isolate->core_rt->native_type_classes[XR_TCOUNTDOWNLATCH]);
        if (isolate->core_rt->native_type_classes[XR_TSEMAPHORE])
            isolate->vm.builtins[XR_GLOBAL_VAR_SEMAPHORE] =
                xr_value_from_class(isolate->core_rt->native_type_classes[XR_TSEMAPHORE]);
        if (isolate->core_rt->native_type_classes[XR_TEVENTCOUNT])
            isolate->vm.builtins[XR_GLOBAL_VAR_EVENTCOUNT] =
                xr_value_from_class(isolate->core_rt->native_type_classes[XR_TEVENTCOUNT]);
        if (isolate->vm.builtin_count < XR_USER_GLOBALS_START)
            isolate->vm.builtin_count = XR_USER_GLOBALS_START;
    }
    xr_isolate_register_runtime_prelude_enums(isolate);

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

#ifdef XR_ENABLE_CYCLE_DETECTOR
    /* Scan the root execution's heap while class names are still readable.
     *
     * The heap itself is torn down later, inside xr_runtime_core_delete — but
     * the symbol table that owns every interned class name goes away right
     * below, so a scan from there would print freed memory for the type of
     * each object on a cycle. This is the last point where a cycle in the main
     * execution can be reported with names attached. */
    {
        XrCycleReport root_report;
        (void) xr_cycle_detector_scan(&isolate->core_rt->root_heap, &root_report);
        /* Claim the scan so the teardown path does not report the same cycles
         * a second time. */
        isolate->core_rt->root_heap.is_tearing_down = 1;

        /* The shared domain has no coroutine-heap teardown to bound it and no
         * `weak` to break it (W4), so a cycle here is a process-lifetime leak.
         * Workers are stopped by now, which gives the scan the quiescence it
         * requires. */
        XrCycleReport shared_report;
        (void) xr_cycle_detector_scan_shared(&shared_report);
    }
#endif

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
    if (!isolate_config_is_valid(params)) {
        xr_log_warning("isolate", "invalid VM configuration");
        return NULL;
    }
    XrVMRuntime *isolate = (XrVMRuntime *) xr_malloc(sizeof(XrVMRuntime));
    if (!isolate) {
        xr_log_warning("isolate", "failed to allocate isolate");
        return NULL;
    }
    memset(isolate, 0, sizeof(XrVMRuntime));

    isolate->params = *params;

    XrRuntimeCoreConfig core_cfg = {
        .owner_isolate = isolate,
        .userdata = isolate->params.userdata,
    };
    isolate->core_rt = xr_runtime_core_new(&core_cfg);
    if (!isolate->core_rt)
        goto fail;
    xr_runtime_core_enable_basic_destroy_ops(isolate->core_rt);
    xr_script_info_set(&isolate->core_rt->script_info, isolate->params.script_file,
                       isolate->params.script_argc, isolate->params.script_argv);

    isolate->globals = xr_globals_create(64);
    if (!isolate->globals)
        goto fail;
    if (xr_execution_engine_init(isolate) != 0)
        goto fail;

#if XR_ENABLE_VM_PROFILER
    isolate->profiler = xr_calloc(1, sizeof(VMProfiler));
    if (!isolate->profiler)
        goto fail_after_vm;
#endif

    xr_isolate_enter(isolate);

    XrExecutionContext *previous =
        xr_exec_context_enter(xr_runtime_core_module_exec(isolate->core_rt));
    int init_result = isolate_init_full(isolate);
    if (init_result == 0 &&
        (isolate->params.script_file || isolate->params.script_argc != 0 ||
         isolate->params.script_argv) &&
        !isolate_materialize_script_info(isolate, isolate->params.script_file,
                                         isolate->params.script_argc,
                                         isolate->params.script_argv))
        init_result = -1;
    xr_exec_context_restore(previous);
    if (init_result != 0) {
        isolate_cleanup_full(isolate);
        xray_vm_delete(isolate);
        return NULL;
    }
    isolate->lifecycle_cleanup = isolate_cleanup_full;
    return isolate;

#if XR_ENABLE_VM_PROFILER
fail_after_vm:
#endif
    xr_execution_engine_cleanup(isolate);
fail:
    if (isolate->globals)
        xr_globals_destroy((XrGlobalsTable *) isolate->globals);
    xr_runtime_core_delete(isolate->core_rt);
    xr_free(isolate);
    return NULL;
}
