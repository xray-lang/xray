/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_program_reachability.h - Frozen SemanticPlan executable closure
 */

#ifndef XR_TARGET_PROGRAM_REACHABILITY_H
#define XR_TARGET_PROGRAM_REACHABILITY_H

#include "../semantic/xr_semantic_plan.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Program-module TargetPlans retain complete per-module type, value and
 * function tables, but call authority is an executable property: an imported
 * library helper that no program root reaches is not a call the product must
 * lower.  This target-neutral bitmap is derived only from frozen SemanticPlan
 * identities, so both construction and independent verification can reproduce
 * it without consulting Xi or a backend.
 *
 * Unsupported open calls are deliberately not guessed here.  They remain in a
 * reachable caller and the ordinary call family rejects them fail-closed. */
typedef struct XrTargetProgramReachability {
    uint32_t module_count;
    uint32_t *function_begins;
    uint8_t *functions;
} XrTargetProgramReachability;

XR_FUNC bool xr_target_program_reachability_build(const XrSemanticPlan *const *modules,
                                                  uint32_t module_count,
                                                  XrTargetProgramReachability *out, char *error,
                                                  size_t error_size);
XR_FUNC void xr_target_program_reachability_dispose(XrTargetProgramReachability *reachability);
XR_FUNC bool
xr_target_program_function_is_reachable(const XrTargetProgramReachability *reachability,
                                        uint32_t module, uint32_t function);

#endif /* XR_TARGET_PROGRAM_REACHABILITY_H */
