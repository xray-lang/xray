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
#include "../runtime/class/xclass_system.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../module/xmodule.h"
#include "../module/xmodule_graph.h"
#include "../module/xmodule_identity.h"
#include "../module/xmodule_resolver.h"
#include "../ir/xi_module.h"
#include "../runtime/value/xvalue.h"
#include "../vm/xic_method.h"
#include "../vm/xvm_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "../base/xmalloc.h"
#include "../vm/xdebug.h"
#include "../base/xsource_cache.h"
#include "../toolchain/xcompiler_session.h"

#include "../frontend/codegen/xcompiler_context.h"
#include "../plan/target/xr_target_profile.h"
#include "../module/xmodule_graph_runtime.h"
#include "../os/os_fs.h"
#include "../runtime/value/xtype_internal.h"

typedef struct ProgramGraphState {
    XrModuleGraph *graph;
    XrModuleResolver *resolver;
    XaAnalyzer *analyzer;
    XrModuleRegistry *registry;
    const XrModuleGraph *previous_source_graph;
    bool source_graph_installed;
    XrModule **previous_module_table;
    int previous_module_table_count;
    XrModule **owned_module_table;
    XrCompiledModuleGraph compilation;
    XrProgramImage image;
    XrTypePool *previous_type_pool;
    bool pool_scope_active;
} ProgramGraphState;

static bool script_compile_input(XrVMRuntime *runtime, XrCompilerSession *session,
                                  bool runtime_imports, XrCompilerInput *output) {
    if (xr_compile_state_status(xr_compile_session_compile_state(session)) != XR_COMPILE_RESOURCE_OK)
        return false;
    if (!runtime || !runtime->native_target_profile || !output)
        return false;
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(runtime->native_target_profile);
    if (!machine)
        return false;
    *output = (XrCompilerInput) {
        .analysis = {session, runtime, &machine->data_layout, NULL},
        .policy = {runtime->native_target_profile, runtime_imports, NULL}
    };
    return true;
}

static bool module_status_ok(const XrCompilerInput *input, XrModuleStatus status) {
    if (status == XR_MODULE_BUDGET || status == XR_MODULE_OUT_OF_MEMORY)
        (void) xr_compile_state_fail(xr_compile_session_compile_state(input->analysis.session),
            status == XR_MODULE_BUDGET ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    return status == XR_MODULE_OK;
}

static void program_graph_cleanup(ProgramGraphState *state) {
    if (!state)
        return;
    if (state->registry && state->registry->module_table == state->owned_module_table) {
        state->registry->module_table = state->previous_module_table;
        state->registry->module_table_count = state->previous_module_table_count;
    }
    if (state->registry && state->source_graph_installed && state->registry->source_graph == state->graph)
        state->registry->source_graph = state->previous_source_graph;
    xr_free(state->owned_module_table);
    xr_program_image_dispose(&state->image);
    xr_compiled_module_graph_dispose(&state->compilation);
    if (state->analyzer) {
        xa_analyzer_set_graph(state->analyzer, NULL);
        xa_analyzer_free(state->analyzer);
    }
    xr_compile_module_graph_free(state->graph);
    xr_compile_module_resolver_free(state->resolver);
    if (state->pool_scope_active)
        xr_type_set_current_pool(state->previous_type_pool, NULL);
    memset(state, 0, sizeof(*state));
}

static bool analyze_program_graph(const XrCompilerInput *input, ProgramGraphState *state) {
    XrCompilerInput local = *input;
    local.analysis.module_identity = state->graph->specs[state->graph->entry_index].canonical;
    state->analyzer = xa_analyzer_new(&local.analysis);
    if (!state->analyzer)
        return false;
    xa_analyzer_set_graph(state->analyzer, state->graph);
    XrCompileState *failure = xr_compile_session_compile_state(input->analysis.session);
    for (int i = 0; i < state->graph->topo_count; i++) {
        XrModuleSpec *spec = &state->graph->specs[state->graph->topo_order[i]];
        if (!spec->ast)
            continue;
        const char *file = spec->source_path ? spec->source_path : spec->canonical;
        xa_analyzer_analyze(state->analyzer, file, spec->ast);
        if (xr_compile_state_status(failure) != XR_COMPILE_RESOURCE_OK)
            return false;
        int errors = xa_analyzer_print_errors(state->analyzer, file);
        XrHashMap *exports = NULL;
        if (!errors && !xa_analyzer_collect_export_symbols_checked(state->analyzer, spec->ast, &exports))
            errors = 1;
        if (xr_compile_state_status(failure) != XR_COMPILE_RESOURCE_OK)
            return false;
        if (!errors && exports != spec->export_symbols) {
            xr_hashmap_owned_free(spec->export_symbols);
            spec->export_symbols = exports;
        }
        xa_analyzer_clear_diagnostics(state->analyzer);
        if (errors)
            return false;
    }
    XrCompileGraphInput graph = {.compiler = input, .analyzer = state->analyzer, .graph = state->graph};
    return xr_compile_module_graph_dependencies(&graph, &state->compilation);
}

static bool build_source_graph(const XrCompilerInput *input, const char *source,
    const char *file, const XrModuleIdentityAuthority *authority, ProgramGraphState *state) {
    XrCompileState *failure = xr_compile_session_compile_state(input->analysis.session);
    if (xr_compile_state_status(failure) != XR_COMPILE_RESOURCE_OK)
        return false;
    state->previous_type_pool = xr_type_get_current_pool();
    state->pool_scope_active = true;
    XrCompileResources *resources = xr_compile_state_resources(failure);
    state->registry = xr_isolate_get_module_registry(input->analysis.runtime);
    if (!state->registry || !source || !xr_module_identity_authority_valid(authority))
        return false;
    XrModuleResolverConfig config = {.stdlib_path = state->registry->stdlib_path};
    if (!module_status_ok(input, xr_compile_module_resolver_new(resources, &config, &state->resolver)) ||
        !module_status_ok(input, xr_compile_module_graph_new(resources, input->analysis.session,
            state->resolver, &state->graph)))
        return false;
    state->graph->X = input->analysis.runtime;
    state->previous_source_graph = state->registry->source_graph;
    state->registry->source_graph = state->graph;
    state->source_graph_installed = true;
    char *canonical = NULL, *logical = NULL, *absolute = NULL, *error = NULL;
    XrModuleStatus status = XR_MODULE_INVALID;
    if (authority->kind == XR_MODULE_IDENTITY_MEMORY)
        status = xr_compile_module_graph_build_logical_source(state->graph, authority, NULL, NULL, source, &error);
    else if (authority->kind == XR_MODULE_IDENTITY_STDLIB && !authority->physical_root) {
        size_t length = 0;
        if (xr_compile_state_string_length(failure, authority->namespace_id, &length) == XR_COMPILE_RESOURCE_OK && length <= 253) {
            char path[512];
            if (xr_compile_state_copy(failure, path, authority->namespace_id, length) == XR_COMPILE_RESOURCE_OK &&
                xr_compile_state_copy(failure, path + length, "/", 1) == XR_COMPILE_RESOURCE_OK &&
                xr_compile_state_copy(failure, path + length + 1, authority->namespace_id, length) == XR_COMPILE_RESOURCE_OK &&
                xr_compile_state_copy(failure, path + length * 2 + 1, ".xr", 4) == XR_COMPILE_RESOURCE_OK)
                status = xr_compile_module_graph_build_logical_source(state->graph, authority, path, file, source, &error);
        }
    } else {
        XrOsIoPolicy policy = xr_compile_io_policy(resources);
        status = xr_module_status_from_io(xr_realpath_owned(&policy, file, &absolute));
        if (status == XR_MODULE_OK)
            status = xr_compile_module_identity_from_source(resources, authority, absolute, &canonical, &logical);
        if (status == XR_MODULE_OK)
            status = xr_compile_module_graph_build_logical_source(state->graph, authority, logical, absolute, source, &error);
    }
    xr_compile_resources_free(absolute);
    xr_compile_resources_free(canonical);
    xr_compile_resources_free(logical);
    bool ready = module_status_ok(input, status);
    if (!ready && xr_compile_state_status(failure) == XR_COMPILE_RESOURCE_OK)
        fprintf(stderr, "Error: %s\n", error ? error : "invalid Source module graph");
    xr_compile_resources_free(error);
    if (!ready || xr_compile_state_status(failure) != XR_COMPILE_RESOURCE_OK ||
        !module_status_ok(input, xr_compile_module_graph_topological_sort(state->graph)))
        return false;
    return state->graph->topo_count > 0 &&
           (state->graph->topo_count == 1 || analyze_program_graph(input, state));
}

static XrProto *compile_graph_entry(const XrCompilerInput *input, ProgramGraphState *state) {
    XiModule *single[1] = {0};
    XrCompileGraphInput graph = {.compiler = input, .analyzer = state->analyzer,
        .graph = state->graph,
        .modules = state->compilation.modules ? state->compilation.modules : single,
        .module_count = state->graph->topo_count};
    int index = state->graph->entry_index;
    int slot = state->graph->specs[index].topo_index;
    if (slot < 0 || slot >= graph.module_count)
        return NULL;
    return xr_compile_ast_in_graph(&graph, index, &graph.modules[slot]);
}

static bool install_program_dependencies(const XrCompilerInput *input, ProgramGraphState *state,
                                          bool preload) {
    if (state->graph->topo_count <= 1)
        return true;
    XrVMRuntime *runtime = input->analysis.runtime;
    if (!xr_program_image_build(runtime, state->graph, &state->compilation, &state->image))
        return false;
    xr_program_image_install(&state->image, state->registry);
    if (!preload)
        return true;
    XrModule **table = NULL;
    if (!xr_module_graph_preload(runtime, state->graph, &table))
        return false;
    state->previous_module_table = state->registry->module_table;
    state->previous_module_table_count = state->registry->module_table_count;
    state->owned_module_table = table;
    state->registry->module_table = table;
    state->registry->module_table_count = state->graph->topo_count;
    return true;
}

typedef struct ModuleSourceContext {
    XrCompilerInput compiler;
    ProgramGraphState state;
} ModuleSourceContext;

static bool module_source_context_dispose(void *opaque, bool succeeded) {
    ModuleSourceContext *context = opaque;
    if (!context)
        return false;
    bool okay = succeeded && xr_compile_state_status(
        xr_compile_session_compile_state(context->compiler.analysis.session)) == XR_COMPILE_RESOURCE_OK;
    program_graph_cleanup(&context->state);
    xr_compile_state_free(context);
    return okay;
}

bool xr_compile_module_source(XrVMRuntime *runtime, XrCompilerSession *session, const char *source,
    const char *source_file, const XrModuleIdentityAuthority *authority, XrModuleSourceCompilation *out) {
    XrCompilerInput input = {0};
    if (!script_compile_input(runtime, session, true, &input))
        return false;
    if (!out || out->initializer || out->context || out->dispose_context || !source)
        return false;
    ModuleSourceContext *context = NULL;
    if (xr_compile_state_calloc(xr_compile_session_compile_state(session), 1, sizeof(*context),
            (void **) &context) != XR_COMPILE_RESOURCE_OK)
        return false;
    context->compiler = input;
    if (!build_source_graph(&context->compiler, source, source_file, authority, &context->state)) {
        (void) module_source_context_dispose(context, false);
        return false;
    }
    XrProto *code = compile_graph_entry(&context->compiler, &context->state);
    if (!code || !install_program_dependencies(&context->compiler, &context->state,
            authority->kind != XR_MODULE_IDENTITY_STDLIB)) {
        xr_free_code(runtime, code);
        (void) module_source_context_dispose(context, false);
        return false;
    }
    *out = (XrModuleSourceCompilation) {.initializer = code, .context = context,
        .dispose_context = module_source_context_dispose};
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

static XrModuleStatus read_file_source(const XrCompilerInput *input, const char *filename, char **output) {
    XrCompileState *state = xr_compile_session_compile_state(input->analysis.session);
    XrCompileResourceStatus first = xr_compile_state_status(state);
    if (first != XR_COMPILE_RESOURCE_OK) {
        if (first == XR_COMPILE_RESOURCE_BUDGET)
            return XR_MODULE_BUDGET;
        if (first == XR_COMPILE_RESOURCE_OUT_OF_MEMORY)
            return XR_MODULE_OUT_OF_MEMORY;
        return XR_MODULE_INVALID;
    }
    XrOsIoPolicy policy = xr_compile_io_policy(xr_compile_state_resources(state));
    uint8_t *bytes = NULL;
    size_t size = 0;
    XrModuleStatus status = xr_module_status_from_io(xr_os_io_read_regular_file(&policy, filename,
        SIZE_MAX - 1, &bytes, &size));
    if (!module_status_ok(input, status))
        return status;
    if (xr_compile_state_resize(state, (void **) &bytes, size + 1) != XR_COMPILE_RESOURCE_OK ||
        xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK)
        goto failure;
    bytes[size] = 0;
    size_t text_size = 0;
    if (xr_compile_state_string_length(state, (const char *) bytes, &text_size) != XR_COMPILE_RESOURCE_OK)
        goto failure;
    if (text_size != size) {
        xr_compile_state_free(bytes);
        return XR_MODULE_INVALID;
    }
    *output = (char *) bytes;
    return XR_MODULE_OK;
failure:
    xr_compile_state_free(bytes);
    return xr_compile_state_status(state) == XR_COMPILE_RESOURCE_BUDGET ? XR_MODULE_BUDGET : XR_MODULE_OUT_OF_MEMORY;
}

static int run_program(XrVMRuntime *isolate, const char *source, const char *filename,
                        const XrModuleIdentityAuthority *authority, void **out_proto) {
    XrCompilerInput compiler = {0};
    if (!script_compile_input(isolate, isolate->compiler_session, false, &compiler))
        return -1;
    ProgramGraphState state = {0};
    if (!build_source_graph(&compiler, source, filename, authority, &state)) {
        program_graph_cleanup(&state);
        return -1;
    }
    XrProto *code = compile_graph_entry(&compiler, &state);
    if (!code || !install_program_dependencies(&compiler, &state, true)) {
        xr_free_code(isolate, code);
        program_graph_cleanup(&state);
        fprintf(stderr, "Compilation error\n");
        return -1;
    }
    const char *label = filename ? filename : "<eval>";
    int result = execute_and_dump(isolate, code, label);
    if (out_proto)
        *out_proto = code;
    else
        xr_free_code(isolate, code);
    program_graph_cleanup(&state);
    return result;
}

int xr_isolate_dostring(XrVMRuntime *isolate, const char *source,
                        const XrModuleIdentityAuthority *authority) {
    xray_api_checkr(isolate != NULL, "xr_isolate_dostring: NULL isolate", -1);
    xray_api_checkr(source != NULL, "xr_isolate_dostring: NULL source", -1);
    xray_api_checkr(authority != NULL && authority->kind == XR_MODULE_IDENTITY_MEMORY &&
                        xr_module_identity_authority_valid(authority),
                    "xr_isolate_dostring: invalid memory module authority", -1);
    return run_program(isolate, source, NULL, authority, NULL);
}

static int run_program_file(XrVMRuntime *isolate, const char *filename,
    const XrModuleIdentityAuthority *authority, void **out_proto) {
    XrCompilerInput input = {0};
    if (!script_compile_input(isolate, isolate->compiler_session, false, &input))
        return -1;
    char *source = NULL;
    XrModuleStatus status = read_file_source(&input, filename, &source);
    if (status != XR_MODULE_OK) {
        if (status == XR_MODULE_INVALID)
            fprintf(stderr, "Invalid Source file: %s\n", filename);
        return -1;
    }
    if (xr_compile_source_cache_add(isolate->diagnostic_sources, filename, source) != XR_COMPILE_RESOURCE_OK) {
        xr_compile_state_free(source);
        return -1;
    }
    int result = run_program(isolate, source, filename, authority, out_proto);
    xr_compile_state_free(source);
    return result;
}

int xr_isolate_dofile(XrVMRuntime *isolate, const char *filename,
                      const XrModuleIdentityAuthority *authority) {
    xray_api_checkr(isolate != NULL, "xr_isolate_dofile: NULL isolate", -1);
    xray_api_checkr(filename != NULL, "xr_isolate_dofile: NULL filename", -1);
    xray_api_checkr(authority != NULL && authority->kind != XR_MODULE_IDENTITY_MEMORY &&
                        xr_module_identity_authority_valid(authority),
                    "xr_isolate_dofile: file module authority is required", -1);
    return run_program_file(isolate, filename, authority, NULL);
}

// Debug version: compile and execute but don't free code (for debug resume)
// Returns proto via out_proto, caller must free with xr_free_code when done
int xr_isolate_dofile_debug(XrVMRuntime *isolate, const char *filename,
                            const XrModuleIdentityAuthority *authority, void **out_proto) {
    xray_api_checkr(isolate != NULL, "xr_isolate_dofile_debug: NULL isolate", -1);
    xray_api_checkr(filename != NULL, "xr_isolate_dofile_debug: NULL filename", -1);
    xray_api_checkr(authority != NULL && authority->kind != XR_MODULE_IDENTITY_MEMORY &&
                        xr_module_identity_authority_valid(authority),
                    "xr_isolate_dofile_debug: file module authority is required", -1);
    return run_program_file(isolate, filename, authority, out_proto);
}
