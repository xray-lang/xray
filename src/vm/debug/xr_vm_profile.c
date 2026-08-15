/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_vm_profile.c - Read-only typed TargetPlan execution counters
 */

#include "xr_vm_profile.h"
#include <string.h>

static void increment_saturating(uint64_t *counter, uint32_t *saturated) {
    if (*counter == UINT64_MAX) {
        *saturated = 1;
        return;
    }
    (*counter)++;
}

bool xr_typed_profile_init(XrVmProfile *profile) {
    if (!profile)
        return false;
    memset(profile, 0, sizeof(*profile));
    profile->schema_version = XR_VM_PROFILE_SCHEMA_VERSION;
    return true;
}

bool xr_typed_profile_is_initialized(const XrVmProfile *profile) {
    return profile &&
           profile->schema_version == XR_VM_PROFILE_SCHEMA_VERSION;
}

bool xr_typed_profile_snapshot(const XrVmProfile *profile,
                            XrVmProfileSnapshot *snapshot) {
    if (snapshot)
        memset(snapshot, 0, sizeof(*snapshot));
    if (!xr_typed_profile_is_initialized(profile) || !snapshot)
        return false;
    snapshot->schema_version = profile->schema_version;
    snapshot->saturated = profile->saturated;
    memcpy(snapshot->event_counts, profile->event_counts,
           sizeof(snapshot->event_counts));
    memcpy(snapshot->opcode_counts, profile->opcode_counts,
           sizeof(snapshot->opcode_counts));
    snapshot->max_frame_depth = profile->max_frame_depth;
    return true;
}

void xr_typed_profile_record_event(XrVmProfile *profile,
                                const XrVmTraceEvent *event) {
    if (!xr_typed_profile_is_initialized(profile) || !event ||
        event->kind >= XR_VM_TRACE_EVENT_KIND_COUNT)
        return;
    increment_saturating(&profile->event_counts[event->kind],
                         &profile->saturated);
    if (event->kind == XR_VM_TRACE_INSTRUCTION &&
        event->opcode > XR_TARGET_INSTRUCTION_INVALID &&
        event->opcode < XR_TARGET_INSTRUCTION_COUNT)
        increment_saturating(&profile->opcode_counts[event->opcode],
                             &profile->saturated);
    if (event->frame_depth > profile->max_frame_depth)
        profile->max_frame_depth = event->frame_depth;
}
