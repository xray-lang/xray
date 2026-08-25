/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_scalar_program.h - Stable PSC/CallDecision binding for Xi
 */

#ifndef XI_SCALAR_PROGRAM_H
#define XI_SCALAR_PROGRAM_H

#include "xi_module.h"
#include "../plan/semantic/xr_program_semantic_closure.h"
#include "../plan/target/xr_scalar_call_decision.h"

struct XrTargetProfile;

/* Borrowed, already-sealed authority used only while lowering is active. */
typedef struct XiScalarProgramInput {
    const XrProgramSemanticClosure *closure;
    const XrScalarCallDecision *decision;
} XiScalarProgramInput;

XR_FUNC bool xi_scalar_program_input_is_consistent(
    const XiScalarProgramInput *input, char *error, size_t error_size);

/* Lowering-only mechanical joins. These functions never inspect analyzer
 * authority, declarations names, or function bodies. */
XR_FUNC bool xi_scalar_program_bind_function(
    XiFunc *function, const XiScalarProgramInput *input, XiSourceLocator locator,
    char *error, size_t error_size);
XR_FUNC bool xi_scalar_program_find_call(
    const XiFunc *caller, const XiScalarProgramInput *input,
    XiSourceLocator locator, uint32_t *call_index, char *error,
    size_t error_size);

/* Complete all row-index relationships after the function tree is built. */
XR_FUNC bool xi_scalar_program_finalize(
    XiFunc *root, const XiScalarProgramInput *input, char *error,
    size_t error_size);

/* Atomically transfers the caller's original PSC reference and a heap-owned
 * copy of the pointer-free decision. Failure changes neither owner. */
XR_FUNC bool xi_module_take_scalar_program(
    XiModule *module, XrProgramSemanticClosure **closure,
    const XrScalarCallDecision *decision,
    const struct XrTargetProfile *target_profile, char *error,
    size_t error_size);

/* Independent post-transfer verification. */
XR_FUNC bool xi_scalar_program_verify(
    const XiModule *module, const struct XrTargetProfile *target_profile,
    char *error, size_t error_size);

#endif  // XI_SCALAR_PROGRAM_H
