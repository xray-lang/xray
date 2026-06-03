/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_pipeline.c - Unified Xi IR compilation pipeline
 *
 * Orchestrates: AST -> canon -> xi_lower -> xi_verify -> xi_opt -> xi_emit -> XrProto
 */

#include "xi_pipeline.h"
#include "xi_lower.h"
#include "xi_verify.h"
#include "xi_opt.h"
#include "xi_pass.h"
#include "xi_emit.h"
#include "xi_backend_lower.h"
#include "xi_escape.h"
#include "xi_own.h"
#include "xi_arc.h"
#include "xi_reuse.h"
#include "../frontend/canonical/xcanon.h"
#include "../frontend/parser/xast.h"
#include "../runtime/xisolate_api.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Debug helper: dump ownership analysis for a function and all its nested
 * children (methods, closures). Gated by XRAY_XI_OWN_DUMP=1. */
static void xi_own_dump_recursive(XiFunc *f) {
    if (!f)
        return;
    XiOwnResult own;
    if (xi_own_analyze(f, &own)) {
        xi_own_dump(f, &own);
        xi_own_free(&own);
    }
    for (uint16_t i = 0; i < f->nchildren; i++)
        xi_own_dump_recursive(f->children[i]);
}

/* ========== Configuration ========== */

XR_FUNC XiPipelineConfig xi_pipeline_default_config(void) {
    XiPipelineConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = XI_PIPE_VM;
    cfg.run_verify = true;
    cfg.run_optimize = true;
    cfg.opt_level = XI_OPT_LIGHT;
    cfg.run_select_rep = false;
    /* The VM runs escape analysis + precise dup/drop insertion (xi_arc) but
     * NOT stack_alloc_rewrite (no XI_STACK_ALLOC handler in the VM emitter)
     * and NOT backend_lower. dup/drop execute as OP_DUP/OP_DROP. */
    cfg.run_escape = true;
    cfg.run_arc = true;
    cfg.run_emit = true;
    cfg.dump_ir_before = false;
    cfg.dump_ir_after = false;
    cfg.budget_ns = XI_BUDGET_JIT_TIER1_NS;
    return cfg;
}

XR_FUNC XiPipelineConfig xi_pipeline_aot_config(void) {
    XiPipelineConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = XI_PIPE_AOT;
    cfg.run_verify = true;
    cfg.run_optimize = true;
    cfg.opt_level = XI_OPT_FULL;
    cfg.run_select_rep = true;
    cfg.run_backend_lower = true;
    cfg.run_escape = true;
    cfg.run_arc = true;
    cfg.run_emit = false;
    cfg.dump_ir_before = false;
    cfg.dump_ir_after = false;
    return cfg;
}

/* ========== Internal Pipeline ========== */

static XiPipelineResult run_pipeline(XiFunc *ir, struct XrayIsolate *X,
                                     const XiPipelineConfig *cfg) {
    XiPipelineResult res;
    memset(&res, 0, sizeof(res));
    res.ir = ir;

    if (!ir) {
        res.status = XI_PIPE_ERR_LOWER;
        res.error_msg = "lowering returned NULL";
        return res;
    }

    /* Closure pass: build XiClosureMeta, assign env layout and cell indices.
     * Advances stage to XI_STAGE_CLOSED. */
    xi_pass_close(ir);

    /* Optional: dump IR before optimization */
    if (cfg->dump_ir_before) {
        fprintf(stderr, "=== Xi IR (before optimization) ===\n");
        xi_func_dump(ir, stderr);
        fprintf(stderr, "===================================\n");
    }

    /* Verification pass (catches lowering bugs early) */
    if (cfg->run_verify) {
        char errbuf[512];
        if (!xi_verify(ir, errbuf, sizeof(errbuf))) {
            res.status = XI_PIPE_ERR_VERIFY;
            res.error_msg = "IR verification failed";
            fprintf(stderr, "[xi_pipeline] verify error: %s\n", errbuf);
            return res;
        }
    }

    /* Optimization passes (pipeline driver handles per-round verify) */
    if (cfg->run_optimize) {
        XiOptLevel level = cfg->opt_level;
        if (level == XI_OPT_NONE)
            level = XI_OPT_LIGHT;

        XiPipelineStats stats;
        xi_opt_run_pipeline_ex(ir, level, &stats, cfg->budget_ns);

        /* Optional dump: XRAY_XI_STATS=1 prints per-function stats */
        const char *env = getenv("XRAY_XI_STATS");
        if (env && env[0] == '1') {
            xi_pipeline_stats_dump(&stats, ir->name);
        }
    }

    /* Escape analysis: compute escape levels for heap-allocating values.
     * Run after optimization (dead code eliminated) but before select_rep
     * so escape info is available when inserting BOX/UNBOX. */
    if (cfg->run_escape) {
        xi_escape_analyze(ir);
    }

    /* Backward ownership inference (analysis only — does not mutate IR).
     * Gated behind XRAY_XI_OWN_DUMP=1 for manual verification of dup/drop
     * decisions; the xi_arc rewrite consumes these annotations directly. */
    {
        const char *own_env = getenv("XRAY_XI_OWN_DUMP");
        if (own_env && own_env[0] == '1') {
            xi_own_dump_recursive(ir);
        }
    }

    /* Stack alloc rewrite: replace NO_ESCAPE heap allocs with XI_STACK_ALLOC.
     * Must run after escape analysis and before ARC insertion (STACK_ALLOC
     * values don't need retain/release since they have frame lifetime).
     * Gated on run_backend_lower because only backend codegen (AOT/JIT)
     * consumes XI_STACK_ALLOC; the VM emitter has no handler for it. */
    if (cfg->run_escape && cfg->run_backend_lower) {
        xi_stack_alloc_rewrite(ir);
    }

    /* Precise dup/drop insertion (consumes ownership analysis).
     * MUST run BEFORE backend lowering, while ops are still semantic
     * (STORE_FIELD/ARRAY_NEW/...) — the owned/borrow split is keyed on
     * those ops. Gated on run_arc (independent of run_backend_lower) so the
     * VM can get dup/drop without stack_alloc_rewrite. */
    if (cfg->run_escape && cfg->run_arc) {
        xi_arc_insert(ir);
        /* Drop-Reuse: convert RELEASE+ALLOC pairs to DROP_REUSE+ALLOC_AT
         * for in-place memory reuse. Only for JIT/AOT (the VM emitter
         * does not handle these ops — it uses OP_DUP/OP_DROP directly). */
        if (cfg->run_backend_lower)
            xi_reuse_insert(ir);
        xi_func_set_stage_recursive(ir, XI_STAGE_OWNED);
    }

    /* SelectRepresentations: insert BOX/UNBOX at representation boundaries.
     * Run after general optimization so constants/copies are resolved first. */
    if (cfg->run_select_rep) {
        xi_opt_select_rep(ir);
        xi_opt_box_elim(ir);
#ifndef NDEBUG
        if (cfg->run_verify) {
            char rep_errbuf[512];
            if (!xi_verify(ir, rep_errbuf, sizeof(rep_errbuf)))
                fprintf(stderr, "[xi_pipeline] post-select_rep verify: %s\n", rep_errbuf);
            XR_DCHECK(xi_verify(ir, rep_errbuf, sizeof(rep_errbuf)),
                      "post-select_rep verify failed");
        }
#endif
    }

    /* Backend lowering: rewrite high-level ops to XI_CALL_BUILTIN.
     * Advances stage to STAGE_BACKEND. */
    if (cfg->run_backend_lower) {
        xi_backend_lower(ir);
#ifndef NDEBUG
        if (cfg->run_verify) {
            char be_errbuf[512];
            if (!xi_verify(ir, be_errbuf, sizeof(be_errbuf)))
                fprintf(stderr, "[xi_pipeline] post-backend_lower verify: %s\n", be_errbuf);
            XR_DCHECK(xi_verify(ir, be_errbuf, sizeof(be_errbuf)),
                      "post-backend_lower verify failed");
        }
#endif
    }

    /* Optional: dump IR after optimization */
    if (cfg->dump_ir_after) {
        fprintf(stderr, "=== Xi IR (after optimization) ===\n");
        xi_func_dump(ir, stderr);
        fprintf(stderr, "==================================\n");
    }

    /* Bytecode emission (skipped in AOT/CHECK mode) */
    if (cfg->run_emit) {
        struct XrProto *proto = NULL;
        XiEmitStatus emit_st = xi_emit(ir, X, &proto);
        if (emit_st != XI_EMIT_OK) {
            res.status = XI_PIPE_ERR_EMIT;
            res.error_msg = xi_emit_status_str(emit_st);
            return res;
        }
        res.proto = proto;
        /* Transfer Xi IR ownership to proto for JIT direct lowering.
         * Null res.ir so xi_pipeline_result_free won't double-free. */
        xi_emit_attach_ir(proto, ir);
        res.ir = NULL;
    }

    res.status = XI_PIPE_OK;
    return res;
}

/* ========== Public API ========== */

XR_FUNC XiPipelineResult xi_pipeline_compile_func(struct AstNode *func_node,
                                                  struct XaAnalyzer *analyzer,
                                                  struct XrayIsolate *isolate,
                                                  const XiPipelineConfig *cfg) {
    XR_DCHECK(func_node != NULL, "xi_pipeline_compile_func: NULL func_node");

    XiPipelineConfig default_cfg;
    if (!cfg) {
        default_cfg = xi_pipeline_default_config();
        cfg = &default_cfg;
    }

    /* Re-install the parse arena so canonicalizer can allocate new AST nodes. */
    struct XrArena *saved_arena = xr_isolate_get_current_arena(isolate);
    if (func_node->as.function_decl.body && func_node->as.function_decl.body->type == AST_BLOCK) {
        /* Function bodies don't own arenas directly; the arena lives on
         * the enclosing program node.  The caller (xvm_compile.c) already
         * re-installs the program arena before entering the pipeline, so
         * the arena should already be set. */
    }

    /* Canonicalize AST before lowering */
    xr_canon_func(func_node, analyzer, isolate);

    /* Restore previous arena */
    xr_isolate_set_current_arena(isolate, saved_arena);

    XiFunc *ir = xi_lower_func(func_node, analyzer, isolate);

    /* Canonicalization guarantees: advance stage and invariant mask
     * for the root and all nested child functions. */
    if (ir) {
        xi_func_set_stage_recursive(ir, XI_STAGE_CANONICAL);
    }

    return run_pipeline(ir, isolate, cfg);
}

XR_FUNC XiPipelineResult xi_pipeline_compile_program(struct AstNode *program_node,
                                                     struct XaAnalyzer *analyzer,
                                                     struct XrayIsolate *isolate,
                                                     const XiPipelineConfig *cfg) {
    XR_DCHECK(program_node != NULL, "xi_pipeline_compile_program: NULL program_node");

    XiPipelineConfig default_cfg;
    if (!cfg) {
        default_cfg = xi_pipeline_default_config();
        cfg = &default_cfg;
    }

    /* Re-install the parse arena so canonicalizer can allocate new AST nodes
     * (temp variables, desugared expressions, synthetic blocks). */
    struct XrArena *saved_arena = xr_isolate_get_current_arena(isolate);
    if (program_node->type == AST_PROGRAM && program_node->as.program.arena) {
        xr_isolate_set_current_arena(isolate, program_node->as.program.arena);
    }

    /* Canonicalize AST before lowering */
    xr_canon_program(program_node, analyzer, isolate);

    /* Restore previous arena */
    xr_isolate_set_current_arena(isolate, saved_arena);

    XiFunc *ir = xi_lower_program_ex(program_node, analyzer, isolate, cfg->repl_mode);

    /* Canonicalization guarantees: advance stage and invariant mask
     * for the root and all nested child functions. */
    if (ir) {
        xi_func_set_stage_recursive(ir, XI_STAGE_CANONICAL);
    }

    return run_pipeline(ir, isolate, cfg);
}

XR_FUNC struct XrProto *xi_pipeline_emit_ir(XiFunc *ir, struct XrayIsolate *isolate) {
    XR_DCHECK(ir != NULL, "xi_pipeline_emit_ir: NULL ir");
    XR_DCHECK(isolate != NULL, "xi_pipeline_emit_ir: NULL isolate");

    struct XrProto *proto = NULL;
    XiEmitStatus emit_st = xi_emit(ir, isolate, &proto);
    if (emit_st != XI_EMIT_OK) {
        fprintf(stderr, "[xi_pipeline] emit_ir failed: %s\n", xi_emit_status_str(emit_st));
        return NULL;
    }

    /* Transfer IR ownership to proto for JIT direct lowering */
    xi_emit_attach_ir(proto, ir);
    return proto;
}

XR_FUNC void xi_pipeline_result_free(XiPipelineResult *res) {
    if (!res)
        return;
    if (res->ir) {
        xi_func_free(res->ir);
        res->ir = NULL;
    }
    if (res->module) {
        xi_module_free(res->module);
        res->module = NULL;
    }
    /* proto is NOT freed — caller owns it */
}

XR_FUNC const char *xi_pipe_status_str(XiPipeStatus s) {
    switch (s) {
        case XI_PIPE_OK:
            return "OK";
        case XI_PIPE_ERR_LOWER:
            return "AST lowering failed";
        case XI_PIPE_ERR_VERIFY:
            return "IR verification failed";
        case XI_PIPE_ERR_EMIT:
            return "bytecode emission failed";
        case XI_PIPE_ERR_INTERNAL:
            return "internal pipeline error";
    }
    return "unknown";
}
