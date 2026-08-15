/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_vm_profile.h - Read-only typed TargetPlan execution counters
 *
 * KEY CONCEPT:
 *   A profile observes the canonical trace event stream. It owns no runtime
 *   object and cannot change plan legality, frame contents, or generation
 *   lifetime.
 */

#ifndef XR_VM_PROFILE_H
#define XR_VM_PROFILE_H

#include "xr_vm_trace.h"

#define XR_VM_PROFILE_SCHEMA_VERSION UINT32_C(1)

struct XrVmProfile {
    uint32_t schema_version;
    uint32_t saturated;
    uint64_t event_counts[XR_VM_TRACE_EVENT_KIND_COUNT];
    uint64_t opcode_counts[XR_TARGET_INSTRUCTION_COUNT];
    uint64_t max_frame_depth;
};

typedef struct XrVmProfileSnapshot {
    uint32_t schema_version;
    uint32_t saturated;
    uint64_t event_counts[XR_VM_TRACE_EVENT_KIND_COUNT];
    uint64_t opcode_counts[XR_TARGET_INSTRUCTION_COUNT];
    uint64_t max_frame_depth;
} XrVmProfileSnapshot;

XR_FUNC bool xr_typed_profile_init(XrVmProfile *profile);
XR_FUNC bool xr_typed_profile_snapshot(const XrVmProfile *profile,
                                    XrVmProfileSnapshot *snapshot);
XR_FUNC bool xr_typed_profile_is_initialized(const XrVmProfile *profile);
XR_FUNC void xr_typed_profile_record_event(XrVmProfile *profile,
                                        const XrVmTraceEvent *event);

#endif  // XR_VM_PROFILE_H
