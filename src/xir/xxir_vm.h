/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm.h - Lowered XIR scalar interpreter
 *
 * KEY CONCEPT:
 *   Execution consumes target-bound offsets and never interprets Checked IR.
 */

#ifndef XXIR_VM_H
#define XXIR_VM_H

#include "xxir.h"
#include "xxir_program.h"

typedef struct XrXirVmBinding {
    const XrXirArtifact *artifact;
    uint32_t function;
} XrXirVmBinding;

/* Bindings and their artifacts outlive activations. Table indices preserve the
 * artifact's function IDs; sealed image integration owns that correspondence.
 * Validation consumes the artifact ledger and failure preserves both outputs. */
XR_FUNC XrXirStatus xr_xir_compile_vm_bind(const XrXirArtifact *artifact, uint32_t function,
                                  XrXirVmBinding *binding, XrXirCallEntry *entry);
/* Validate the complete Lowered artifact once and bind every function in ID
 * order. Both distinct output slots must be empty. The caller owns both arrays
 * through xr_compile_resources_free and keeps the artifact alive while using
 * them. Failure preserves both outputs and releases all temporary storage. */
XR_FUNC XrXirStatus xr_xir_compile_vm_bind_table(const XrXirArtifact *artifact,
    XrXirVmBinding **bindings, XrXirCallEntry **entries);
/* Success consumes and clears the Lowered artifact; failure preserves it. */
XR_FUNC XrXirStatus xr_xir_compile_vm_program_take(XrXirArtifact **artifact,
                                          XrXirProgram **output);

XR_FUNC XrXirRunStatus xr_xir_compile_vm_run(const XrXirArtifact *artifact, uint32_t function,
                                   XrXirRunContext *context, const XrXirValue *arguments,
                                   uint32_t argument_count, XrXirValue *result);

#endif // XXIR_VM_H
