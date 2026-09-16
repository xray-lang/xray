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

typedef struct XrProgramFromXiInput {
    const struct XiFunc *const *module_roots;
    uint32_t module_count;
    const struct XiFunc *entry_function;
    const struct XgGlobalEvidence *global_evidence;
    const uint8_t *semantic_profile_fingerprint;
} XrProgramFromXiInput;

/* Xi is compiler-private source IR. This function accepts only an Optimized,
 * target-neutral graph with exact source-module authority and whose old
 * SemanticPlan has never been built. The caller selects one exact Xi function
 * as the linked-program entry. Calls consume the same verified global evidence
 * identities that were bound during Xi lowering; no source spelling, Xi value
 * shape, or legacy plan row is used to infer a target. The result is the
 * canonical distributable artifact and one independently owned immutable
 * program validated with the default admission policy and budget. Both output
 * pointers are required and must be empty on entry. On failure they remain
 * empty; on success neither output borrows Xi or the other's byte storage.
 * A consumer with a different admission policy must validate the bytes under
 * that policy. Unsupported Xi operations fail closed. */
XR_FUNC XrProgramBuildStatus xr_program_write_from_xi(const XrProgramFromXiInput *input,
                                                      XrProgramArtifact *artifact_out,
                                                      XrValidatedProgram **program_out,
                                                      char *diagnostic, size_t diagnostic_size);

#endif /* XR_PROGRAM_FROM_XI_H */
