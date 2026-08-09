/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xisolate_scripting.c - Script execution API (dostring, dofile)
 *
 * KEY CONCEPT:
 *   Functions that require the compiler/parser are isolated here so that
 *   bytecode-only executables (xray build) never link this .o file.
 *
 * RELATED MODULES:
 *   - xray_isolate.c: core lifecycle (new/delete)
 *   - xisolate_tls.c: thread-local storage
 */

#include "../runtime/xisolate_internal.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../base/xchecks.h"
#include "../runtime/object/xstring.h"
#include "../runtime/object/xarray.h"
#include "../runtime/class/xinstance.h"
#include "../runtime/class/xclass_system.h"
#include "../base/xglobal_indices.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../module/xmodule.h"
#include "../module/xmodule_graph.h"
#include "../module/xmodule_resolver.h"
#include "../runtime/value/xvalue.h"
#include "../vm/xic_method.h"
#include "../vm/xvm_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "../os/os_fs.h"

#include "../base/xmalloc.h"
#include "../vm/xdebug.h"
#include "../base/xsource_cache.h"
#include "../toolchain/xcompiler_session.h"

typedef struct DostringGraphState {
    XrModuleGraph *graph;
    XaAnalyzer *analyzer;
    XrModuleRegistry *registry;
    XrModule **previous_module_table;
    int previous_module_table_count;
    XrModule **owned_module_table;
} DostringGraphState;

static XrSourceCache *ensure_script_source_cache(XrVMRuntime *isolate) {
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    return xr_compiler_session_ensure_source_cache(session);
}

static void dostring_graph_cleanup(XrCompilerSession *session, DostringGraphState *state) {
    if (!state)
        return;
    if (session)
        xr_compiler_session_set_module_graph(session, NULL);
    if (state->registry && state->registry->module_table == state->owned_module_table) {
        state->registry->module_table = state->previous_module_table;
        state->registry->module_table_count = state->previous_module_table_count;
    }
    xr_free(state->owned_module_table);
    if (state->analyzer) {
        xa_analyzer_set_graph(state->analyzer, NULL);
        xa_analyzer_free(state->analyzer);
    }
    if (state->graph)
        xr_module_graph_free(state->graph);
    memset(state, 0, sizeof(*state));
}

static bool analyze_graph_exports_for_dostring(XrCompilerSession *session, XrModuleGraph *graph,
                                               XaAnalyzer **out_analyzer) {
    XaAnalyzer *analyzer = xa_analyzer_new(session);
    if (!analyzer) {
        fprintf(stderr, "Error: cannot create analyzer for eval module graph\n");
        return false;
    }

    xa_analyzer_set_graph(analyzer, graph);
    int graph_errors = 0;
    for (int ti = 0; ti < graph->topo_count; ti++) {
        int idx = graph->topo_order[ti];
        XrModuleSpec *spec = &graph->specs[idx];
        if (idx == graph->entry_index || !spec->ast)
            continue;

        xa_analyzer_analyze(analyzer, spec->source_path, (XrAstNode *) spec->ast);
        spec->export_symbols =
            xa_analyzer_collect_export_symbols(analyzer, (XrAstNode *) spec->ast);

        int diag_count = 0;
        XaDiagnostic *diags = xa_analyzer_get_diagnostics(analyzer, &diag_count);
        for (XaDiagnostic *d = diags; d; d = d->next) {
            if (d->severity == XR_DIAG_SEV_ERROR) {
                graph_errors++;
                fprintf(stderr, "%s:%d:%d: error: %s\n",
                        spec->source_path ? spec->source_path : "<eval>", d->location.line,
                        d->location.column, d->message);
            }
        }
        xa_analyzer_clear_diagnostics(analyzer);
    }

    if (graph_errors > 0) {
        xa_analyzer_set_graph(analyzer, NULL);
        xa_analyzer_free(analyzer);
        return false;
    }

    *out_analyzer = analyzer;
    return true;
}

static bool preload_graph_modules_for_dostring(XrVMRuntime *isolate, XrModuleGraph *graph,
                                               DostringGraphState *state) {
    XrModuleRegistry *registry = state->registry;
    int nmod = graph->topo_count;
    if (!registry || nmod <= 1)
        return true;

    XrModule **mod_table = NULL;
    if (!xr_module_graph_preload(isolate, graph, &mod_table))
        return false;

    state->previous_module_table = registry->module_table;
    state->previous_module_table_count = registry->module_table_count;
    state->owned_module_table = mod_table;
    registry->module_table = mod_table;
    registry->module_table_count = nmod;

    return true;
}

static bool prepare_graph_for_dostring(XrVMRuntime *isolate, XrCompilerSession *session,
                                       const char *source, DostringGraphState *state) {
    XrModuleRegistry *registry = (XrModuleRegistry *) xr_isolate_get_module_registry(isolate);
    XrModuleResolver *resolver = xr_module_registry_get_resolver(registry);
    if (!registry || !resolver)
        return true;

    XrModuleGraph *graph = xr_module_graph_new(session, resolver);
    if (!graph)
        return true;

    char *err = NULL;
    if (xr_module_graph_build_source(graph, "<eval>", source, &err) != 0) {
        fprintf(stderr, "Error: %s\n", err ? err : "failed to build eval module graph");
        xr_free(err);
        xr_module_graph_free(graph);
        return false;
    }
    xr_free(err);

    xr_module_graph_topological_sort(graph);
    if (graph->has_cycle) {
        fprintf(stderr, "Error: %s\n",
                graph->cycle_desc ? graph->cycle_desc : "circular dependency detected");
        xr_module_graph_free(graph);
        return false;
    }

    if (graph->topo_count <= 1) {
        xr_module_graph_free(graph);
        return true;
    }

    memset(state, 0, sizeof(*state));
    state->graph = graph;
    state->registry = registry;
    if (!analyze_graph_exports_for_dostring(session, graph, &state->analyzer)) {
        dostring_graph_cleanup(session, state);
        return false;
    }

    xr_compiler_session_set_module_graph(session, graph);
    if (!preload_graph_modules_for_dostring(isolate, graph, state)) {
        dostring_graph_cleanup(session, state);
        return false;
    }
    return true;
}

/* ========== IC Feedback Dump ========== */

// Recursively dump IC type feedback for a proto tree, reading the
// per-coroutine IC tables on the resolved VM context (IC state lives
// ctx-side now, not on the immutable proto).
static void dump_ic_feedback_recursive(XrVMContext *ctx, XrProto *proto) {
    if (!proto || !ctx)
        return;

    const char *name = proto->name ? (const char *) proto->name->data : "<script>";
    XrICMethodTable *icm = xr_vm_ctx_get_ic_methods(ctx, proto);
    if (icm) {
        xr_ic_method_table_dump_feedback(icm, name);
    }

    // Recurse into nested functions
    int nprotos = PROTO_PROTO_COUNT(proto);
    for (int i = 0; i < nprotos; i++) {
        dump_ic_feedback_recursive(ctx, PROTO_PROTO(proto, i));
    }
}

// Common execute + optional dump logic shared by dostring/dofile
static int execute_and_dump(XrVMRuntime *isolate, XrProto *code, const char *label) {
    if (isolate->params.dump_bytecode) {
        xr_disassemble_proto(code, label);
    }
    int result = xr_execute(isolate, code);
    if (isolate->params.dump_ic_feedback) {
        fprintf(stderr, "\n========== IC Type Feedback ==========\n");
        dump_ic_feedback_recursive(xr_vm_current_ctx(isolate), code);
        fprintf(stderr, "======================================\n");
    }
    return result;
}

/* ========== File Reading Helpers ========== */

static char *read_file_source(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        return NULL;
    }

    long size = -1;
    if (fseek(file, 0, SEEK_END) == 0) {
        size = ftell(file);
        fseek(file, 0, SEEK_SET);
    }

    char *source = NULL;
    size_t read_size = 0;

    if (size > 0) {
        source = (char *) xr_malloc(size + 1);
        if (source == NULL) {
            fclose(file);
            return NULL;
        }
        read_size = fread(source, 1, size, file);
        source[read_size] = '\0';
    } else {
        // Pipe/stdin fallback: read in 4KB blocks
        size_t capacity = 4096;
        source = (char *) xr_malloc(capacity);
        if (source == NULL) {
            fclose(file);
            return NULL;
        }
        size_t n;
        while ((n = fread(source + read_size, 1, capacity - read_size - 1, file)) > 0) {
            read_size += n;
            if (read_size + 1 >= capacity) {
                capacity *= 2;
                char *new_source = (char *) xr_realloc(source, capacity);
                if (new_source == NULL) {
                    xr_free(source);
                    fclose(file);
                    return NULL;
                }
                source = new_source;
            }
        }
        source[read_size] = '\0';
    }

    fclose(file);
    return source;
}

/* ========== Execution API ========== */

int xray_vm_dostring(XrVMRuntime *isolate, const char *source) {
    xray_api_checkr(isolate != NULL, "xray_vm_dostring: NULL isolate", -1);
    xray_api_checkr(source != NULL, "xray_vm_dostring: NULL source", -1);

    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    if (!session) {
        fprintf(stderr, "Compiler unavailable: source execution requires a compiler session\n");
        return -1;
    }
    DostringGraphState graph_state = {0};
    if (!prepare_graph_for_dostring(isolate, session, source, &graph_state))
        return -1;

    XrProto *code = xr_compile_source_with_path(session, source, "<eval>");
    if (code == NULL) {
        dostring_graph_cleanup(session, &graph_state);
        fprintf(stderr, "Compilation error\n");
        return -1;
    }

    int result = execute_and_dump(isolate, code, "<eval>");

    xr_free_code(isolate, code);
    dostring_graph_cleanup(session, &graph_state);

    return result;
}

int xray_vm_dofile(XrVMRuntime *isolate, const char *filename) {
    xray_api_checkr(isolate != NULL, "xray_vm_dofile: NULL isolate", -1);
    xray_api_checkr(filename != NULL, "xray_vm_dofile: NULL filename", -1);

    char *source = read_file_source(filename);
    if (source == NULL) {
        fprintf(stderr, "Cannot open file: %s\n", filename);
        return -1;
    }

    XrSourceCache *source_cache = ensure_script_source_cache(isolate);
    if (source_cache) {
        xr_source_cache_add(source_cache, filename, source);
    }

    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    if (!session) {
        fprintf(stderr, "Compiler unavailable: source execution requires a compiler session\n");
        xr_free(source);
        return -1;
    }
    XrProto *code = xr_compile_source_with_path(session, source, filename);
    if (code == NULL) {
        xr_free(source);
        return -1;
    }

    int result = execute_and_dump(isolate, code, filename);

    xr_free_code(isolate, code);
    xr_free(source);

    return result;
}

// Debug version: compile and execute but don't free code (for debug resume)
// Returns proto via out_proto, caller must free with xr_free_code when done
int xray_vm_dofile_debug(XrVMRuntime *isolate, const char *filename, void **out_proto) {
    xray_api_checkr(isolate != NULL, "xray_vm_dofile_debug: NULL isolate", -1);
    xray_api_checkr(filename != NULL, "xray_vm_dofile_debug: NULL filename", -1);

    char *source = read_file_source(filename);
    if (source == NULL) {
        fprintf(stderr, "Cannot open file: %s\n", filename);
        return -1;
    }

    XrSourceCache *source_cache = ensure_script_source_cache(isolate);
    if (source_cache) {
        xr_source_cache_add(source_cache, filename, source);
    }

    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    if (!session) {
        fprintf(stderr, "Compiler unavailable: source execution requires a compiler session\n");
        xr_free(source);
        return -1;
    }
    XrProto *code = xr_compile_source_with_path(session, source, filename);
    if (code == NULL) {
        xr_free(source);
        return -1;
    }

    int result = xr_execute(isolate, code);

    if (out_proto) {
        *out_proto = code;
    } else {
        xr_free_code(isolate, code);
    }

    xr_free(source);

    return result;
}

/* ========== Script Info ========== */

void xray_vm_set_script_info(XrVMRuntime *isolate, const char *script_file, int argc, char **argv) {
    if (isolate == NULL)
        return;

    XrExecutionContext *previous =
        xr_exec_context_enter(xr_runtime_core_module_exec(isolate->core_rt));

    if (isolate->core_rt) {
        xr_script_info_set(&isolate->core_rt->script_info, script_file, argc, argv);
    }

    char abs_path[XR_PATH_MAX];
    char dir_path[XR_PATH_MAX];
    XrString *main_str = NULL;
    XrString *dir_str = NULL;

    if (script_file && xr_fs_realpath(script_file, abs_path, sizeof(abs_path))) {
        main_str = xr_string_intern(isolate, abs_path, strlen(abs_path), 0);

        snprintf(dir_path, XR_PATH_MAX, "%s", abs_path);
        char *last_slash = strrchr(dir_path, '/');
        if (last_slash) {
            *last_slash = '\0';
            dir_str = xr_string_intern(isolate, dir_path, strlen(dir_path), 0);
        } else if (xr_fs_getcwd(dir_path, XR_PATH_MAX)) {
            dir_str = xr_string_intern(isolate, dir_path, strlen(dir_path), 0);
        }
    } else if (script_file) {
        main_str = xr_string_intern(isolate, script_file, strlen(script_file), 0);
    }

    XrArray *args_array =
        xr_array_with_capacity_in(&isolate->core_rt->root_alloc, argc, XR_ELEM_ANY);
    for (int i = 0; i < argc; i++) {
        XrString *arg_str = xr_string_intern(isolate, argv[i], strlen(argv[i]), 0);
        xr_array_push(args_array, xr_string_value(arg_str));
    }

    // Create process singleton with file, args, dir fields
    if (isolate->core && isolate->core->processClass) {
        XrInstance *process = xr_instance_new(isolate, isolate->core->processClass);
        if (process) {
            xr_instance_set_field_fast(process, PROCESS_FIELD_FILE,
                                       main_str ? xr_string_value(main_str) : xr_null());
            xr_instance_set_field_fast(process, PROCESS_FIELD_ARGS,
                                       xr_value_from_array(args_array));
            xr_instance_set_field_fast(process, PROCESS_FIELD_DIR,
                                       dir_str ? xr_string_value(dir_str) : xr_null());

            isolate->vm.builtins[XR_GLOBAL_VAR_PROCESS] = xr_value_from_instance(process);
        }
    }

    // Module-level __file__ and __dir__
    isolate->vm.builtins[XR_GLOBAL_VAR_FILE] = main_str ? xr_string_value(main_str) : xr_null();
    isolate->vm.builtins[XR_GLOBAL_VAR_DIR] = dir_str ? xr_string_value(dir_str) : xr_null();

    if (isolate->vm.builtin_count < XR_USER_GLOBALS_START) {
        isolate->vm.builtin_count = XR_USER_GLOBALS_START;
    }

    xr_exec_context_restore(previous);
}
