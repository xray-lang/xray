/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_pipeline.h - Unified Xi IR compilation pipeline
 *
 * KEY CONCEPT:
 *   Orchestrates the full IR compilation pipeline in one call:
 *     AST -> xi_lower -> xi_verify -> xi_opt -> xi_emit -> XrProto
 *
 *   Provides both high-level convenience API and fine-grained control
 *   over individual passes via configuration flags.
 *
 * USAGE:
 *   XiPipelineConfig cfg = xi_pipeline_default_config();
 *   XiPipelineResult res = xi_pipeline_compile_func(func_ast, analyzer, isolate, &cfg);
 *   if (res.status == XI_PIPE_OK) use(res.proto);
 *   xi_pipeline_result_free(&res);
 */

#ifndef XI_PIPELINE_H
#define XI_PIPELINE_H

#include "xi.h"
#include "xi_pass.h"

struct AstNode;
struct XaAnalyzer;
struct XrayIsolate;
struct XrProto;

/* ========== Pipeline Status ========== */

typedef enum {
    XI_PIPE_OK = 0,
    XI_PIPE_ERR_LOWER,    /* AST lowering failed */
    XI_PIPE_ERR_VERIFY,   /* IR verification found errors */
    XI_PIPE_ERR_EMIT,     /* bytecode emission failed */
    XI_PIPE_ERR_INTERNAL, /* unexpected internal error */
} XiPipeStatus;

/* ========== Pipeline Mode ========== */

typedef enum {
    XI_PIPE_VM,    /* lower → verify → opt → bytecode emit */
    XI_PIPE_AOT,   /* lower → verify → opt → select_rep → box_elim (no emit) */
    XI_PIPE_CHECK, /* lower → verify only (no opt, no emit) */
} XiPipelineMode;

/* ========== Pipeline Configuration ========== */

/* Time budget for JIT Tier 1 optimization: 5 ms in nanoseconds. */
#define XI_BUDGET_JIT_TIER1_NS (5ULL * 1000 * 1000)

typedef struct XiPipelineConfig {
    XiPipelineMode mode;    /* selects default pass sequence (can be overridden) */
    bool run_verify;        /* run IR verification after lowering (default: true) */
    bool run_optimize;      /* run optimization passes (default: true) */
    XiOptLevel opt_level;   /* optimization aggressiveness (XI_OPT_LIGHT for VM,
                             * XI_OPT_FULL for AOT/JIT Tier 2) */
    bool run_select_rep;    /* run SelectRepresentations pass (BOX/UNBOX insertion,
                             * needed by AOT/JIT backends for unboxed values;
                             * default: false for VM, true for AOT) */
    bool run_backend_lower; /* lower high-level ops to XI_CALL_BUILTIN, advancing
                             * to STAGE_BACKEND (default: false for VM, true for AOT) */
    bool run_escape;        /* run escape analysis (populates XiValue.escape;
                             * default: false for VM, true for AOT) */
    bool run_emit;          /* emit bytecode (default: true for VM, false for AOT) */
    bool dump_ir_before;    /* dump IR to stderr before optimization */
    bool dump_ir_after;     /* dump IR to stderr after optimization */
    uint64_t budget_ns;     /* optimization time budget in nanoseconds
                             * (0 = unlimited; use XI_BUDGET_JIT_TIER1_NS for JIT) */
    bool repl_mode;         /* REPL incremental compilation: top-level bindings
                             * are lowered to XI_GET/SET_GLOBAL (name-keyed dict)
                             * instead of XI_GET/SET_SHARED (slot-indexed array).
                             * Default: false (script-mode shared array path). */
} XiPipelineConfig;

/* ========== Pipeline Result ========== */

typedef struct XiPipelineResult {
    XiPipeStatus status;
    struct XrProto *proto; /* output bytecode (owned by caller; NULL in AOT/CHECK mode) */
    XiFunc *ir;            /* intermediate IR (freed on result_free) */
    XiModule *module;      /* module metadata (populated in AOT mode; freed on result_free) */
    const char *error_msg; /* human-readable error description */
} XiPipelineResult;

/* ========== API ========== */

/* Default configuration: XI_PIPE_VM mode, verify + optimize + emit enabled. */
XR_FUNC XiPipelineConfig xi_pipeline_default_config(void);

/* AOT configuration: verify + optimize + select_rep, no bytecode emit. */
XR_FUNC XiPipelineConfig xi_pipeline_aot_config(void);

/* Compile a function AST node through the full pipeline.
 * Returns pipeline result; caller must call xi_pipeline_result_free. */
XR_FUNC XiPipelineResult xi_pipeline_compile_func(struct AstNode *func_node,
                                                  struct XaAnalyzer *analyzer,
                                                  struct XrayIsolate *isolate,
                                                  const XiPipelineConfig *cfg);

/* Compile a top-level program AST through the full pipeline. */
XR_FUNC XiPipelineResult xi_pipeline_compile_program(struct AstNode *program_node,
                                                     struct XaAnalyzer *analyzer,
                                                     struct XrayIsolate *isolate,
                                                     const XiPipelineConfig *cfg);

/* Free pipeline result (frees IR, does NOT free proto). */
XR_FUNC void xi_pipeline_result_free(XiPipelineResult *res);

/* Human-readable status string. */
XR_FUNC const char *xi_pipe_status_str(XiPipeStatus s);

#endif  // XI_PIPELINE_H
