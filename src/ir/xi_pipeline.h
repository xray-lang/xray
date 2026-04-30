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

struct AstNode;
struct XaAnalyzer;
struct XrayIsolate;
struct XrProto;

/* ========== Pipeline Status ========== */

typedef enum {
    XI_PIPE_OK = 0,
    XI_PIPE_ERR_LOWER,      /* AST lowering failed */
    XI_PIPE_ERR_VERIFY,     /* IR verification found errors */
    XI_PIPE_ERR_EMIT,       /* bytecode emission failed */
    XI_PIPE_ERR_INTERNAL,   /* unexpected internal error */
} XiPipeStatus;

/* ========== Pipeline Configuration ========== */

typedef struct XiPipelineConfig {
    bool run_verify;        /* run IR verification after lowering (default: true) */
    bool run_optimize;      /* run optimization passes (default: true) */
    bool run_select_rep;    /* run SelectRepresentations pass (BOX/UNBOX insertion,
                             * needed by AOT/JIT backends for unboxed values;
                             * default: false, VM bytecode backend does not need it) */
    bool dump_ir_before;    /* dump IR to stderr before optimization */
    bool dump_ir_after;     /* dump IR to stderr after optimization */
} XiPipelineConfig;

/* ========== Pipeline Result ========== */

typedef struct XiPipelineResult {
    XiPipeStatus status;
    struct XrProto *proto;  /* output bytecode (owned by caller on success) */
    XiFunc *ir;             /* intermediate IR (freed on result_free) */
    const char *error_msg;  /* human-readable error description */
} XiPipelineResult;

/* ========== API ========== */

/* Default configuration: verify + optimize enabled, no dumps. */
XR_FUNC XiPipelineConfig xi_pipeline_default_config(void);

/* Compile a function AST node through the full pipeline.
 * Returns pipeline result; caller must call xi_pipeline_result_free. */
XR_FUNC XiPipelineResult xi_pipeline_compile_func(
    struct AstNode *func_node,
    struct XaAnalyzer *analyzer,
    struct XrayIsolate *isolate,
    const XiPipelineConfig *cfg);

/* Compile a top-level program AST through the full pipeline. */
XR_FUNC XiPipelineResult xi_pipeline_compile_program(
    struct AstNode *program_node,
    struct XaAnalyzer *analyzer,
    struct XrayIsolate *isolate,
    const XiPipelineConfig *cfg);

/* Free pipeline result (frees IR, does NOT free proto). */
XR_FUNC void xi_pipeline_result_free(XiPipelineResult *res);

/* Human-readable status string. */
XR_FUNC const char *xi_pipe_status_str(XiPipeStatus s);

#endif  // XI_PIPELINE_H
