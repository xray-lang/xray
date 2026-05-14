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
#include "../../base/xchecks.h"
#include "../../ir/xi_pipeline.h"
#include "../analyzer/xanalyzer_mono.h"
#include "../analyzer/xanalyzer_escape.h"
#include "../xdiag_fmt.h"
#include <stdio.h>

/* Compile AST to bytecode via Xi IR pipeline.
 * Returns XrProto on success, NULL on failure. */
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
        xa_analyzer_analyze(ctx->analyzer, NULL, ast);

        int diag_count = 0;
        XaDiagnostic *diagnostics = xa_analyzer_get_diagnostics(ctx->analyzer, &diag_count);
        if (diag_count > 0) {
            int error_count = 0;
            int warning_count = 0;
            for (XaDiagnostic *d = diagnostics; d; d = d->next) {
                if (d->code == 0)
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
                    xr_diag_print(XR_DIAG_ERROR, d->code, d->message, file, d->location.line, col,
                                  0, NULL, NULL);
                    d->reported = true;
                } else if (d->severity == XR_DIAG_SEV_WARNING) {
                    warning_count++;
                    xr_diag_print(XR_DIAG_WARNING, d->code, d->message, file, d->location.line, col,
                                  0, NULL, NULL);
                    d->reported = true;
                }
            }
            if (error_count > 0) {
                xr_diag_print_summary(ctx->source_file, error_count, warning_count, 0);
                return NULL;
            }
        }
    }

    /* Monomorphization: clone generic functions/structs for each concrete type */
    xa_mono_pass(ast);

    /* Post-mono: re-analyze monomorphized declarations for struct layouts */
    if (ctx->analyzer) {
        xa_analyzer_analyze(ctx->analyzer, NULL, ast);
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
        /* REPL mode: top-level bindings go through XrGlobalDict
         * (name-keyed) instead of the slot-indexed shared array.  This
         * is the single switch that activates the Phase 2 globals
         * lowering for the live REPL. */
        pipe_cfg.repl_mode = ctx->repl_mode;
        XiPipelineResult pipe_res =
            xi_pipeline_compile_program(ast, ctx->analyzer, ctx->X, &pipe_cfg);
        if (pipe_res.status == XI_PIPE_OK && pipe_res.proto != NULL) {
            XrProto *proto = pipe_res.proto;
            xi_pipeline_result_free(&pipe_res);
            return proto;
        }
        /* Pipeline failed — report and abort */
        fprintf(stderr, "[xcompiler] Xi IR pipeline failed: %s\n",
                xi_pipe_status_str(pipe_res.status));
        if (pipe_res.error_msg) {
            fprintf(stderr, "[xcompiler]   detail: %s\n", pipe_res.error_msg);
        }
        xi_pipeline_result_free(&pipe_res);
        return NULL;
    }
}
