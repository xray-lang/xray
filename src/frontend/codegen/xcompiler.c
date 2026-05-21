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
#include "../parser/xast_api.h"
#include "../parser/xast_types.h"
#include "../parser/xparse_internal.h"
#include "../../runtime/xisolate_api.h"
#include <stdio.h>
#include <string.h>

/* Inject the built-in `enum Result { Ok(value), Err(error) }` into
 * the program AST so the analyzer sees it as a user-declared type.
 * Called once before analysis; idempotent (skips if already present). */
static void inject_prelude_result_enum(XrayIsolate *X, AstNode *program) {
    XR_DCHECK(X != NULL, "inject_prelude_result_enum: NULL isolate");
    XR_DCHECK(program != NULL && program->type == AST_PROGRAM,
              "inject_prelude_result_enum: bad program");

    /* Guard: skip if Result is already declared by user source */
    for (int i = 0; i < program->as.program.count; i++) {
        AstNode *s = program->as.program.statements[i];
        if (s && s->type == AST_ENUM_DECL && s->as.enum_decl.name &&
            strcmp(s->as.enum_decl.name, "Result") == 0)
            return;
    }

    /* Ok(value) variant — single payload */
    AstNode *ok_member = xr_ast_enum_member(X, "Ok", NULL, NULL, NULL, 1, 0);
    /* Err(error) variant — single payload */
    AstNode *err_member = xr_ast_enum_member(X, "Err", NULL, NULL, NULL, 1, 0);

    AstNode **members = (AstNode **) ast_alloc_array(X, sizeof(AstNode *), 2);
    members[0] = ok_member;
    members[1] = err_member;

    AstNode *decl = xr_ast_enum_decl(X, "Result", NULL, members, 2, NULL, 0, NULL, 0, NULL, 0, 0);

    /* Prepend: the enum must execute before user code so its runtime
     * type object exists by the time user expressions reference it. */
    xr_ast_program_add(X, program, NULL); /* grow capacity */
    int n = program->as.program.count - 1;
    for (int i = n; i > 0; i--)
        program->as.program.statements[i] = program->as.program.statements[i - 1];
    program->as.program.statements[0] = decl;
}

/* Compile AST to bytecode via Xi IR pipeline.
 * Returns XrProto on success, NULL on failure. */
XR_FUNC XrProto *xr_compile(XrCompilerContext *ctx, AstNode *ast) {
    XR_DCHECK(ctx != NULL, "xr_compile: NULL context");
    XR_DCHECK(ast != NULL, "xr_compile: NULL ast");

    /* Inject prelude types into the AST before analysis.
     * Must activate the parse arena so synthetic AST nodes land there. */
    {
        struct XrArena *saved_arena = xr_isolate_get_current_arena(ctx->X);
        if (ast->type == AST_PROGRAM && ast->as.program.arena)
            xr_isolate_set_current_arena(ctx->X, ast->as.program.arena);
        inject_prelude_result_enum(ctx->X, ast);
        xr_isolate_set_current_arena(ctx->X, saved_arena);
    }

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
         * (name-keyed) instead of the slot-indexed shared array. */
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
