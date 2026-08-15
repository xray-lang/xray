/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_vm_dynamic_entry.h - Runtime resolver boundary for typed entry calls
 */

#ifndef XR_VM_DYNAMIC_ENTRY_H
#define XR_VM_DYNAMIC_ENTRY_H

#include "../plan/target/xr_target_plan.h"
#include "../../include/xray_runtime_generation.h"

#define XR_VM_DYNAMIC_ENTRY_CONTEXT_SCHEMA_VERSION UINT32_C(2)

typedef struct XrVmDecodedCache XrVmDecodedCache;
typedef struct XrVmDynamicEntryContext XrVmDynamicEntryContext;

typedef enum XrVmDynamicEntryStatus {
    XR_VM_DYNAMIC_ENTRY_OK = 0,
    XR_VM_DYNAMIC_ENTRY_INVALID_ARGUMENT,
    XR_VM_DYNAMIC_ENTRY_AUTHORITY_MISMATCH,
    XR_VM_DYNAMIC_ENTRY_NOT_FOUND,
    XR_VM_DYNAMIC_ENTRY_BUDGET_EXCEEDED,
    XR_VM_DYNAMIC_ENTRY_RELEASE_FAILED,
} XrVmDynamicEntryStatus;

typedef struct XrVmDynamicEntryResolution {
    const XrTargetPlan *plan;
    const XrVmDecodedCache *decoded_cache;
    const XrVmDynamicEntryContext *dynamic_entries;
    XrFingerprint plan_fingerprint;
    XrModuleGenerationIdentity generation_identity;
    void *lease;
    uint32_t function;
} XrVmDynamicEntryResolution;

typedef XrVmDynamicEntryStatus (*XrVmDynamicEntryAcquireFn)(
    const XrVmDynamicEntryContext *context,
    const XrTargetPlan *caller_plan,
    const XrFingerprint *caller_fingerprint,
    const XrTargetEntryExpectationRecord *expectation, bool use_cache,
    XrVmDynamicEntryResolution *resolution);
typedef XrVmDynamicEntryStatus (*XrVmDynamicEntryReleaseFn)(
    const XrVmDynamicEntryContext *context,
    XrVmDynamicEntryResolution *resolution);
typedef XrVmDynamicEntryStatus (*XrVmDynamicEntryValidateFn)(
    const XrVmDynamicEntryContext *context,
    const XrTargetPlan *caller_plan,
    const XrFingerprint *caller_fingerprint,
    const XrModuleGenerationIdentity *caller_generation_identity);

struct XrVmDynamicEntryContext {
    uint32_t schema_version;
    uint32_t reserved;
    void *owner;
    const XrTargetPlan *verified_plan;
    XrFingerprint plan_fingerprint;
    XrModuleGenerationIdentity generation_identity;
    XrVmDynamicEntryValidateFn validate;
    XrVmDynamicEntryAcquireFn acquire;
    XrVmDynamicEntryReleaseFn release;
};

#endif  // XR_VM_DYNAMIC_ENTRY_H
