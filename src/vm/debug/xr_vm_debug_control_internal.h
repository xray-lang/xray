/* Internal dispatcher bridge for the TargetPlan-driven debugger. */

#ifndef XR_VM_DEBUG_CONTROL_INTERNAL_H
#define XR_VM_DEBUG_CONTROL_INTERNAL_H

#include "xr_vm_debug_control.h"
#include "../xr_typed_frame.h"

typedef enum XrVmDebugControlEventStatus {
    XR_VM_DEBUG_CONTROL_EVENT_OK = 0,
    XR_VM_DEBUG_CONTROL_EVENT_INVALID,
    XR_VM_DEBUG_CONTROL_EVENT_CAPTURE_FAILED,
    XR_VM_DEBUG_CONTROL_EVENT_TERMINATED,
    XR_VM_DEBUG_CONTROL_EVENT_STOP_REJECTED,
} XrVmDebugControlEventStatus;

static inline bool xr_typed_debug_snapshot_budget_reserve(uint32_t *local_count, size_t *byte_count,
                                                          size_t value_size) {
    if (!local_count || !byte_count || !value_size ||
        *local_count >= XR_VM_DEBUG_MAX_TRACKED_SLOTS ||
        *byte_count > XR_VM_DEBUG_MAX_SNAPSHOT_BYTES ||
        value_size > XR_VM_DEBUG_MAX_SNAPSHOT_BYTES - *byte_count)
        return false;
    (*local_count)++;
    *byte_count += value_size;
    return true;
}

XR_FUNC bool xr_typed_debug_control_retain(XrVmDebugControl *control);
XR_FUNC void xr_typed_debug_control_release(XrVmDebugControl *control);

XR_FUNC XrVmDebugControlEventStatus
xr_typed_debug_control_begin_execution(XrVmDebugControl *control, const XrTargetPlan *verified_plan,
                                       const XrFingerprint *required_plan_fingerprint);
XR_FUNC XrVmDebugControlEventStatus xr_typed_debug_control_push_frame(
    XrVmDebugControl *control, const XrTargetPlan *verified_plan,
    const XrFingerprint *required_plan_fingerprint,
    const XrModuleGenerationIdentity *generation_identity, XrTypedFrame *frame, uint32_t function,
    uint32_t frame_id, uint32_t parent_frame_id, bool parameters_prebound);
XR_FUNC XrVmDebugControlEventStatus xr_typed_debug_control_instruction(XrVmDebugControl *control,
                                                                       const XrVmTraceEvent *event);
XR_FUNC XrVmDebugControlEventStatus
xr_typed_debug_control_commit_row(XrVmDebugControl *control, const XrTargetInstructionRecord *row);
XR_FUNC XrVmDebugControlEventStatus xr_typed_debug_control_pop_frame(XrVmDebugControl *control,
                                                                     uint32_t frame_id);
XR_FUNC void xr_typed_debug_control_end_execution(XrVmDebugControl *control);

#endif  // XR_VM_DEBUG_CONTROL_INTERNAL_H
