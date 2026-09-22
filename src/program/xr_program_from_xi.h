/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_from_xi.h - Verified Xi to canonical XrProgram producer
 */

#ifndef XR_PROGRAM_FROM_XI_H
#define XR_PROGRAM_FROM_XI_H

#include "xr_program_verify.h"

struct XiFunc;
struct XgGlobalEvidence;
struct XrModuleGraph;

typedef struct XrProgramFromXiInput {
    const struct XiFunc *const *module_roots;
    uint32_t module_count;
    const struct XiFunc *entry_function;
    const struct XiFunc *const *retained_functions;
    uint32_t retained_function_count;
    const struct XgGlobalEvidence *global_evidence;
    const struct XrModuleGraph *module_graph;
    const uint8_t *semantic_profile_fingerprint;
} XrProgramFromXiInput;

/* Accepts an Optimized, target-neutral Xi graph with exact module authority and
 * no legacy SemanticPlan. entry_function selects the single default entry;
 * retained_functions adds complete callable roots without extra default entries.
 *
 * For a nonzero retained_function_count, retained_function_ids must address that
 * many writable slots. IDs use request order and refer to the returned Program.
 * For bounded requests, every supplied slot is UINT32_MAX on failure.
 *
 * Calls consume verified global evidence bound during lowering. Source spelling,
 * value shape and legacy plan rows never infer targets. The required artifact
 * and program outputs must be empty on entry. They own independent byte storage
 * and never borrow Xi. Rejection leaves both empty. The writer validates under
 * default admission policy; a different policy requires fresh byte admission.
 * Unsupported Xi operations fail closed. */
XR_FUNC XrProgramBuildStatus xr_program_write_from_xi(const XrProgramFromXiInput *input,
                                                      XrProgramArtifact *artifact_out,
                                                      XrValidatedProgram **program_out,
                                                      uint32_t *retained_function_ids,
                                                      char *diagnostic, size_t diagnostic_size);

#endif /* XR_PROGRAM_FROM_XI_H */
