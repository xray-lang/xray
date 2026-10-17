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
#include "xxir_source_query.h"
#include "xxir_value_place.h"

typedef struct XirVmDebugPoint {
    const XrXirVmBinding *binding;
    const XrXirFunctionLayout *layout;
    void *state, *frame;
    XrXirCallView *view;
    uint32_t instruction;
} XirVmDebugPoint;
typedef struct XirVmDebugStopCause { uint32_t groups;bool unfiltered; } XirVmDebugStopCause;
typedef enum XirVmDebugExceptionLocalKind {
    XIR_VM_DEBUG_EXCEPTION_LOCAL_UNKNOWN,
    XIR_VM_DEBUG_EXCEPTION_LOCAL_MATCH,
    XIR_VM_DEBUG_EXCEPTION_LOCAL_PROPAGATE
} XirVmDebugExceptionLocalKind;
/* A selector result in the throwing function. It predicts neither cleanup
 * completion nor caller/native/Task propagation and grants no frame authority. */
typedef struct XirVmDebugExceptionLocal {
    XirVmDebugExceptionLocalKind kind;
    uint32_t function,handler,accepted;
} XirVmDebugExceptionLocal;
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
XR_FUNC void xir_vm_debug_faulted(XrXirVmDebug *,const XrXirCallView *);
/* Between driver quanta only. Copies authenticated live activation identities,
 * never frame state or values. Failure preserves both outputs. Before the first
 * admitted VM ingress there is no complete Call observer inventory. */
XR_FUNC XrXirValueStatus xir_vm_debug_threads(XrXirVmDebug *,uint64_t *,uint32_t,uint32_t *);
/* Before target construction only. Verify Source selector correspondence with
 * the exact artifact and copy coordinates/routes on its original finite compiler
 * ledger. No parser/product borrow or execution authority comes from the map. */
XR_FUNC XrXirStatus xir_vm_debug_source_map(XrXirVmDebug *,const XrXirCompileContext *,const XrXirSourceDebugView *);
/* Only actual matching logical rule groups are captured. A pause, step, throw
 * or ungrouped breakpoint is unfiltered and cannot be silently discarded. */
XR_FUNC XrXirValueStatus xir_vm_debug_snapshot_cause(const XrXirVmDebugSnapshot *,XirVmDebugStopCause *);
XR_FUNC XrXirValueStatus xir_vm_debug_snapshot_exception_local(const XrXirVmDebugSnapshot *,XirVmDebugExceptionLocal *);
/* Copies only the captured payload of a published, admitted module slot. A
 * retained snapshot never supplies a live frame or module permission. */
XR_FUNC XrXirValueStatus xir_vm_debug_snapshot_slot(const XrXirVmDebugSnapshot *,uint32_t,uint32_t,XrXirValue *);
/* Resume a filtered logical rule visit once, retaining the original step
 * thread/frame/depth/source anchor. Stale or unfiltered stops stay paused. */
XR_FUNC XrXirValueStatus xir_vm_debug_continue_filtered(XrXirVmDebug *,uint64_t);
/* Queue an owned replacement and copied typed path for a stopped Source operand.
 * An empty path requires a Cell; a value root must cross a Class identity.
 * A fresh authenticated VM callback
 * checks declaration writes and ordinary Instance/Call authority before commit.
 * Every begin must be paired with finish, including driver failure/cleanup. */
XR_FUNC XrXirValueStatus xir_vm_debug_write_begin(XrXirVmDebug *,uint64_t,uint64_t,uint32_t,const XrXirValuePath *,const XrXirValue *);
XR_FUNC XrXirValueStatus xir_vm_debug_write_slot_begin(XrXirVmDebug *,uint64_t,uint64_t,uint32_t,const XrXirValuePath *,const XrXirValue *);
XR_FUNC bool xir_vm_debug_write_pending(const XrXirVmDebug *);
XR_FUNC XrXirValueStatus xir_vm_debug_write_visit(XrXirVmDebug *);
/* A successful applied write transfers its prepared snapshot even if the
 * enclosing driver later failed. Driver outcome and write outcome are distinct. */
XR_FUNC XrXirValueStatus xir_vm_debug_write_finish(XrXirVmDebug *,bool *,XrXirVmDebugSnapshot **);
#endif // XXIR_VM_DEBUG_INTERNAL_H
