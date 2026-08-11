/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_plan_internal.h - Mutable construction storage for TargetPlan
 */

#ifndef XR_TARGET_PLAN_INTERNAL_H
#define XR_TARGET_PLAN_INTERNAL_H

#include "xr_target_plan.h"
#include <stdatomic.h>

struct XrTargetPlan {
    atomic_uint_least32_t references;
    uint32_t schema_version;
    bool frozen;
    bool verified;
    XrFingerprint fingerprint;
    XrFingerprint semantic_fingerprint;
    XrSemanticPlan *semantic_plan;
    XrTargetProfile *profile;
#define XR_TARGET_TABLE_FIELD(name, type)                                                          \
    type *name;                                                                                    \
    uint32_t name##_count
    XR_TARGET_TABLE_FIELD(machine_reps, XrTargetMachineRepRecord);
    XR_TARGET_TABLE_FIELD(value_reps, XrTargetValueRepRecord);
    XR_TARGET_TABLE_FIELD(extents, XrTargetExtentRecord);
    XR_TARGET_TABLE_FIELD(layouts, XrTargetLayoutRecord);
    XR_TARGET_TABLE_FIELD(fields, XrTargetFieldRecord);
    XR_TARGET_TABLE_FIELD(storage, XrTargetStorageRecord);
    XR_TARGET_TABLE_FIELD(allocations, XrTargetAllocationRecord);
    XR_TARGET_TABLE_FIELD(extent_operands, XrTargetExtentOperandRecord);
    XR_TARGET_TABLE_FIELD(functions, XrTargetFunctionRecord);
    XR_TARGET_TABLE_FIELD(slots, XrTargetSlotRecord);
    XR_TARGET_TABLE_FIELD(calls, XrTargetCallRecord);
    XR_TARGET_TABLE_FIELD(call_arguments, XrTargetCallArgumentRecord);
    XR_TARGET_TABLE_FIELD(root_maps, XrTargetRootMapRecord);
    XR_TARGET_TABLE_FIELD(root_slots, uint32_t);
    XR_TARGET_TABLE_FIELD(cleanups, XrTargetCleanupRecord);
    XR_TARGET_TABLE_FIELD(adapters, XrTargetAdapterRecord);
    XR_TARGET_TABLE_FIELD(capabilities, XrTargetCapabilityRecord);
    XR_TARGET_TABLE_FIELD(coroutines, XrTargetCoroutineStateRecord);
#undef XR_TARGET_TABLE_FIELD
};

XR_FUNC void xr_target_plan_compute_fingerprint(const XrTargetPlan *plan, XrFingerprint *out);
XR_FUNC void xr_target_layout_compute_fingerprint(const XrTargetPlan *plan, uint32_t layout,
                                                  XrFingerprint *out);
XR_FUNC void xr_target_call_compute_fingerprint(const XrTargetPlan *plan, uint32_t call,
                                                XrFingerprint *out);

#endif  // XR_TARGET_PLAN_INTERNAL_H
