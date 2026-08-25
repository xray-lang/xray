/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_program_semantic.h - Stable PSC family binding for Xi
 */

#ifndef XI_PROGRAM_SEMANTIC_H
#define XI_PROGRAM_SEMANTIC_H

#include "xi_module.h"
#include "../plan/semantic/xr_program_semantic_closure.h"
#include "../plan/target/xr_scalar_call_decision.h"

struct XrTargetProfile;

/* Borrowed, already-sealed authority used only while lowering is active. */
typedef struct XiProgramSemanticInput {
    const XrProgramSemanticClosure *closure;
    /* Present only for the already-sealed scalar target family. */
    const XrScalarCallDecision *decision;
} XiProgramSemanticInput;

XR_FUNC bool xi_program_semantic_input_is_consistent(const XiProgramSemanticInput *input,
                                                     char *error, size_t error_size);

/* Lowering-only mechanical joins. These functions never inspect analyzer
 * authority, declarations names, or function bodies. */
XR_FUNC bool xi_program_semantic_bind_function(XiFunc *function,
                                               const XiProgramSemanticInput *input,
                                               XiSourceLocator locator, char *error,
                                               size_t error_size);
XR_FUNC bool xi_program_semantic_find_call(const XiFunc *caller,
                                           const XiProgramSemanticInput *input,
                                           XiSourceLocator locator, uint32_t *call_index,
                                           char *error, size_t error_size);

/* Complete all row-index relationships after the function tree is built. */
XR_FUNC bool xi_program_semantic_finalize(XiFunc *root, const XiProgramSemanticInput *input,
                                          char *error, size_t error_size);

/* Atomically transfers the caller's original PSC reference and a heap-owned
 * copy of the pointer-free decision. Failure changes neither owner. */
XR_FUNC bool xi_module_take_program_semantics(XiModule *module, XrProgramSemanticClosure **closure,
                                              const XrScalarCallDecision *decision,
                                              const struct XrTargetProfile *target_profile,
                                              char *error, size_t error_size);

/* Independent post-transfer verification. */
XR_FUNC bool xi_program_semantic_verify(const XiModule *module,
                                        const struct XrTargetProfile *target_profile, char *error,
                                        size_t error_size);

#endif  // XI_PROGRAM_SEMANTIC_H
