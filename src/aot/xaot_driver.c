/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_driver.c - AOT native compilation driver (Xi IR pipeline)
 *
 * KEY CONCEPT:
 *   Full pipeline from source file to generated C program:
 *   1. Module graph discovery (topo-sorted via XrModuleGraph)
 *   2. Cross-module analysis with shared XaAnalyzer (typed imports)
 *   3. Per-module: canonicalize → Xi IR lower → optimize → select_rep
 *   4. Cross-module import resolution via export table
 *   5. C code generation via xi_cgen
 *   6. Main() generation calling module inits in topo order
 *
 * RELATED MODULES:
 *   - xi_cgen.h: Xi IR → C code generation
 *   - xaot_driver.h: public API
 *   - xcmd_build.c: CLI entry that invokes xaot_build + CC
 */

#include "xaot_driver.h"
#include "../../include/xray.h"
#include "../../include/xray_isolate.h"
#include "../runtime/xisolate_api.h"
#include "../module/xmodule_graph.h"
#include "../module/xmodule_resolver.h"
#include "../module/xmodule.h"
#include "../base/xmalloc.h"
#include "../base/xmemstream.h"
#include "../ir/xi.h"
#include "../ir/xi_pipeline.h"
#include "../ir/xi_import_resolve.h"
#include "xi_cgen.h"
#include "xi_lto.h"
#include "../frontend/analyzer/xanalyzer.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef _WIN32
#include <stdlib.h>
#define realpath(path, resolved) _fullpath((resolved), (path), PATH_MAX)
#endif

/* Create a full-runtime isolate for AOT compilation.
 * Equivalent to XR_ISOLATE_PROFILE_RUN without depending on the
 * isolate-profile factory in src/api/. */
static XrayIsolate *create_isolate(void) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    return xray_isolate_new(&params);
}

/* ========== Module Name Helpers ========== */

/* Derive a C-safe module name from absolute path.  Caller must free. */
static char *derive_module_name(const char *path) {
    XR_DCHECK(path != NULL, "derive_module_name: NULL path");
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t len = strlen(base);
    if (len > 3 && strcmp(base + len - 3, ".xr") == 0)
        len -= 3;
    char *name = (char *) xr_malloc(len + 1);
    if (!name)
        return NULL;
    for (size_t i = 0; i < len; i++)
        name[i] = (base[i] == '-' || base[i] == '.') ? '_' : base[i];
    name[len] = '\0';
    return name;
}

/* Graph-based import resolution: delegate to shared xi_import_resolve utility */

/* ========== Feature Inference ========== */

/* Map import module name to XaotStdlibSet flag.
 * Import names are bare identifiers (e.g. "math", "crypto");
 * relative paths (starting with "./") are user modules, not stdlib.
 * Json is a builtin type and not an import module. */
static XaotStdlibSet stdlib_flag_for_import(const char *name) {
    if (!name || name[0] == '.')
        return 0;

    struct {
        const char *name;
        XaotStdlibSet flag;
    } table[] = {
        {"regex", XAOT_STDLIB_REGEX},   {"math", XAOT_STDLIB_MATH},
        {"time", XAOT_STDLIB_TIME},     {"datetime", XAOT_STDLIB_TIME},
        {"path", XAOT_STDLIB_PATH},     {"io", XAOT_STDLIB_IO},
        {"os", XAOT_STDLIB_OS},         {"net", XAOT_STDLIB_NET},
        {"http", XAOT_STDLIB_HTTP},     {"crypto", XAOT_STDLIB_CRYPTO},
        {"base64", XAOT_STDLIB_BASE64}, {"csv", XAOT_STDLIB_CSV},
        {"toml", XAOT_STDLIB_TOML},     {"yaml", XAOT_STDLIB_YAML},
        {"xml", XAOT_STDLIB_XML},       {"compress", XAOT_STDLIB_COMPRESS},
    };
    for (int i = 0; i < (int) (sizeof(table) / sizeof(table[0])); i++) {
        if (strcmp(name, table[i].name) == 0)
            return table[i].flag;
    }
    return 0;
}

/* Scan a single XiFunc (non-recursive) for feature-indicating ops */
static void scan_func_features(XiFunc *f, XaotFeatureSet *fs) {
    XR_DCHECK(f != NULL, "scan_func_features: NULL func");
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;
            switch (v->op) {
                case XI_YIELD:
                    fs->need_coro = true;
                    break;
                case XI_GO:
                    fs->need_coro = true;
                    fs->need_netpoll = true;
                    break;
                case XI_CHAN_NEW:
                case XI_CHAN_SEND:
                case XI_CHAN_RECV:
                case XI_CHAN_TRY_SEND:
                case XI_CHAN_TRY_RECV:
                case XI_CHAN_IS_CLOSED:
                case XI_TIME_AFTER:
                case XI_SELECT_BLOCK:
                    fs->need_channel = true;
                    fs->need_coro = true;
                    if (v->op == XI_TIME_AFTER)
                        fs->need_timer = true;
                    break;
                case XI_SCOPE_ENTER:
                case XI_SCOPE_EXIT:
                    fs->need_scope = true;
                    fs->need_coro = true;
                    break;
                case XI_AWAIT:
                    fs->need_coro = true;
                    break;
                case XI_CALL_METHOD:
                    if (v->aux && strcmp((const char *) v->aux, "sleep") == 0 && v->nargs == 2) {
                        fs->need_coro = true;
                        fs->need_timer = true;
                    }
                    break;
                case XI_TRY:
                case XI_THROW:
                    fs->need_exception = true;
                    break;
                case XI_IS:
                    fs->need_instanceof = true;
                    break;
                case XI_IMPORT_REF: {
                    XiImportRef *ref = (XiImportRef *) v->aux;
                    if (ref && ref->module_path) {
                        XaotStdlibSet flag = stdlib_flag_for_import(ref->module_path);
                        if (flag)
                            fs->stdlib |= flag;
                        /* net/http imply netpoll runtime */
                        if (flag & (XAOT_STDLIB_NET | XAOT_STDLIB_HTTP))
                            fs->need_netpoll = true;
                        /* time implies timer subsystem */
                        if (flag & XAOT_STDLIB_TIME)
                            fs->need_timer = true;
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
}

/* Recursively infer features from an Xi IR function tree */
static void infer_features_recursive(XiFunc *f, XaotFeatureSet *fs) {
    if (!f)
        return;
    scan_func_features(f, fs);
    for (uint16_t c = 0; c < f->nchildren; c++)
        infer_features_recursive(f->children[c], fs);
}

/* Infer XaotFeatureSet for the entire compiled bundle */
static void infer_features(XiFunc **ir_funcs, int nmodules, XaotFeatureSet *fs) {
    memset(fs, 0, sizeof(*fs));
    for (int m = 0; m < nmodules; m++)
        infer_features_recursive(ir_funcs[m], fs);
}

XR_FUNC int xaot_build(const char *input_path, XaotBuildResult *result) {
    XR_DCHECK(input_path != NULL, "xaot_build: NULL input_path");
    XR_DCHECK(result != NULL, "xaot_build: NULL result");
    memset(result, 0, sizeof(*result));

    printf("[xi-native] Building: %s\n", input_path);

    /* --- Build module graph (topo order, entry last) --- */
    XrayIsolate *X = create_isolate();
    if (!X) {
        fprintf(stderr, "Error: failed to create isolate\n");
        return 1;
    }

    xr_module_system_init_with_script(X, input_path);
    XrModuleRegistry *registry = xr_isolate_get_module_registry(X);
    XrModuleResolver *resolver = xr_module_registry_get_resolver(registry);
    XrModuleGraph *graph = xr_module_graph_new(X, resolver);
    if (!graph) {
        fprintf(stderr, "Error: failed to create module graph\n");
        xray_isolate_delete(X);
        return 1;
    }

    char *build_err = NULL;
    if (xr_module_graph_build(graph, input_path, &build_err) != 0) {
        fprintf(stderr, "Error: module graph build failed: %s\n", build_err ? build_err : "?");
        xr_free(build_err);
        xr_module_graph_free(graph);
        xray_isolate_delete(X);
        return 1;
    }
    xr_free(build_err);

    xr_module_graph_topological_sort(graph);
    if (graph->has_cycle) {
        fprintf(stderr, "Error: %s\n",
                graph->cycle_desc ? graph->cycle_desc : "circular dependency detected");
        xr_module_graph_free(graph);
        xray_isolate_delete(X);
        return 1;
    }

    int nmodules = graph->topo_count;
    int entry_index = -1;

    /* Build parallel arrays for paths/names (graph topo order) */
    char **paths = (char **) xr_calloc(nmodules, sizeof(char *));
    char **mod_names = (char **) xr_calloc(nmodules, sizeof(char *));
    if (!paths || !mod_names) {
        xr_free(paths);
        xr_free(mod_names);
        xr_module_graph_free(graph);
        xray_isolate_delete(X);
        return 1;
    }
    /* Resolve input_path to canonical form (handles symlinks like /tmp -> /private/tmp) */
    char real_input[PATH_MAX];
    if (!realpath(input_path, real_input))
        strncpy(real_input, input_path, PATH_MAX - 1);

    for (int ti = 0; ti < nmodules; ti++) {
        int idx = graph->topo_order[ti];
        XrModuleSpec *spec = &graph->specs[idx];
        paths[ti] = xr_strdup(spec->source_path);
        mod_names[ti] = derive_module_name(spec->source_path);
        if (strcmp(spec->source_path, real_input) == 0)
            entry_index = ti;
    }
    /* Entry is the last module in topo order (all deps come first) */
    if (entry_index < 0)
        entry_index = nmodules - 1;

    if (nmodules > 1) {
        printf("[xi-native] %d modules (topo order):\n", nmodules);
        for (int i = 0; i < nmodules; i++)
            printf("  [%d] %s%s\n", i, paths[i], i == entry_index ? " (entry)" : "");
    }

    /* --- Analyze all modules with shared analyzer (cross-module types) --- */
    XaAnalyzer *shared_analyzer = xa_analyzer_new(X);
    if (!shared_analyzer) {
        fprintf(stderr, "Error: failed to create shared analyzer\n");
        goto fail_free_graph;
    }
    xa_analyzer_set_graph(shared_analyzer, graph);

    for (int ti = 0; ti < nmodules; ti++) {
        int idx = graph->topo_order[ti];
        XrModuleSpec *spec = &graph->specs[idx];
        if (!spec->ast || !spec->source_path)
            continue;
        xa_analyzer_analyze(shared_analyzer, spec->source_path, (XrAstNode *) spec->ast);
        spec->exports = xa_analyzer_collect_exports(shared_analyzer, (XrAstNode *) spec->ast);
    }

    /* --- Compile all modules through Xi IR pipeline --- */
    XiPipelineConfig cfg = xi_pipeline_aot_config();
    XiPipelineResult *pres_arr = (XiPipelineResult *) xr_calloc(nmodules, sizeof(XiPipelineResult));
    XiFunc **ir_funcs = (XiFunc **) xr_calloc(nmodules, sizeof(XiFunc *));
    XiModule **modules = (XiModule **) xr_calloc(nmodules, sizeof(XiModule *));
    if (!pres_arr || !ir_funcs || !modules) {
        xr_free(pres_arr);
        xr_free(ir_funcs);
        xr_free(modules);
        xa_analyzer_set_graph(shared_analyzer, NULL);
        xa_analyzer_free(shared_analyzer);
        goto fail_free_graph;
    }

    int total_funcs = 0;
    for (int ti = 0; ti < nmodules; ti++) {
        int idx = graph->topo_order[ti];
        XrModuleSpec *spec = &graph->specs[idx];
        if (!spec->ast || !spec->source_path) {
            pres_arr[ti].status = XI_PIPE_ERR_INTERNAL;
            fprintf(stderr, "Error: no AST for module '%s'\n", paths[ti]);
            goto fail_free_ir;
        }

        /* Compile using the shared analyzer (has cross-module type info) */
        pres_arr[ti] = xi_pipeline_compile_program((AstNode *) spec->ast, shared_analyzer, X, &cfg);
        if (pres_arr[ti].status != XI_PIPE_OK) {
            fprintf(stderr, "Error: Xi pipeline failed for '%s': %s\n", paths[ti],
                    xi_pipe_status_str(pres_arr[ti].status));
            if (pres_arr[ti].error_msg)
                fprintf(stderr, "  %s\n", pres_arr[ti].error_msg);
            goto fail_free_ir;
        }
        ir_funcs[ti] = pres_arr[ti].ir;
        XR_DCHECK(ir_funcs[ti] != NULL, "xaot_build: pipeline OK but NULL IR");
        total_funcs += 1 + ir_funcs[ti]->nchildren;

        XR_DCHECK(ir_funcs[ti]->module != NULL, "xaot_build: pipeline produced no module metadata");
        modules[ti] = ir_funcs[ti]->module;
        modules[ti]->path = paths[ti];
        modules[ti]->name = mod_names[ti];
        ir_funcs[ti]->module = NULL;
    }
    xa_analyzer_set_graph(shared_analyzer, NULL);
    xa_analyzer_free(shared_analyzer);
    shared_analyzer = NULL;

    /* --- Resolve XI_IMPORT_REF using graph (before graph is freed) --- */
    for (int ti = 0; ti < nmodules; ti++) {
        xi_resolve_imports(ir_funcs[ti], graph, paths[ti], modules, nmodules);
    }

    /* --- Cross-module LTO: direct-bind imported callees --- */
    {
        XiLtoContext lto;
        if (xi_lto_context_init(&lto, modules, (uint32_t) nmodules))
            (void) xi_lto_link_modules(&lto);
        xi_lto_context_free(&lto);
    }

    /* Graph ASTs must not be freed before compilation is done.
     * Now that pipeline is complete, free the graph (frees ASTs too). */
    xr_module_graph_free(graph);
    graph = NULL;
    xray_isolate_delete(X);
    X = NULL;

    /* --- Create codegen context (no global state) --- */
    XiCgenCtx *cg_ctx = xi_cgen_ctx_new();
    if (!cg_ctx) {
        fprintf(stderr, "Error: failed to create codegen context\n");
        goto fail_free_ir;
    }

    /* --- Resolve cross-module imports for C codegen --- */
    xi_cgen_resolve_module_imports(cg_ctx, modules, nmodules);

    /* --- Generate combined C source --- */
    char *buf = NULL;
    size_t bufsz = 0;
    FILE *mem = xr_open_memstream(&buf, &bufsz);
    if (!mem) {
        fprintf(stderr, "Error: xr_open_memstream failed\n");
        xi_cgen_ctx_free(cg_ctx);
        goto fail_free_ir;
    }

    if (nmodules == 1) {
        /* Single-module fast path */
        xi_cgen_program(cg_ctx, mem, modules[0]);
    } else {
        /* Multi-module: header + per-module sections + combined main */
        xi_cgen_header(mem);
        for (int m = 0; m < nmodules; m++)
            xi_cgen_module(cg_ctx, mem, modules[m]);
        xi_cgen_main(mem, modules, nmodules, entry_index);
    }
    if (xr_close_memstream(mem, &buf, &bufsz) != 0) {
        fprintf(stderr, "Error: xr_close_memstream failed\n");
        xi_cgen_ctx_free(cg_ctx);
        goto fail_free_ir;
    }
    if (xi_cgen_has_error(cg_ctx)) {
        fprintf(stderr, "Error: AOT C code generation failed\n");
        xi_cgen_ctx_free(cg_ctx);
        xr_free(buf);
        goto fail_free_ir;
    }
    XiCgenCoroFrameStats coro_frame_stats = xi_cgen_coro_frame_stats(cg_ctx);
    xi_cgen_ctx_free(cg_ctx);

    /* Infer runtime features before freeing IR */
    XaotFeatureSet features;
    infer_features(ir_funcs, nmodules, &features);

    /* Free IR and module metadata (no longer needed after C generation) */
    for (int m = 0; m < nmodules; m++) {
        xi_module_free(modules[m]);
        xi_pipeline_result_free(&pres_arr[m]);
    }
    xr_free(modules);
    xr_free(pres_arr);
    xr_free(ir_funcs);

    printf("[xi-native] Generated %zu bytes of C (%d functions, %d modules)\n", bufsz, total_funcs,
           nmodules);

    /* buf is xr_malloc-owned (xr_close_memstream guarantees this on every
     * platform). Hand it off to the caller as-is. */
    result->c_source = buf;
    result->total_compiled = total_funcs;
    result->total_aot = total_funcs;
    result->nmodules = nmodules;
    result->features = features;
    result->coro_frame_stats = coro_frame_stats;

    /* Cleanup module name arrays */
    for (int i = 0; i < nmodules; i++) {
        xr_free(paths[i]);
        xr_free(mod_names[i]);
    }
    xr_free(paths);
    xr_free(mod_names);
    return 0;

fail_free_ir:
    for (int m = 0; m < nmodules; m++) {
        if (modules)
            xi_module_free(modules[m]);
        xi_pipeline_result_free(&pres_arr[m]);
    }
    xr_free(modules);
    xr_free(pres_arr);
    xr_free(ir_funcs);
    if (shared_analyzer) {
        xa_analyzer_set_graph(shared_analyzer, NULL);
        xa_analyzer_free(shared_analyzer);
    }
fail_free_graph:
    if (graph)
        xr_module_graph_free(graph);
    if (X)
        xray_isolate_delete(X);
    for (int i = 0; i < nmodules; i++) {
        xr_free(paths[i]);
        xr_free(mod_names[i]);
    }
    xr_free(paths);
    xr_free(mod_names);
    return 1;
}
