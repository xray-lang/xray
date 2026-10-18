/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_compile.c - Source/AST to VM bytecode compilation API
 *
 * KEY CONCEPT:
 *   AST/source compilation into bytecode. VM execution and lifecycle live in
 *   xvm_exec.c so bytecode-only embedders do not link compiler objects.
 */

#include "../runtime/xisolate_internal.h"
#include "../runtime/xisolate_api.h"
#include "xray_vm.h"
#include "../base/xchecks.h"
#include "../base/xfileio.h"
#include "../base/xlog.h"
#include "../frontend/codegen/xcompiler.h"
#include "../frontend/codegen/xcompiler_context.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../frontend/analyzer/xanalyzer_mono.h"
#include "../base/xhashmap.h"
#include "../base/xmalloc.h"
#include "../frontend/parser/xast.h"
#include "../frontend/parser/xparse.h"
#include "../ir/xi.h"
#include "../ir/xi_module.h"
#include "../module/xmodule.h"
#include "../module/xmodule_graph.h"
#include "../module/xmodule_identity.h"
#include "../module/xproto_codec.h"
#include "../os/os_thread.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xtype_internal.h"
#include "../base/xio_policy.h"
#include "../toolchain/xcompiler_session.h"

/* ========== Compilation API ========== */

struct XiFunc;
void xi_func_free(struct XiFunc *f);

static void free_xi_func_opaque(void *ptr) {
    xi_func_free((struct XiFunc *) ptr);
}

static xr_once_t compiler_proto_hooks_once = XR_ONCE_INITIALIZER;

static void init_compiler_proto_hooks(void) {
    xr_instruction_unit_set_ir_free_fn(free_xi_func_opaque);
}

static void ensure_compiler_proto_hooks(void) {
    xr_once_call(&compiler_proto_hooks_once, init_compiler_proto_hooks);
}

typedef struct XrCompileUnitRequest {
    const XrCompilerInput *compiler;
    const XrCompileGraphInput *graph;
    const char *source_file;
    const char *source_content;
    const XrModuleIdentityAuthority *authority;
    const char *canonical;
} XrCompileUnitRequest;

static XrCompileState *compile_state(const XrCompilerInput *input) {
    return input ? xr_compile_session_compile_state(input->analysis.session) : NULL;
}

static bool compile_input_ready(const XrCompilerInput *input) {
    if (xr_compile_state_status(compile_state(input)) != XR_COMPILE_RESOURCE_OK)
        return false;
    return input->analysis.runtime && input->analysis.target && input->policy.target_profile;
}

static bool identity_failure(XrCompileState *state, XrModuleStatus status) {
    if (status == XR_MODULE_BUDGET || status == XR_MODULE_OUT_OF_MEMORY)
        (void) xr_compile_state_fail(state, status == XR_MODULE_BUDGET
            ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    return false;
}

static bool build_compile_unit_identity(const XrCompilerInput *input,
    const XrModuleIdentityAuthority *authority, const char *source_file, char **output) {
    XrCompileState *state = compile_state(input);
    if (!compile_input_ready(input) || !output || *output || !authority)
        return false;
    XrCompileResources *resources = xr_compile_state_resources(state);
    XrModuleStatus status;
    if (authority->kind == XR_MODULE_IDENTITY_MEMORY)
        status = xr_compile_module_identity_from_logical(resources, authority, NULL, output);
    else if (authority->kind == XR_MODULE_IDENTITY_STDLIB) {
        size_t length = 0;
        if (xr_compile_state_string_length(state, authority->namespace_id, &length) != XR_COMPILE_RESOURCE_OK ||
            length > 253)
            return false;
        char logical[512];
        if (xr_compile_state_copy(state, logical, authority->namespace_id, length) != XR_COMPILE_RESOURCE_OK ||
            xr_compile_state_copy(state, logical + length, "/", 1) != XR_COMPILE_RESOURCE_OK ||
            xr_compile_state_copy(state, logical + length + 1, authority->namespace_id, length) != XR_COMPILE_RESOURCE_OK ||
            xr_compile_state_copy(state, logical + length * 2 + 1, ".xr", 4) != XR_COMPILE_RESOURCE_OK)
            return false;
        status = xr_compile_module_identity_from_logical(resources, authority, logical, output);
    } else {
        XrOsIoPolicy policy = xr_compile_io_policy(resources);
        char *absolute = NULL;
        XrOsIoStatus io = xr_realpath_owned(&policy, source_file, &absolute);
        if (io != XR_OS_IO_OK)
            return identity_failure(state, xr_module_status_from_io(io));
        char *logical = NULL;
        status = xr_compile_module_identity_from_source(resources, authority, absolute, output, &logical);
        xr_compile_resources_free(logical);
        xr_compile_resources_free(absolute);
    }
    return status == XR_MODULE_OK || identity_failure(state, status);
}

static XrProto *compile_ast_internal(const XrCompileUnitRequest *request, AstNode *ast,
                                     XiModule **out_module) {
    const XrCompilerInput *input = request ? request->compiler : NULL;
    XrCompileState *state = compile_state(input);
    if (!compile_input_ready(input))
        return NULL;
    if (!ast || (out_module && *out_module))
        return NULL;
    char *owned_identity = NULL;
    const char *canonical = request->canonical;
    if (!canonical) {
        if (!build_compile_unit_identity(input, request->authority, request->source_file, &owned_identity))
            return NULL;
        canonical = owned_identity;
    }
    if (!xr_module_identity_valid(canonical, NULL)) {
        xr_compile_resources_free(owned_identity);
        return NULL;
    }
    XrCompilerInput local = *input;
    local.analysis.module_identity = canonical;
    ensure_compiler_proto_hooks();
    XrCompilerSessionScope scope = {0};
    if (ast->type != AST_PROGRAM || !ast->as.program.arena ||
        xr_compile_session_push_arena(local.analysis.session, ast->as.program.arena, &scope) != XR_COMPILE_RESOURCE_OK) {
        xr_compile_resources_free(owned_identity);
        return NULL;
    }
    XrTypePool *previous_pool = xr_type_get_current_pool();
    XrCompilerContext *context = request->graph && request->graph->analyzer
        ? xr_compiler_context_new_with_analyzer(request->graph->analyzer, &local.policy)
        : xr_compiler_context_new(&local);
    XrProto *proto = NULL;
    XiModule *module = NULL;
    if (context) {
        context->source_file = request->source_file;
        context->source_content = request->source_content;
        if (request->graph) {
            context->module_graph = request->graph->graph;
            context->graph_modules = request->graph->modules;
            context->graph_module_count = request->graph->module_count;
        }
        xa_analyzer_set_graph(context->analyzer, request->graph ? request->graph->graph : NULL);
        proto = xr_compile(context, ast);
        if (proto && !xr_entry_plan_derive(proto)) {
            xr_instruction_unit_free(proto);
            proto = NULL;
        }
        if (proto && out_module) {
            XiFunc *function = (XiFunc *) proto->xi_func;
            module = function ? function->module : NULL;
            if (!module) {
                xr_instruction_unit_free(proto);
                proto = NULL;
            }
        }
        xr_compiler_context_free(context);
    }
    xr_type_set_current_pool(previous_pool, NULL);
    (void) xr_compile_session_pop_arena(&scope);
    xr_compile_resources_free(owned_identity);
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK && proto) {
        xr_instruction_unit_free(proto);
        proto = NULL;
    }
    if (proto && out_module)
        *out_module = module;
    else if (out_module)
        *out_module = NULL;
    return proto;
}

XrProto *xr_compile_ast_with_source(const XrCompilerInput *input, AstNode *ast,
    const char *source_file, const XrModuleIdentityAuthority *authority) {
    XrCompileUnitRequest request = {.compiler = input, .source_file = source_file, .authority = authority};
    return compile_ast_internal(&request, ast, NULL);
}

XrProto *xr_compile_ast_in_graph(const XrCompileGraphInput *input, int spec_index, XiModule **out_module) {
    if (!input || !compile_input_ready(input->compiler))
        return NULL;
    XrCompileState *state = compile_state(input->compiler);
    if (!input->graph || !input->modules || !out_module || *out_module ||
        input->module_count != input->graph->topo_count || spec_index < 0 || spec_index >= input->graph->spec_count ||
        (input->analyzer && input->analyzer->compiler_session != input->compiler->analysis.session) ||
        (!input->analyzer && (input->graph->topo_count != 1 || spec_index != input->graph->entry_index)) ||
        input->graph->compiler_session != input->compiler->analysis.session ||
        input->graph->resources != xr_compile_state_resources(state))
        return NULL;
    const XrModuleSpec *spec = &input->graph->specs[spec_index];
    XrCompileUnitRequest request = {.compiler = input->compiler, .graph = input,
        .source_file = spec->source_path ? spec->source_path : spec->canonical,
        .authority = &spec->authority, .canonical = spec->canonical};
    return compile_ast_internal(&request, spec->ast, out_module);
}

void xr_compiled_module_graph_dispose(XrCompiledModuleGraph *compilation) {
    if (!compilation)
        return;
    for (int i = 0; i < compilation->count; i++)
        if (compilation->units && compilation->units[i])
            xr_instruction_unit_free(compilation->units[i]);
    xr_compile_state_free(compilation->units);
    xr_compile_state_free(compilation->modules);
    memset(compilation, 0, sizeof(*compilation));
}

static bool specialize_module_graph(const XrCompileGraphInput *input) {
    XrModuleGraph *graph = input->graph;
    XaAnalyzer *analyzer = input->analyzer;
    XrCompileState *state = compile_state(input->compiler);
    AstNode **roots = NULL;
    if (xr_compile_state_calloc(state, (size_t) graph->topo_count, sizeof(*roots), (void **) &roots) != XR_COMPILE_RESOURCE_OK)
        return false;
    int count = 0;
    for (int i = 0; i < graph->topo_count; i++) {
        int index = graph->topo_order[i];
        if (index < 0 || index >= graph->spec_count) {
            xr_compile_state_free(roots);
            return false;
        }
        if (graph->specs[index].ast)
            roots[count++] = graph->specs[index].ast;
    }
    XaMonoBudget budget = xa_mono_default_budget();
    XaMonoUsage usage = {0};
    bool specialized = xa_mono_graph_pass(roots, count, input->compiler->analysis.runtime, &budget, &usage, analyzer);
    xr_compile_state_free(roots);
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK)
        return false;
    int errors = xa_analyzer_print_errors(analyzer, NULL);
    xa_analyzer_clear_diagnostics(analyzer);
    if (!specialized || errors)
        return false;
    for (int i = 0; i < graph->topo_count; i++) {
        XrModuleSpec *spec = &graph->specs[graph->topo_order[i]];
        if (!spec->ast)
            continue;
        const char *file = spec->source_path ? spec->source_path : spec->canonical;
        xa_analyzer_analyze(analyzer, file, spec->ast);
        if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK)
            return false;
        errors = xa_analyzer_print_errors(analyzer, file);
        XrHashMap *exports = NULL;
        if (!errors && !xa_analyzer_collect_export_symbols_checked(analyzer, spec->ast, &exports))
            errors = 1;
        if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK)
            return false;
        if (!errors && spec->export_symbols != exports) {
            xr_hashmap_owned_free(spec->export_symbols);
            spec->export_symbols = exports;
        }
        xa_analyzer_clear_diagnostics(analyzer);
        if (errors)
            return false;
    }
    return true;
}

bool xr_compile_module_graph_dependencies(const XrCompileGraphInput *input, XrCompiledModuleGraph *output) {
    if (!input || !compile_input_ready(input->compiler))
        return false;
    XrCompileState *state = compile_state(input->compiler);
    if (!output || output->modules || output->units || output->count || !input->analyzer || !input->graph ||
        input->graph->compiler_session != input->compiler->analysis.session ||
        input->analyzer->compiler_session != input->compiler->analysis.session ||
        input->graph->resources != xr_compile_state_resources(state) ||
        input->graph->topo_count <= 0 || !input->graph->topo_order)
        return false;
    xa_analyzer_set_graph(input->analyzer, input->graph);
    if (!specialize_module_graph(input))
        return false;
    XrCompiledModuleGraph staged = {0};
    staged.count = input->graph->topo_count;
    if (xr_compile_state_calloc(state, (size_t) staged.count, sizeof(*staged.modules), (void **) &staged.modules) != XR_COMPILE_RESOURCE_OK ||
        xr_compile_state_calloc(state, (size_t) staged.count, sizeof(*staged.units), (void **) &staged.units) != XR_COMPILE_RESOURCE_OK)
        goto failure;
    XrCompileGraphInput current = *input;
    current.modules = staged.modules;
    current.module_count = staged.count;
    for (int i = 0; i < staged.count; i++) {
        int index = input->graph->topo_order[i];
        const XrModuleSpec *spec = &input->graph->specs[index];
        if (index == input->graph->entry_index || !spec->ast)
            continue;
        staged.units[i] = xr_compile_ast_in_graph(&current, index, &staged.modules[i]);
        if (!staged.units[i])
            goto failure;
    }
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK)
        goto failure;
    *output = staged;
    return true;
failure:
    xr_compiled_module_graph_dispose(&staged);
    return false;
}

bool xr_program_image_build(XrVMRuntime *X, const XrModuleGraph *graph,
                            const XrCompiledModuleGraph *compilation, XrProgramImage *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!X || !graph || !compilation || graph->topo_count <= 0 || !graph->topo_order ||
        compilation->count != graph->topo_count)
        return false;
    out->modules =
        (XrBytecodeModule *) xr_calloc((size_t) graph->topo_count, sizeof(*out->modules));
    if (!out->modules)
        return false;
    out->count = (size_t) graph->topo_count;
    for (int ti = 0; ti < graph->topo_count; ti++) {
        int index = graph->topo_order[ti];
        const XrModuleSpec *spec = &graph->specs[index];
        XrBytecodeModule *module = &out->modules[ti];
        /* An in-memory entry has no import name; it keeps its canonical
         * identity, which nothing imports. */
        const char *name = xr_module_spec_import_name(spec);
        if (!name)
            name = spec->canonical;
        if (!name)
            goto failure;
        module->path = xr_strdup(name);
        if (!module->path)
            goto failure;
        XrProto *unit = index == graph->entry_index ? NULL : compilation->units[ti];
        if (!unit)
            continue;
        size_t size = 0;
        uint8_t *bytecode =
            spec->kind == XR_MOD_STDLIB
                ? xr_bootstrap_container_write_stdlib(X, name, unit, 0, &size, NULL)
                : xr_bootstrap_container_write(X, unit, 0, &size, NULL);
        if (!bytecode)
            goto failure;
        module->bytecode = bytecode;
        module->bytecode_size = size;
    }
    return true;

failure:
    xr_program_image_dispose(out);
    return false;
}

void xr_program_image_install(XrProgramImage *image, XrModuleRegistry *registry) {
    if (!image || !registry || image->registry)
        return;
    image->registry = registry;
    image->previous_modules = registry->embedded_modules;
    image->previous_count = registry->embedded_module_count;
    registry->embedded_modules = image->modules;
    registry->embedded_module_count = image->count;
}

void xr_program_image_dispose(XrProgramImage *image) {
    if (!image)
        return;
    if (image->registry && image->registry->embedded_modules == image->modules) {
        image->registry->embedded_modules = image->previous_modules;
        image->registry->embedded_module_count = image->previous_count;
    }
    for (size_t i = 0; image->modules && i < image->count; i++) {
        xr_free((void *) image->modules[i].path);
        xr_free((void *) image->modules[i].bytecode);
    }
    xr_free(image->modules);
    memset(image, 0, sizeof(*image));
}

XrProto *xr_compile_source_with_path(const XrCompilerInput *input, const char *source,
    const char *source_file, const XrModuleIdentityAuthority *authority) {
    if (!compile_input_ready(input) || !source)
        return NULL;
    AstNode *ast = NULL;
    if (xr_compile_parse(input->analysis.session, source, &ast) != XR_PARSE_OK)
        return NULL;
    XrCompileUnitRequest request = {.compiler = input, .source_file = source_file,
        .source_content = source, .authority = authority};
    XrProto *proto = compile_ast_internal(&request, ast, NULL);
    xr_program_destroy(ast);
    return proto;
}
