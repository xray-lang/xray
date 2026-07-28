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
#include "xi_semantic_snapshot.h"
#include "../frontend/analyzer/xa_typed_program.h"
#include "xi_lower.h"
#include "xi_verify.h"
#include "xi_opt.h"
#include "xi_pass.h"
#include "xi_emit.h"
#include "xi_backend_lower.h"
#include "xi_escape.h"
#include "xi_own.h"
#include "xi_arc.h"
#include "xi_arc_verify.h"
#include "xi_source_move_verify.h"
#include "xi_stage.h"
#include "../frontend/canonical/xcanon.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../frontend/parser/xast.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/value/xchunk.h"
#include "../toolchain/xcompiler_session.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static bool xi_env_is_enabled(const char *name) {
    const char *env = getenv(name);
    return env && env[0] == '1' && env[1] == '\0';
}

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

/* Sum RC ops that survive xi_arc_elim across a function tree. This is the
 * backend-independent static dup/drop budget consumed by regression gates. */
static void xi_count_rc_ops_recursive(const XiFunc *f, uint64_t *retain, uint64_t *release) {
    if (!f)
        return;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        const XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            const XiValue *v = blk->values[i];
            if (!v)
                continue;
            if (v->op == XI_RETAIN)
                (*retain)++;
            else if (v->op == XI_RELEASE)
                (*release)++;
        }
    }
    for (uint16_t i = 0; i < f->nchildren; i++)
        xi_count_rc_ops_recursive(f->children[i], retain, release);
}

static void xi_set_source_file_recursive(XiFunc *f, const char *source_file) {
    if (!f)
        return;
    f->source_file = source_file;
    for (uint16_t i = 0; i < f->nchildren; i++)
        xi_set_source_file_recursive(f->children[i], source_file);
}

static void xi_set_wide_vector_boundary_policy_recursive(XiFunc *f, bool preserve) {
    if (!f)
        return;
    f->preserve_wide_vector_boundaries = preserve;
    for (uint16_t i = 0; i < f->nchildren; i++)
        xi_set_wide_vector_boundary_policy_recursive(f->children[i], preserve);
}

static void xi_pipeline_set_error(XiPipelineResult *res, XiPipeStatus status, XiPipelineStage stage,
                                  XiVerifyCode code, const XiFunc *func, const XiValue *value,
                                  const char *pass_name, const char *detail) {
    if (!res || res->status != XI_PIPE_OK || res->error.detail[0] != '\0')
        return;
    res->status = status;
    res->error.stage = stage;
    res->error.code = code;
    res->error.func = func;
    res->error.value = value;
    res->error.source_line = value ? value->line : 0;
    res->error.pass_name = pass_name;
    snprintf(res->error.detail, sizeof(res->error.detail), "%s",
             detail && detail[0] ? detail : "pipeline stage failed without a diagnostic");
}

static bool xi_verify_tree(const XiFunc *f, const XiFunc **failed_func, char *errbuf,
                           size_t errbuf_size) {
    if (!f) {
        snprintf(errbuf, errbuf_size, "NULL Xi function");
        if (failed_func)
            *failed_func = NULL;
        return false;
    }
    if (!xi_verify_stage(f, f->stage, errbuf, (int) errbuf_size)) {
        if (failed_func)
            *failed_func = f;
        return false;
    }
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i] && !xi_verify_tree(f->children[i], failed_func, errbuf, errbuf_size))
            return false;
    }
    return true;
}

static XiVerifyCode xi_verify_code_from_detail(const char *detail) {
    if (!detail)
        return XI_VERIFY_STRUCTURE;
    if (strstr(detail, "RETURN") || strstr(detail, "return"))
        return XI_VERIFY_RETURN;
    if (strstr(detail, "PTR_") || strstr(detail, "memory") || strstr(detail, "Endian"))
        return XI_VERIFY_MEMORY;
    if (strstr(detail, "ErrorType"))
        return XI_VERIFY_EXECUTABLE_TYPE;
    return XI_VERIFY_STRUCTURE;
}

static bool xi_pipeline_verify_barrier(XiPipelineResult *res, XiPipelineStage stage) {
    char errbuf[512];
    const XiFunc *failed_func = NULL;
    if (!xi_verify_tree(res->ir, &failed_func, errbuf, sizeof(errbuf))) {
        xi_pipeline_set_error(res, XI_PIPE_ERR_VERIFY, stage, xi_verify_code_from_detail(errbuf),
                              failed_func, NULL, NULL, errbuf);
        return false;
    }

    XiSourceMoveVerifyReport move_report;
    XiSourceMoveVerifyStatus move_status = xi_source_move_verify_tree(res->ir, &move_report);
    if (move_status == XI_SOURCE_MOVE_PASS)
        return true;
    xi_pipeline_set_error(res,
                          move_status == XI_SOURCE_MOVE_INTERNAL_ERROR ? XI_PIPE_ERR_INTERNAL
                                                                       : XI_PIPE_ERR_VERIFY,
                          stage, XI_VERIFY_STRUCTURE, move_report.func, move_report.move,
                          "xi_source_move_verify", move_report.message);
    return false;
}

static bool xi_pipeline_analyzer_gate(struct XaAnalyzer *analyzer, XiPipelineResult *res) {
    if (!analyzer)
        return true;
    int count = 0;
    for (XaDiagnostic *diag = xa_analyzer_get_diagnostics(analyzer, &count); diag;
         diag = diag->next) {
        if (diag->severity != XR_DIAG_SEV_ERROR)
            continue;
        xi_pipeline_set_error(res, XI_PIPE_ERR_ANALYZE, XI_PIPE_STAGE_ANALYZE,
                              XI_VERIFY_EXECUTABLE_TYPE, NULL, NULL, NULL,
                              diag->message ? diag->message : "semantic analysis failed");
        res->error.source_line = diag->location.line > 0 ? (uint32_t) diag->location.line : 0;
        return false;
    }
    return true;
}

static bool xi_pipeline_push_source_file(struct XaAnalyzer *analyzer, const XiPipelineConfig *cfg,
                                         XaAnalyzerFileScope *file_scope, XiPipelineResult *res) {
    if (!analyzer || !cfg || !cfg->source_file || !cfg->source_file[0])
        return true;
    if (xa_analyzer_push_file_scope(analyzer, cfg->source_file, file_scope))
        return true;

    char detail[512];
    snprintf(detail, sizeof(detail), "analyzer has no file scope for '%s'", cfg->source_file);
    xi_pipeline_set_error(res, XI_PIPE_ERR_INTERNAL, XI_PIPE_STAGE_ANALYZE, XI_VERIFY_STRUCTURE,
                          NULL, NULL, NULL, detail);
    return false;
}

/* ========== Configuration ========== */

XR_FUNC XiPipelineConfig xi_pipeline_default_config(void) {
    XiPipelineConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = XI_PIPE_VM;
    cfg.run_optimize = true;
    cfg.opt_level = XI_OPT_LIGHT;
    cfg.run_select_rep = false;
    /* The VM runs escape analysis + precise dup/drop insertion (xi_arc) but
     * NOT stack_alloc_rewrite (no XI_STACK_ALLOC handler in the VM emitter)
     * and NOT backend_lower. dup/drop execute as OP_DUP/OP_DROP. */
    cfg.run_escape = true;
    cfg.run_arc = true;
    cfg.run_emit = true;
    cfg.run_canonicalize = true;
    cfg.dump_ir_before = false;
    cfg.dump_ir_after = false;
    cfg.budget_ns = XI_BUDGET_OPT_NS;
    cfg.rep_policy = xi_rep_policy_tagged_boundary();
    return cfg;
}

XR_FUNC XiPipelineConfig xi_pipeline_aot_config(void) {
    XiPipelineConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = XI_PIPE_AOT;
    cfg.run_optimize = true;
    cfg.opt_level = XI_OPT_FULL;
    cfg.run_select_rep = true;
    cfg.run_backend_lower = true;
    cfg.run_escape = true;
    cfg.run_arc = true;
    cfg.run_emit = false;
    cfg.run_canonicalize = true;
    cfg.dump_ir_before = false;
    cfg.dump_ir_after = false;
    cfg.rep_policy = xi_rep_policy_native_boundary();
    cfg.disabled_opt_passes = XI_OPT_DISABLE_IVSR;
    return cfg;
}

/* ========== Internal Pipeline ========== */

static XiFunc *xi_pipeline_release_stage_handle(void *program, XiStage stage) {
    switch (stage) {
        case XI_STAGE_RAW:
            return xi_raw_program_release((XiRawProgram *) program);
        case XI_STAGE_CANONICAL:
            return xi_canonical_program_release((XiCanonicalProgram *) program);
        case XI_STAGE_CLOSED:
            return xi_closed_program_release((XiClosedProgram *) program);
        case XI_STAGE_OWNED:
            return xi_owned_program_release((XiOwnedProgram *) program);
        case XI_STAGE_LOWERED:
            return xi_lowered_program_release((XiLoweredProgram *) program);
        case XI_STAGE_OPTIMIZED:
            return xi_optimized_program_release((XiOptimizedProgram *) program);
        case XI_STAGE_REPPED:
            return xi_repped_program_release((XiReppedProgram *) program);
        case XI_STAGE_BACKEND:
            return xi_backend_program_release((XiBackendProgram *) program);
        default:
            return NULL;
    }
}

static XiPipelineResult run_pipeline(XiFunc *ir, struct XrVMRuntime *X,
                                     const XiPipelineConfig *cfg) {
    XiPipelineResult res;
    memset(&res, 0, sizeof(res));
    res.ir = ir;

    if (!ir) {
        xi_pipeline_set_error(&res, XI_PIPE_ERR_LOWER, XI_PIPE_STAGE_LOWER,
                              XI_VERIFY_EXECUTABLE_TYPE, NULL, NULL, NULL,
                              "lowering did not produce executable Xi IR");
        return res;
    }

    xi_set_wide_vector_boundary_policy_recursive(ir, cfg->preserve_wide_vector_boundaries);

    char transition_error[512] = {0};
    void *program = xi_stage_adopt_raw(ir, transition_error, sizeof(transition_error));
    XiStage current_stage = XI_STAGE_RAW;
    if (!program) {
        xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_VERIFY_RAW,
                              XI_VERIFY_STRUCTURE, ir, NULL, NULL, transition_error);
        return res;
    }

    void *next = xi_program_canonicalize((XiRawProgram *) program, transition_error,
                                         sizeof(transition_error));
    if (!next) {
        xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_VERIFY_RAW,
                              XI_VERIFY_STRUCTURE, ir, NULL, NULL, transition_error);
        xi_pipeline_release_stage_handle(program, current_stage);
        return res;
    }
    program = next;
    current_stage = XI_STAGE_CANONICAL;

    /* Closure pass: build XiClosureMeta and materialize environment layout. */
    xi_pass_close(ir);
    next = xi_program_close((XiCanonicalProgram *) program, transition_error,
                            sizeof(transition_error));
    if (!next) {
        xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_VERIFY_RAW,
                              XI_VERIFY_STRUCTURE, ir, NULL, NULL, transition_error);
        xi_pipeline_release_stage_handle(program, current_stage);
        return res;
    }
    program = next;
    current_stage = XI_STAGE_CLOSED;

    /* Optional: dump IR before optimization */
    if (cfg->dump_ir_before) {
        fprintf(stderr, "=== Xi IR (before optimization) ===\n");
        xi_func_dump(ir, stderr);
        fprintf(stderr, "===================================\n");
    }

    /* Verification barriers are part of the executable pipeline contract and
     * cannot be disabled by configuration. */
    if (!xi_pipeline_verify_barrier(&res, XI_PIPE_STAGE_VERIFY_RAW))
        goto fail;

    if (cfg->run_backend_lower) {
        XiPassChange enum_type_lookup = xi_opt_compact_enum_payload_type_lookup(ir);
        if (enum_type_lookup.values_changed) {
            xi_opt_dce(ir);
            if (!xi_pipeline_verify_barrier(&res, XI_PIPE_STAGE_OPTIMIZE))
                return res;
        }
    }

    /* A concrete EnumVariant<E>/EnumPayloadField<E> is a scalar while E is
     * statically known.  Crossing an erased identity boundary is a semantic
     * allocation, so materialize it before escape and ARC analysis for both
     * VM and AOT instead of hiding it in AOT representation selection. */
    xi_opt_materialize_enum_descriptor_erasure(ir);

    /* Escape analysis: compute escape levels for heap-allocating values.
     * Ownership is made explicit before semantic optimization. */
    if (cfg->run_escape) {
        xi_escape_analyze(ir);
    }
    if (!xi_pipeline_verify_barrier(&res, XI_PIPE_STAGE_ESCAPE))
        goto fail;

    /* Backward ownership inference (analysis only — does not mutate IR).
     * Gated behind XRAY_XI_OWN_DUMP=1 for manual verification of dup/drop
     * decisions; the xi_arc rewrite consumes these annotations directly. */
    {
        if (xi_env_is_enabled("XRAY_XI_OWN_DUMP")) {
            xi_own_dump_recursive(ir);
        }
    }

    /* Stack alloc rewrite: replace NO_ESCAPE heap allocs with XI_STACK_ALLOC.
     * Must run after escape analysis and before ARC insertion (STACK_ALLOC
     * values don't need retain/release since they have frame lifetime).
     * Gated on run_backend_lower because only backend codegen (AOT)
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
        if (getenv("XRAY_XI_ARC_DUMP")) {
            fprintf(stderr, "=== Xi IR after xi_arc_insert ===\n");
            xi_func_dump(ir, stderr);
            fprintf(stderr, "=================================\n");
        }
        /* Dup/drop elimination: remove redundant RETAIN+RELEASE pairs where
         * the value is merely forwarded (copy→move optimization). Runs on
         * all backends including VM (fewer RC ops = faster interpretation). */
        xi_arc_elim(ir);
        if (xi_env_is_enabled("XRAY_XI_RC_COUNT")) {
            uint64_t nret = 0, nrel = 0;
            xi_count_rc_ops_recursive(ir, &nret, &nrel);
            fprintf(stderr,
                    "[xi-rc-count] func=%s retain=%" PRIu64 " release=%" PRIu64 " total=%" PRIu64
                    "\n",
                    ir->name ? ir->name : "<anon>", nret, nrel, nret + nrel);
        }
        /* Task 219: independent RC-contract verifier. Runs FORCED once here,
         * right after ARC insertion, in every build mode. A violation means the
         * compiler mis-inferred ownership (use-after-release / double-free in
         * otherwise-safe user code) — a hard ICE with a counterexample path is
         * the correct response, not a silent miscompile. */
        xi_arc_verify_or_ice(ir, "xi_arc_insert");
    }

    next = xi_program_make_owned((XiClosedProgram *) program, transition_error,
                                 sizeof(transition_error));
    if (!next) {
        xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_OWNERSHIP,
                              XI_VERIFY_STRUCTURE, ir, NULL, NULL, transition_error);
        goto fail;
    }
    program = next;
    current_stage = XI_STAGE_OWNED;

    next = xi_program_lower_semantics((XiOwnedProgram *) program, transition_error,
                                      sizeof(transition_error));
    if (!next) {
        xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_OWNERSHIP,
                              XI_VERIFY_STRUCTURE, ir, NULL, NULL, transition_error);
        goto fail;
    }
    program = next;
    current_stage = XI_STAGE_LOWERED;

    if (!xi_pipeline_verify_barrier(&res, XI_PIPE_STAGE_OWNERSHIP))
        goto fail;

    /* Optimization consumes Lowered and produces a verified Optimized program. */
    if (cfg->run_optimize) {
        XiOptLevel level = cfg->opt_level;
        if (level == XI_OPT_NONE)
            level = XI_OPT_LIGHT;

        XiPipelineStats stats;
        XiOptResult opt = xi_opt_run_pipeline_ex_with_mask(ir, level, &stats, cfg->budget_ns,
                                                           cfg->disabled_opt_passes);
        if (!opt.ok) {
            xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_OPTIMIZE,
                                  XI_VERIFY_OPT_INVARIANT, ir, NULL, opt.pass_name, opt.detail);
            goto fail;
        }
        if (xi_env_is_enabled("XRAY_XI_STATS"))
            xi_pipeline_stats_dump(&stats, ir->name);
    }

    next = xi_program_finish_optimization((XiLoweredProgram *) program, transition_error,
                                          sizeof(transition_error));
    if (!next) {
        xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_OPTIMIZE,
                              XI_VERIFY_OPT_INVARIANT, ir, NULL, NULL, transition_error);
        goto fail;
    }
    program = next;
    current_stage = XI_STAGE_OPTIMIZED;
    if (!xi_pipeline_verify_barrier(&res, XI_PIPE_STAGE_OPTIMIZE))
        goto fail;

    /* SelectRepresentations: insert BOX/UNBOX at representation boundaries.
     * Run after general optimization so constants/copies are resolved first. */
    if (cfg->run_select_rep) {
        xi_opt_refresh_representations_with_policy(ir, &cfg->rep_policy);
        next = xi_program_select_reps((XiOptimizedProgram *) program, transition_error,
                                      sizeof(transition_error));
        if (!next) {
            xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_REPRESENTATION,
                                  XI_VERIFY_STRUCTURE, ir, NULL, NULL, transition_error);
            goto fail;
        }
        program = next;
        current_stage = XI_STAGE_REPPED;
        if (!xi_pipeline_verify_barrier(&res, XI_PIPE_STAGE_REPRESENTATION))
            goto fail;
    }

    /* Backend lowering is legal only after target representation selection. */
    if (cfg->run_backend_lower) {
        if (current_stage != XI_STAGE_REPPED) {
            xi_pipeline_set_error(&res, XI_PIPE_ERR_INTERNAL, XI_PIPE_STAGE_BACKEND,
                                  XI_VERIFY_STRUCTURE, ir, NULL, NULL,
                                  "backend planning requires a Repped program");
            goto fail;
        }
        xi_backend_lower(ir);
        /* A unit ERR_CHECK has an implicit function-exit edge which ordinary
         * CFG liveness cannot represent.  Publish its cold-edge drop operands
         * only after optimization, representation selection, vector scalar
         * lowering, and every other value-cloning rewrite.  Earlier attachment
         * would turn error-only ownership into normal SSA uses and inhibit
         * dead-value/representation cleanup on the successful hot path. */
        if (cfg->run_escape && cfg->run_arc)
            xi_arc_attach_error_cleanups(ir);
        next = xi_program_plan_backend((XiReppedProgram *) program, transition_error,
                                       sizeof(transition_error));
        if (!next) {
            xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_BACKEND,
                                  XI_VERIFY_AOT_PLAN, ir, NULL, NULL, transition_error);
            goto fail;
        }
        program = next;
        current_stage = XI_STAGE_BACKEND;
        if (!xi_pipeline_verify_barrier(&res, XI_PIPE_STAGE_BACKEND))
            goto fail;
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
            xi_pipeline_set_error(&res, XI_PIPE_ERR_EMIT, XI_PIPE_STAGE_EMIT, XI_VERIFY_EMISSION,
                                  ir, NULL, NULL, xi_emit_status_str(emit_st));
            goto fail;
        }
        res.proto = proto;

        /* The VM emitter consumes Optimized semantic IR. After bytecode is
         * fixed, select the VM/JIT tagged representation and close the Repped
         * transition for the IR retained by the proto. */
        if (current_stage == XI_STAGE_OPTIMIZED) {
            XiRepPolicy vm_policy = xi_rep_policy_tagged_boundary();
            xi_opt_refresh_representations_with_policy(ir, &vm_policy);
            next = xi_program_select_reps((XiOptimizedProgram *) program, transition_error,
                                          sizeof(transition_error));
            if (!next) {
                xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_REPRESENTATION,
                                      XI_VERIFY_STRUCTURE, ir, NULL, NULL, transition_error);
                xr_vm_proto_free(proto);
                res.proto = NULL;
                goto fail;
            }
            program = next;
            current_stage = XI_STAGE_REPPED;
        }

        ir = xi_pipeline_release_stage_handle(program, current_stage);
        program = NULL;
        res.ir = ir;
        char snapshot_error[256];
        if (!xi_semantic_snapshot_detach_ex(ir, snapshot_error, sizeof(snapshot_error))) {
            xr_vm_proto_free(proto);
            res.proto = NULL;
            xi_pipeline_set_error(&res, XI_PIPE_ERR_INTERNAL, XI_PIPE_STAGE_EMIT,
                                  XI_VERIFY_EMISSION, ir, NULL, NULL,
                                  snapshot_error);
            return res;
        }
        /* Transfer Xi IR ownership to proto for AOT direct lowering.
         * Null res.ir so xi_pipeline_result_free won't double-free. */
        if (!xi_emit_attach_ir(proto, ir)) {
            xr_vm_proto_free(proto);
            res.proto = NULL;
            res.ir = ir;
            xi_pipeline_set_error(&res, XI_PIPE_ERR_INTERNAL, XI_PIPE_STAGE_EMIT,
                                  XI_VERIFY_EMISSION, ir, NULL, NULL,
                                  "VM IR attachment requires a verified Repped program");
            return res;
        }
        res.ir = NULL;
    } else {
        ir = xi_pipeline_release_stage_handle(program, current_stage);
        program = NULL;
        res.ir = ir;
        char snapshot_error[256];
        if (!xi_semantic_snapshot_detach_ex(ir, snapshot_error, sizeof(snapshot_error))) {
            xi_pipeline_set_error(&res, XI_PIPE_ERR_INTERNAL, XI_PIPE_STAGE_BACKEND,
                                  XI_VERIFY_AOT_PLAN, ir, NULL, NULL,
                                  snapshot_error);
            return res;
        }
    }

    res.status = XI_PIPE_OK;
    return res;

fail:
    if (program)
        xi_pipeline_release_stage_handle(program, current_stage);
    return res;
}

/* ========== Public API ========== */

XR_FUNC XiPipelineResult xi_pipeline_compile_func(struct AstNode *func_node,
                                                  struct XaAnalyzer *analyzer,
                                                  struct XrVMRuntime *isolate,
                                                  const XiPipelineConfig *cfg) {
    XR_DCHECK(func_node != NULL, "xi_pipeline_compile_func: NULL func_node");
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);

    XiPipelineConfig default_cfg;
    if (!cfg) {
        default_cfg = xi_pipeline_default_config();
        cfg = &default_cfg;
    }

    XiPipelineResult gate;
    memset(&gate, 0, sizeof(gate));
    XaAnalyzerFileScope file_scope = {0};
    if (!xi_pipeline_push_source_file(analyzer, cfg, &file_scope, &gate))
        return gate;
    if (!xi_pipeline_analyzer_gate(analyzer, &gate)) {
        xa_analyzer_pop_file_scope(analyzer, &file_scope);
        return gate;
    }

    /* Canonicalize AST before lowering */
    if (cfg->run_canonicalize)
        xr_canon_func(func_node, analyzer, session);

    XaTypedProgramPublishResult typed = xa_typed_program_publish(analyzer, func_node, NULL, 0);
    if (!typed.program) {
        xi_pipeline_set_error(&gate, XI_PIPE_ERR_ANALYZE, XI_PIPE_STAGE_ANALYZE,
                              XI_VERIFY_EXECUTABLE_TYPE, NULL, NULL,
                              xa_typed_program_reason_name(typed.reason), typed.detail);
        gate.error.source_line = typed.source_line;
        xa_analyzer_pop_file_scope(analyzer, &file_scope);
        return gate;
    }
    XiFunc *ir = xi_lower_func(typed.program, isolate);
    xa_typed_program_free(typed.program);

    xa_analyzer_pop_file_scope(analyzer, &file_scope);

    if (ir)
        xi_set_source_file_recursive(ir, cfg->source_file);

    return run_pipeline(ir, isolate, cfg);
}

XR_FUNC XiPipelineResult xi_pipeline_compile_program(struct AstNode *program_node,
                                                     struct XaAnalyzer *analyzer,
                                                     struct XrVMRuntime *isolate,
                                                     const XiPipelineConfig *cfg) {
    XR_DCHECK(program_node != NULL, "xi_pipeline_compile_program: NULL program_node");

    XiPipelineConfig default_cfg;
    if (!cfg) {
        default_cfg = xi_pipeline_default_config();
        cfg = &default_cfg;
    }

    XiPipelineResult gate;
    memset(&gate, 0, sizeof(gate));
    XaAnalyzerFileScope file_scope = {0};
    if (!xi_pipeline_push_source_file(analyzer, cfg, &file_scope, &gate))
        return gate;
    if (!xi_pipeline_analyzer_gate(analyzer, &gate)) {
        xa_analyzer_pop_file_scope(analyzer, &file_scope);
        return gate;
    }

    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    XrCompilerSessionScope canon_scope;
    bool has_canon_scope = cfg->run_canonicalize && program_node->type == AST_PROGRAM &&
                           program_node->as.program.arena &&
                           xr_compiler_session_push_arena(session, program_node->as.program.arena,
                                                          cfg->source_file, &canon_scope);

    /* Canonicalize AST before lowering */
    if (cfg->run_canonicalize)
        xr_canon_program(program_node, analyzer, session);

    if (has_canon_scope)
        xr_compiler_session_pop_arena(&canon_scope);

    XaTypedProgramPublishResult typed = xa_typed_program_publish(
        analyzer, program_node, cfg->global_evidence, cfg->global_evidence_module_id);
    if (!typed.program) {
        xi_pipeline_set_error(&gate, XI_PIPE_ERR_ANALYZE, XI_PIPE_STAGE_ANALYZE,
                              XI_VERIFY_EXECUTABLE_TYPE, NULL, NULL,
                              xa_typed_program_reason_name(typed.reason), typed.detail);
        gate.error.source_line = typed.source_line;
        xa_analyzer_pop_file_scope(analyzer, &file_scope);
        return gate;
    }
    XiFunc *ir = xi_lower_program(typed.program, isolate, cfg->repl_mode);
    xa_typed_program_free(typed.program);

    xa_analyzer_pop_file_scope(analyzer, &file_scope);

    if (ir)
        xi_set_source_file_recursive(ir, cfg->source_file);

    return run_pipeline(ir, isolate, cfg);
}

XR_FUNC struct XrProto *xi_pipeline_emit_ir(XiFunc *ir, struct XrVMRuntime *isolate) {
    XR_DCHECK(ir != NULL, "xi_pipeline_emit_ir: NULL ir");
    XR_DCHECK(isolate != NULL, "xi_pipeline_emit_ir: NULL isolate");

    struct XrProto *proto = NULL;
    XiEmitStatus emit_st = xi_emit(ir, isolate, &proto);
    if (emit_st != XI_EMIT_OK) {
        fprintf(stderr, "[xi_pipeline] emit_ir failed: %s\n", xi_emit_status_str(emit_st));
        return NULL;
    }

    char snapshot_error[256];
    if (!xi_semantic_snapshot_detach_ex(ir, snapshot_error, sizeof(snapshot_error))) {
        fprintf(stderr, "[xi_pipeline] semantic snapshot failed: %s\n", snapshot_error);
        xr_vm_proto_free(proto);
        return NULL;
    }

    /* Transfer IR ownership to proto for AOT direct lowering */
    if (!xi_emit_attach_ir(proto, ir)) {
        xr_vm_proto_free(proto);
        return NULL;
    }
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
        case XI_PIPE_ERR_ANALYZE:
            return "semantic analysis failed";
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

XR_FUNC const char *xi_pipeline_stage_str(XiPipelineStage stage) {
    switch (stage) {
        case XI_PIPE_STAGE_NONE:
            return "none";
        case XI_PIPE_STAGE_ANALYZE:
            return "analyze";
        case XI_PIPE_STAGE_LOWER:
            return "lower";
        case XI_PIPE_STAGE_VERIFY_RAW:
            return "verify-raw";
        case XI_PIPE_STAGE_OPTIMIZE:
            return "optimize";
        case XI_PIPE_STAGE_ESCAPE:
            return "escape";
        case XI_PIPE_STAGE_OWNERSHIP:
            return "ownership";
        case XI_PIPE_STAGE_REPRESENTATION:
            return "representation";
        case XI_PIPE_STAGE_BACKEND:
            return "backend";
        case XI_PIPE_STAGE_EMIT:
            return "emit";
    }
    return "unknown";
}
