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
#include "xxir_call.h"

typedef struct XrXirVmBinding {
    const XrXirArtifact *artifact;
    uint32_t function;
} XrXirVmBinding;

/* Bindings and their artifacts outlive activations. Table indices preserve the
 * artifact's function IDs; sealed image integration owns that correspondence. */
XR_FUNC XrXirStatus xr_xir_vm_bind(const XrXirArtifact *artifact, uint32_t function,
                                  XrXirVmBinding *binding, XrXirCallEntry *entry);

XR_FUNC XrXirRunStatus xr_xir_vm_run(const XrXirArtifact *artifact, uint32_t function,
                                   XrXirRunContext *context, const XrXirScalar *arguments,
                                   uint32_t argument_count, XrXirScalar *result);

#endif // XXIR_VM_H
