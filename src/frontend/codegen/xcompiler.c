/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompiler.c - AST-to-bytecode compiler entry point
 *
 * KEY CONCEPT:
 *   Runs the analysis pipeline (type inference, monomorphization,
 *   escape analysis) then delegates to the Xi IR pipeline for
 *   lowering, verification, optimization, and bytecode emission.
 */

#include "xcompiler.h"
#include "xcompiler_context.h"
#include "../../analysis/xglobal_producer.h"
#include "../../base/xchecks.h"
#include "../../base/xfileio.h"
#include "../../base/xmalloc.h"
#include "../../ir/xi_pipeline.h"
#include "../../module/xmodule_graph.h"
#include "../../toolchain/xcompiler_session.h"
#include "../analyzer/xanalyzer_mono.h"
#include "../analyzer/xanalyzer_escape.h"
#include "../xdiag_fmt.h"
#include "../parser/xast_api.h"
#include "../parser/xast_types.h"
#include "../parser/xparse_internal.h"
#include "../../runtime/xisolate_api.h"
#include <stdio.h>
#include <string.h>

static uint32_t compiler_graph_module_id(const XrModuleGraph *graph, const AstNode *ast,
                                         const char *source_file) {
    uint32_t match = XG_NO_ID;
    char *source_path = source_file ? xr_realpath(source_file) : NULL;
    if (!graph || !ast || !graph->topo_order || !source_path) {
        xr_free(source_path);
        return XG_NO_ID;
    }
    for (int ti = 0; ti < graph->topo_count; ti++) {
        int index = graph->topo_order[ti];
        const XrModuleSpec *spec = &graph->specs[index];
        /* The graph owns the exact analyzed AST used to produce its evidence.
         * Imports may reparse that module before bytecode generation, so the
         * canonical graph source path is the durable identity in that case. */
        char *spec_path = spec->source_path ? xr_realpath(spec->source_path) : NULL;
        bool same_module = spec->ast == ast ||
                           (spec_path && strcmp(spec_path, source_path) == 0);
        xr_free(spec_path);
        if (!same_module)
            continue;
        if (match != XG_NO_ID) {
            xr_free(source_path);
            return XG_NO_ID;
        }
        match = (uint32_t) (ti + 1);
    }
    xr_free(source_path);
    return match;
}

/* Print and mark every unreported analyzer diagnostic; return the error count.
 * Called after each analysis stage that can produce user-visible errors --
 * including monomorphization, whose budget diagnostics arrive after the first
 * analysis pass and would otherwise be marked reported without being shown. */
static int drain_analyzer_diagnostics(XrCompilerContext *ctx) {
    if (!ctx->analyzer)
        return 0;
    int diag_count = 0;
    XaDiagnostic *diagnostics = xa_analyzer_get_diagnostics(ctx->analyzer, &diag_count);
    if (diag_count == 0)
        return 0;

    int error_count = 0;
    int warning_count = 0;
    for (XaDiagnostic *d = diagnostics; d; d = d->next) {
        if (d->code == 0 || d->reported)
            continue;
        /* REPL mode: suppress analyzer diagnostics — analyzer cannot see
         * cross-compilation-unit shared variables seeded from prior inputs
         * and would produce false-positive undefined/unused warnings. */
        if (ctx->repl_mode) {
            d->reported = true;
            continue;
        }
        const char *file = d->location.file ? d->location.file : ctx->source_file;
        int col = d->location.column > 0 ? d->location.column : 1;
        if (d->severity == XR_DIAG_SEV_ERROR) {
            error_count++;
            xr_diag_print(XR_DIAG_ERROR, d->code, d->message, file, d->location.line, col, 0, NULL,
                          NULL);
            d->reported = true;
        } else if (d->severity == XR_DIAG_SEV_WARNING) {
            warning_count++;
            xr_diag_print(XR_DIAG_WARNING, d->code, d->message, file, d->location.line, col, 0,
                          NULL, NULL);
            d->reported = true;
        }
    }
    if (error_count > 0)
        xr_diag_print_summary(ctx->source_file, error_count, warning_count, 0);
    return error_count;
}

/* Compile AST to bytecode via Xi IR pipeline.
 * Returns XrProto on success, NULL on failure.
 *
 * Prelude types `Result` and `Ordering` are NOT injected per program.  They
 * are registered once per isolate as canonical XrEnumType values bound to VM
 * builtin slots (see xr_prelude_register_builtin_enums) and seeded into the
 * analyzer's global scope, so every compilation unit — entry file and imported
 * modules alike — shares a single type identity.  Per-module AST injection
 * used to create a distinct enum type per module, which broke cross-module
 * pattern matching on `Result`. */
XR_FUNC XrProto *xr_compile(XrCompilerContext *ctx, AstNode *ast) {
    XR_DCHECK(ctx != NULL, "xr_compile: NULL context");
    XR_DCHECK(ast != NULL, "xr_compile: NULL ast");

    /* Save initial global variable offset (for module compilation) */
    int initial_global_offset = ctx->global_var_count;

    for (int i = 0; i < MAX_GLOBALS; i++) {
        ctx->global_vars[i].name = NULL;
        ctx->global_vars[i].index = -1;
    }

    ctx->global_var_count = initial_global_offset;

    /* Type inference pass */
    if (ctx->analyzer) {
        xa_analyzer_analyze(ctx->analyzer, ctx->source_file, ast);
        if (drain_analyzer_diagnostics(ctx) > 0)
            return NULL;
    }

    /* Interactive frontends may elaborate syntax that depends on inferred
     * types after the first analysis pass. The ordinary second analysis below
     * then treats the elaborated AST as the sole lowering input. */
    if (ctx->post_analyze_hook)
        ctx->post_analyze_hook(ctx, ast, ctx->post_analyze_user_data);

    /* Monomorphization: clone generic functions/structs for each concrete type */
    bool mono_ok = xa_mono_pass(ast, NULL, 0, ctx->X, ctx->analyzer);

    /* Drain before the blanket mark-as-reported below, which would otherwise
     * swallow an E0387/E0388 without ever printing it. */
    if (!mono_ok) {
        drain_analyzer_diagnostics(ctx);
        return NULL;
    }

    /* Post-mono: re-analyze monomorphized declarations for struct layouts */
    if (ctx->analyzer) {
        xa_analyzer_analyze(ctx->analyzer, ctx->source_file, ast);
        if (drain_analyzer_diagnostics(ctx) > 0)
            return NULL;
    }

    /* Mark all diagnostics as reported before escape analysis */
    if (ctx->analyzer) {
        int pre_diag_count = 0;
        XaDiagnostic *pre = xa_analyzer_get_diagnostics(ctx->analyzer, &pre_diag_count);
        for (XaDiagnostic *d = pre; d; d = d->next) {
            d->reported = true;
        }
    }

    /* Escape analysis: enforce explicit sharing rules for go closures */
    xa_escape_analyze(ast, ctx->analyzer);

    /* Report escape analysis diagnostics */
    if (ctx->analyzer) {
        int post_diag_count = 0;
        XaDiagnostic *post_diagnostics =
            xa_analyzer_get_diagnostics(ctx->analyzer, &post_diag_count);
        int post_error_count = 0;
        int post_warning_count = 0;
        for (XaDiagnostic *d = post_diagnostics; d; d = d->next) {
            if (d->code == 0)
                continue;
            if (d->reported)
                continue;
            const char *file = d->location.file ? d->location.file : ctx->source_file;
            int col = d->location.column > 0 ? d->location.column : 1;
            if (d->severity == XR_DIAG_SEV_ERROR) {
                post_error_count++;
                xr_diag_print(XR_DIAG_ERROR, d->code, d->message, file, d->location.line, col, 0,
                              NULL, NULL);
                d->reported = true;
            } else if (d->severity == XR_DIAG_SEV_WARNING) {
                post_warning_count++;
                xr_diag_print(XR_DIAG_WARNING, d->code, d->message, file, d->location.line, col, 0,
                              NULL, NULL);
                d->reported = true;
            }
        }
        if (post_error_count > 0) {
            xr_diag_print_summary(ctx->source_file, post_error_count, post_warning_count, 0);
            return NULL;
        }
    }

    /* Xi IR pipeline: single compilation path (no legacy fallback) */
    {
        XiPipelineConfig pipe_cfg = xi_pipeline_default_config();
        XgGlobalEvidence global_evidence;
        bool global_evidence_initialized = false;
        memset(&global_evidence, 0, sizeof(global_evidence));
        /* REPL mode: top-level bindings go through XrGlobalDict
         * (name-keyed) instead of the slot-indexed shared array. */
        pipe_cfg.repl_mode = ctx->repl_mode;
        pipe_cfg.source_file = ctx->source_file;
        pipe_cfg.module_graph = ctx->module_graph;
        pipe_cfg.graph_modules = ctx->graph_modules;
        pipe_cfg.graph_module_count = ctx->graph_module_count;
        const XrModuleGraph *evidence_graph =
            ctx->module_graph ? ctx->module_graph
                              : xr_compiler_session_module_graph(ctx->compiler_session);
        if (evidence_graph) {
            uint32_t module_id =
                compiler_graph_module_id(evidence_graph, ast, ctx->source_file);
            if (module_id == XG_NO_ID ||
                !xg_global_evidence_build_from_module_graph_with_imported_modules_and_analyzer(
                    &global_evidence, evidence_graph, XG_BUILD_DEV, 0, NULL, 0,
                    ctx->analyzer)) {
                xg_global_evidence_free(&global_evidence);
                fprintf(stderr,
                        "[xcompiler] global evidence graph identity failed for '%s'\n",
                        ctx->source_file ? ctx->source_file : "<unknown>");
                return NULL;
            }
            global_evidence_initialized = true;
            pipe_cfg.global_evidence = &global_evidence;
            pipe_cfg.global_evidence_module_id = module_id;
        }
        XiPipelineResult pipe_res =
            xi_pipeline_compile_program(ast, ctx->analyzer, ctx->X, &pipe_cfg);
        if (global_evidence_initialized)
            xg_global_evidence_free(&global_evidence);
        if (pipe_res.status == XI_PIPE_OK && pipe_res.proto != NULL) {
            XrProto *proto = pipe_res.proto;
            xi_pipeline_result_free(&pipe_res);
            return proto;
        }
        /* Render the structured root error exactly once. */
        fprintf(stderr, "[xcompiler] Xi IR pipeline failed at %s: %s",
                xi_pipeline_stage_str(pipe_res.error.stage), pipe_res.error.detail);
        if (pipe_res.error.func)
            fprintf(stderr, " (func=%s)", pipe_res.error.func->name);
        if (pipe_res.error.value)
            fprintf(stderr, " (v%u)", pipe_res.error.value->id);
        fprintf(stderr, "\n");
        xi_pipeline_result_free(&pipe_res);
        return NULL;
    }
}
