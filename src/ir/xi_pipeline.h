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
#include "xi_opt.h"
#include "xi_module.h"

struct AstNode;
struct XaAnalyzer;
struct XgGlobalEvidence;
struct XrModuleGraph;
struct XrVMRuntime;
struct XrProto;

/* ========== Pipeline Status ========== */

typedef enum {
    XI_PIPE_OK = 0,
    XI_PIPE_ERR_ANALYZE,  /* semantic analysis produced an error */
    XI_PIPE_ERR_LOWER,    /* AST lowering failed */
    XI_PIPE_ERR_VERIFY,   /* IR verification found errors */
    XI_PIPE_ERR_EMIT,     /* bytecode emission failed */
    XI_PIPE_ERR_INTERNAL, /* unexpected internal error */
} XiPipeStatus;

typedef enum XiPipelineStage {
    XI_PIPE_STAGE_NONE = 0,
    XI_PIPE_STAGE_ANALYZE,
    XI_PIPE_STAGE_LOWER,
    XI_PIPE_STAGE_VERIFY_RAW,
    XI_PIPE_STAGE_OPTIMIZE,
    XI_PIPE_STAGE_ESCAPE,
    XI_PIPE_STAGE_OWNERSHIP,
    XI_PIPE_STAGE_SEMANTIC_PLAN,
    XI_PIPE_STAGE_REPRESENTATION,
    XI_PIPE_STAGE_BACKEND,
    XI_PIPE_STAGE_EMIT,
} XiPipelineStage;

typedef enum XiVerifyCode {
    XI_VERIFY_NONE = 0,
    XI_VERIFY_EXECUTABLE_TYPE,
    XI_VERIFY_STRUCTURE,
    XI_VERIFY_RETURN,
    XI_VERIFY_MEMORY,
    XI_VERIFY_OPT_INVARIANT,
    XI_VERIFY_AOT_PLAN,
    XI_VERIFY_EMISSION,
} XiVerifyCode;

typedef struct XiPipelineError {
    XiPipelineStage stage;
    XiVerifyCode code;
    const XiFunc *func;
    const XiValue *value;
    uint32_t source_line;
    const char *pass_name;
    char detail[512];
} XiPipelineError;

/* ========== Pipeline Mode ========== */

typedef enum {
    XI_PIPE_VM,    /* lower → verify → opt → bytecode emit */
    XI_PIPE_AOT,   /* lower → verify → opt → select_rep → box_elim (no emit) */
    XI_PIPE_CHECK, /* lower → verify only (no opt, no emit) */
} XiPipelineMode;

/* ========== Pipeline Configuration ========== */

/* Default optimizer time budget: 5 ms in nanoseconds. */
#define XI_BUDGET_OPT_NS (5ULL * 1000 * 1000)

typedef struct XiPipelineConfig {
    XiPipelineMode mode;     /* selects default pass sequence (can be overridden) */
    bool run_optimize;       /* run optimization passes (default: true) */
    XiOptLevel opt_level;    /* optimization aggressiveness (XI_OPT_LIGHT for VM,
                              * XI_OPT_FULL for AOT) */
    bool run_select_rep;     /* run SelectRepresentations pass (BOX/UNBOX insertion,
                              * needed by the AOT backend for unboxed values;
                              * default: false for VM, true for AOT) */
    bool run_backend_lower;  /* lower high-level ops and verify the Backend transition
                              * (default: false for VM, true for AOT) */
    bool run_escape;         /* run escape analysis (populates XiValue.escape;
                              * default: false for VM, true for AOT) */
    bool run_arc;            /* run precise dup/drop insertion (xi_arc) consuming
                              * ownership analysis. Independent of run_backend_lower
                              * so the VM can get dup/drop WITHOUT stack_alloc_rewrite
                              * (the VM emitter has no XI_STACK_ALLOC handler).
                              * default: false for VM (until RC takeover), true for AOT. */
    bool run_emit;           /* emit bytecode (default: true for VM, false for AOT) */
    bool run_canonicalize;   /* canonicalize AST before lowering (default: true). AOT driver can
                              * canonicalize all modules first, build global evidence from that
                              * canonical AST, then run lowering with this disabled. */
    bool dump_ir_before;     /* dump IR to stderr before optimization */
    bool dump_ir_after;      /* dump IR to stderr after optimization */
    uint64_t budget_ns;      /* optimization time budget in nanoseconds
                              * (0 = unlimited; default XI_BUDGET_OPT_NS) */
    bool repl_mode;          /* REPL incremental compilation: top-level bindings
                              * are lowered to XI_GET/SET_GLOBAL (name-keyed dict)
                              * instead of XI_GET/SET_SHARED (slot-indexed array).
                              * Default: false (script-mode shared array path). */
    const char *source_file; /* Source path propagated to emitted XrProto debug info. */
    XiRepPolicy rep_policy;  /* policy for representation boundary insertion */
    XiOptDisableMask disabled_opt_passes;
    bool preserve_wide_vector_boundaries; /* keep target-specific wide SIMD behind call edges */
    const struct XgGlobalEvidence *global_evidence; /* optional lowering-time evidence seed */
    uint32_t global_evidence_module_id;             /* 1-based module id in global evidence */
    /* Multi-module import resolution context. When a driver compiles a module
     * graph in topological order it can expose the graph plus the (partially
     * filled, dependency-complete) module array here; the pipeline then
     * resolves this module's XI_IMPORT_REF values BEFORE ARC insertion, so
     * ARC can read the final borrow signature of a cross-module callee and
     * keep caller-side ownership of arguments the callee only borrows.
     * All-NULL/0 (the default) skips early resolution; unresolved refs fall
     * back to the moved-argument convention (leak-safe, never a double free). */
    const struct XrModuleGraph *module_graph; /* module graph, or NULL */
    struct XiModule **graph_modules;          /* topo-indexed modules (NULL tail allowed) */
    int graph_module_count;                   /* entries in graph_modules */
} XiPipelineConfig;

/* ========== Pipeline Result ========== */

typedef struct XiPipelineResult {
    XiPipeStatus status;
    struct XrProto *proto; /* output bytecode (owned by caller; NULL in AOT/CHECK mode) */
    XiFunc *ir;            /* intermediate IR (freed on result_free) */
    XiModule *module;      /* module metadata (populated in AOT mode; freed on result_free) */
    XiPipelineError error; /* owned, structured first-error diagnostic */
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
                                                  struct XrVMRuntime *isolate,
                                                  const XiPipelineConfig *cfg);

/* Compile a top-level program AST through the full pipeline. */
XR_FUNC XiPipelineResult xi_pipeline_compile_program(struct AstNode *program_node,
                                                     struct XaAnalyzer *analyzer,
                                                     struct XrVMRuntime *isolate,
                                                     const XiPipelineConfig *cfg);

/* Emit bytecode from a pre-lowered XiFunc* (for split compilation).
 * The IR must have been lowered and optimized (via compile_program with
 * run_emit=false, or the AOT pipeline).  Returns XrProto on success, NULL
 * on emission failure.  Attaches the IR to the proto for AOT reuse. */
XR_FUNC struct XrProto *xi_pipeline_emit_ir(XiFunc *ir, struct XrVMRuntime *isolate);

/* Free pipeline result (frees IR, does NOT free proto). */
XR_FUNC void xi_pipeline_result_free(XiPipelineResult *res);

/* Human-readable status string. */
XR_FUNC const char *xi_pipe_status_str(XiPipeStatus s);
XR_FUNC const char *xi_pipeline_stage_str(XiPipelineStage stage);

#endif  // XI_PIPELINE_H
