/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#ifndef XAOT_ENTRY_PLAN_H
#define XAOT_ENTRY_PLAN_H

#include "../analysis/xglobal_summary.h"
#include "../base/xdefs.h"
#include <stdbool.h>
#include <stdint.h>

struct XaotBundle;

typedef enum XaotRootRepresentation {
    XAOT_ROOT_ELIDED = 0,
    XAOT_ROOT_DESCRIPTOR,
    XAOT_ROOT_RESUMABLE_FRAME,
} XaotRootRepresentation;

typedef enum XaotSchedulerMode {
    XAOT_SCHED_NONE = 0,
    XAOT_SCHED_SINGLE,
    XAOT_SCHED_MULTI,
} XaotSchedulerMode;

enum {
    XAOT_PROVIDER_ABI_VERSION = 1,
    XAOT_PROVIDER_HOOK_TASK_ALLOC = 1u << 0,
    XAOT_PROVIDER_HOOK_SUBMIT = 1u << 1,
    XAOT_PROVIDER_HOOK_PARK_WAKE = 1u << 2,
    XAOT_PROVIDER_HOOK_TIMER = 1u << 3,
    XAOT_PROVIDER_HOOK_EXECUTOR_PUMP = 1u << 4,
};

typedef struct XaotTargetCapabilityProvider {
    uint32_t abi_version;
    uint32_t provided_capability_bits;
    uint32_t hook_bits;
    uint64_t target_metadata_hash;
} XaotTargetCapabilityProvider;

enum {
    XAOT_ENTRY_EV_GLOBAL_BODY = 1u << 0,
    XAOT_ENTRY_EV_CLOSED_WORLD_REACHABILITY = 1u << 1,
    XAOT_ENTRY_EV_ROOT_EFFECT = 1u << 2,
    XAOT_ENTRY_EV_TARGET_PROVIDER = 1u << 3,
};

typedef enum XaotEntryUnprovenReason {
    XAOT_ENTRY_PROVEN = 0,
    XAOT_ENTRY_NO_ROOT_BODY,
    XAOT_ENTRY_PROVIDER_ABI,
    XAOT_ENTRY_MISSING_CAPABILITY,
    XAOT_ENTRY_MODULE_INIT_SUSPENDS,
} XaotEntryUnprovenReason;

typedef struct XaotEntryPlan {
    XgFuncId entry_func_id;
    uint32_t reachable_body_count;
    uint32_t reachable_effect_bits;
    uint32_t required_capability_bits;
    uint32_t provided_capability_bits;
    uint32_t runtime_component_bits;
    uint32_t provider_hook_bits;
    uint32_t evidence;
    uint8_t root_representation;
    uint8_t scheduler_mode;
    uint8_t unproven_reason;
} XaotEntryPlan;

XR_FUNC bool xaot_entry_plan_derive(const struct XaotBundle *bundle,
                                    const XgGlobalEvidence *evidence, uint32_t profile,
                                    XaotEntryPlan *out);
XR_FUNC const char *xaot_root_representation_name(uint8_t value);
XR_FUNC const char *xaot_scheduler_mode_name(uint8_t value);
XR_FUNC const char *xaot_entry_unproven_reason_name(uint8_t value);

#endif /* XAOT_ENTRY_PLAN_H */
