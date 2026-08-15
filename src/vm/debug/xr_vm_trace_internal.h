/* Internal immutable storage for a runtime-only typed debug session. */

#ifndef XR_VM_TRACE_INTERNAL_H
#define XR_VM_TRACE_INTERNAL_H

#include "xr_vm_trace.h"

struct XrVmDebugSession {
    uint32_t schema_version;
    uint8_t generation_identity_present;
    uint8_t reserved8[3];
    XrFingerprint target_plan_fingerprint;
    XrModuleGenerationIdentity generation_identity;
    XrVmTraceSink trace;
    XrVmProfile *profile;
    XrVmDebugControl *control;
};

#endif  // XR_VM_TRACE_INTERNAL_H
