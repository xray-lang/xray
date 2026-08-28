/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_capability.h - Canonical semantic-to-target capability closure
 */

#ifndef XR_TARGET_CAPABILITY_H
#define XR_TARGET_CAPABILITY_H

#include "xr_target_plan.h"
#include "../semantic/xr_semantic_plan.h"
#include "../../runtime/value/xtype.h"

/* The built-in freestanding assertion adapter is allocation-free and has no
 * hosted object registry.  Target planning therefore admits only the frozen
 * value domains for which that adapter has exact equality and rendering.
 * Builder, verifier, and CGen consume this predicate instead of maintaining
 * independent allow-lists. */
static inline bool
xr_target_freestanding_assertion_equality_type_supported(const XrSemanticTypeRecord *type) {
    if (!type || (type->flags & XR_SEM_TYPE_NULLABLE) != 0)
        return false;
    switch ((XrTypeKind) type->kind) {
        case XR_KIND_INT:
        case XR_KIND_STRING:
        case XR_KIND_BOOL:
        case XR_KIND_NULL:
            return true;
        default:
            return false;
    }
}

static inline bool xr_target_semantic_capability_mask(const XrSemanticPlan *semantic,
                                                      uint8_t runtime_profile, uint64_t *out) {
    if (!semantic || !out ||
        (runtime_profile != XR_TARGET_RUNTIME_PROFILE_HOSTED &&
         runtime_profile != XR_TARGET_RUNTIME_PROFILE_FREESTANDING))
        return false;
    uint64_t mask = XR_TARGET_FOUNDATION_CAPABILITY_MASK;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_OUTPUT) {
            XrPrintPlan print_plan;
            if (!xr_semantic_operation_print_plan(operation, &print_plan))
                return false;
            /* Hosted output reaches the process stream the executor already
             * owns. Only freestanding lowering has to negotiate an exact
             * output-write provider operation. */
            if (runtime_profile == XR_TARGET_RUNTIME_PROFILE_FREESTANDING &&
                (print_plan.required_capabilities & XR_PRINT_CAPABILITY_OUTPUT_WRITE) != 0)
                mask |= xr_target_capability_mask(XR_TARGET_CAPABILITY_OUTPUT_WRITE);
            continue;
        }
        if (!operation || operation->intrinsic_kind != XR_SEM_INTRINSIC_ASSERTION)
            continue;
        XrAssertionPlan assertion;
        if (!xr_semantic_operation_assertion_plan(operation, &assertion))
            return false;
        /* Hosted failures are captured as the canonical exception/panic
         * channel and must not perform an external report side effect before
         * an enclosing assertPanics can observe them.  Only freestanding
         * lowering consumes the exact assertion-report provider operation. */
        if (runtime_profile == XR_TARGET_RUNTIME_PROFILE_FREESTANDING &&
            (assertion.required_capabilities & XR_ASSERTION_CAPABILITY_FAILURE_REPORT) != 0)
            mask |= xr_target_capability_mask(XR_TARGET_CAPABILITY_ASSERTION_REPORT);
        if ((assertion.required_capabilities & XR_ASSERTION_CAPABILITY_TYPED_ERROR_BOUNDARY) != 0)
            mask |= xr_target_capability_mask(XR_TARGET_CAPABILITY_TYPED_ERROR_BOUNDARY);
        if ((assertion.required_capabilities & XR_ASSERTION_CAPABILITY_PANIC_BOUNDARY) != 0)
            mask |= xr_target_capability_mask(XR_TARGET_CAPABILITY_PANIC_BOUNDARY);
    }
    *out = mask;
    return true;
}

static inline bool xr_target_plan_capability_mask(const XrTargetPlan *plan, uint64_t *out) {
    if (!plan || !out)
        return false;
    uint32_t count = 0;
    const XrTargetCapabilityRecord *records = xr_target_plan_capabilities(plan, &count);
    uint64_t mask = 0;
    for (uint32_t i = 0; i < count; i++) {
        const XrTargetCapabilityRecord *record = &records[i];
        uint64_t bit = xr_target_capability_mask(record->capability);
        if (record->id != i || bit == 0 ||
            record->provider != xr_target_capability_provider(record->capability) ||
            record->flags != XR_TARGET_CAPABILITY_REQUIRED ||
            (i != 0 && records[i - 1].capability >= record->capability) || (mask & bit) != 0)
            return false;
        mask |= bit;
    }
    *out = mask;
    return true;
}

static inline bool xr_target_capability_mask_is_backed(uint64_t capability_mask,
                                                       uint64_t provider_mask) {
    const uint64_t language_capabilities =
        XR_TARGET_CAPABILITY_MASK(XR_TARGET_CAPABILITY_TYPED_ERROR_BOUNDARY);
    return (capability_mask & ~(provider_mask | language_capabilities)) == 0;
}

#endif /* XR_TARGET_CAPABILITY_H */
