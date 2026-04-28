/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_pipeline.c - Unified Xi IR compilation pipeline
 *
 * Orchestrates: AST -> xi_lower -> xi_verify -> xi_opt -> xi_emit -> XrProto
 */

#include "xi_pipeline.h"
#include "xi_lower.h"
#include "xi_verify.h"
#include "xi_opt.h"
#include "xi_emit.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"

#include <stdio.h>
#include <string.h>

/* ========== Configuration ========== */

XR_FUNC XiPipelineConfig xi_pipeline_default_config(void) {
    XiPipelineConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.run_verify = true;
    cfg.run_optimize = true;
    cfg.dump_ir_before = false;
    cfg.dump_ir_after = false;
    return cfg;
}

/* ========== Internal Pipeline ========== */

static XiPipelineResult run_pipeline(XiFunc *ir, const XiPipelineConfig *cfg) {
    XiPipelineResult res;
    memset(&res, 0, sizeof(res));
    res.ir = ir;

    if (!ir) {
        res.status = XI_PIPE_ERR_LOWER;
        res.error_msg = "lowering returned NULL";
        return res;
    }

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

    /* Optimization passes */
    if (cfg->run_optimize) {
        xi_opt_run(ir);
    }

    /* Optional: dump IR after optimization */
    if (cfg->dump_ir_after) {
        fprintf(stderr, "=== Xi IR (after optimization) ===\n");
        xi_func_dump(ir, stderr);
        fprintf(stderr, "==================================\n");
    }

    /* Bytecode emission */
    struct XrProto *proto = NULL;
    XiEmitStatus emit_st = xi_emit(ir, &proto);
    if (emit_st != XI_EMIT_OK) {
        res.status = XI_PIPE_ERR_EMIT;
        res.error_msg = xi_emit_status_str(emit_st);
        return res;
    }

    res.status = XI_PIPE_OK;
    res.proto = proto;
    return res;
}

/* ========== Public API ========== */

XR_FUNC XiPipelineResult xi_pipeline_compile_func(
    struct AstNode *func_node,
    struct XaAnalyzer *analyzer,
    struct XrayIsolate *isolate,
    const XiPipelineConfig *cfg)
{
    XR_DCHECK(func_node != NULL, "xi_pipeline_compile_func: NULL func_node");

    XiPipelineConfig default_cfg;
    if (!cfg) {
        default_cfg = xi_pipeline_default_config();
        cfg = &default_cfg;
    }

    XiFunc *ir = xi_lower_func(func_node, analyzer, isolate);
    return run_pipeline(ir, cfg);
}

XR_FUNC XiPipelineResult xi_pipeline_compile_program(
    struct AstNode *program_node,
    struct XaAnalyzer *analyzer,
    struct XrayIsolate *isolate,
    const XiPipelineConfig *cfg)
{
    XR_DCHECK(program_node != NULL, "xi_pipeline_compile_program: NULL program_node");

    XiPipelineConfig default_cfg;
    if (!cfg) {
        default_cfg = xi_pipeline_default_config();
        cfg = &default_cfg;
    }

    XiFunc *ir = xi_lower_program(program_node, analyzer, isolate);
    return run_pipeline(ir, cfg);
}

XR_FUNC void xi_pipeline_result_free(XiPipelineResult *res) {
    if (!res) return;
    if (res->ir) {
        xi_func_free(res->ir);
        res->ir = NULL;
    }
    /* proto is NOT freed — caller owns it */
}

XR_FUNC const char *xi_pipe_status_str(XiPipeStatus s) {
    switch (s) {
        case XI_PIPE_OK:          return "OK";
        case XI_PIPE_ERR_LOWER:   return "AST lowering failed";
        case XI_PIPE_ERR_VERIFY:  return "IR verification failed";
        case XI_PIPE_ERR_EMIT:    return "bytecode emission failed";
        case XI_PIPE_ERR_INTERNAL:return "internal pipeline error";
    }
    return "unknown";
}
