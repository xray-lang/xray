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
#include "../base/xentry_plan.h"
#include <stdbool.h>
#include <stdint.h>

struct XaotBundle;

enum {
    XAOT_PROVIDER_ABI_VERSION = 1,
    XAOT_PROVIDER_HOOK_TASK_ALLOC = 1u << 0,
    XAOT_PROVIDER_HOOK_SUBMIT = 1u << 1,
    XAOT_PROVIDER_HOOK_PARK_WAKE = 1u << 2,
    XAOT_PROVIDER_HOOK_TIMER = 1u << 3,
    XAOT_PROVIDER_HOOK_EXECUTOR_PUMP = 1u << 4,
    XAOT_PROVIDER_HOOK_INTERRUPT_COMPLETE = 1u << 5,
};

typedef struct XaotTargetCapabilityProvider {
    uint32_t abi_version;
    uint32_t provided_capability_bits;
    uint32_t hook_bits;
    uint64_t target_metadata_hash;
} XaotTargetCapabilityProvider;

XR_FUNC bool xaot_entry_plan_derive(const struct XaotBundle *bundle,
                                    const XgGlobalEvidence *evidence, uint32_t profile,
                                    XrEntryPlan *out);
XR_FUNC uint32_t xaot_entry_plan_required_provider_hooks(const XrEntryPlan *plan);
/* True when any prepared function contains a parallel range/map/reduce
 * intrinsic.  The source-summary capability bits do not record this, so the
 * prepared IR is the authority; both the entry plan and the link feature set
 * must agree on it or the generated C references the parallel runtime without
 * declaring it. */
XR_FUNC bool xaot_bundle_uses_parallel_intrinsic(const struct XaotBundle *bundle);
XR_FUNC const char *xaot_root_representation_name(uint8_t value);
XR_FUNC const char *xaot_scheduler_mode_name(uint8_t value);
XR_FUNC const char *xaot_entry_unproven_reason_name(uint8_t value);

#endif /* XAOT_ENTRY_PLAN_H */
