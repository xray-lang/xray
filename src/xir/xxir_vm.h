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
 * artifact's function IDs; sealed image integration owns that correspondence. */
XR_FUNC XrXirStatus xr_xir_vm_bind(const XrXirArtifact *artifact, uint32_t function,
                                  XrXirVmBinding *binding, XrXirCallEntry *entry);
/* Success consumes and clears the Lowered artifact; failure preserves it. */
XR_FUNC XrXirStatus xr_xir_vm_program_take(XrXirArtifact **artifact, uint64_t byte_limit,
                                          XrXirProgram **output);

XR_FUNC XrXirRunStatus xr_xir_vm_run(const XrXirArtifact *artifact, uint32_t function,
                                   XrXirRunContext *context, const XrXirValue *arguments,
                                   uint32_t argument_count, XrXirValue *result);

#endif // XXIR_VM_H
