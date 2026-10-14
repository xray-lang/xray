/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_debug_internal.h - Authenticated VM observation points
 */
#ifndef XXIR_VM_DEBUG_INTERNAL_H
#define XXIR_VM_DEBUG_INTERNAL_H
#include "xxir_vm_debug.h"

typedef struct XirVmDebugPoint {
    const XrXirVmBinding *binding;
    const XrXirFunctionLayout *layout;
    void *state, *frame;
    XrXirCallView *view;
    uint32_t instruction;
} XirVmDebugPoint;
XR_FUNC bool xir_vm_debug_retain(XrXirVmDebug *debug);
XR_FUNC bool xir_vm_debug_bind(XrXirVmDebug *debug, const XrXirArtifact *artifact);
XR_FUNC void xir_vm_debug_program(XrXirVmDebug *debug, XrXirProgram *program);
XR_FUNC void xir_vm_debug_unbind(XrXirVmDebug *debug);
XR_FUNC XrXirValueStatus xir_vm_debug_before(XrXirVmDebug *debug,
    const XirVmDebugPoint *point, bool *stopped);
XR_FUNC void xir_vm_debug_after(XrXirVmDebug *debug, const XirVmDebugPoint *point,
    XrXirActionKind action);
XR_FUNC XrXirValueStatus xir_vm_debug_inbox(XrXirVmDebug *debug, void *state, uint32_t offset);
XR_FUNC void xir_vm_debug_release(XrXirVmDebug *debug, void *state);
#endif // XXIR_VM_DEBUG_INTERNAL_H
