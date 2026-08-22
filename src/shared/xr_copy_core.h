/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_copy_core.h - Runtime-neutral Xi copy semantics.
 *
 * Xi copy values preserve the source value but do not all have the same
 * ownership or optimization meaning. The semantic immediate distinguishes
 * borrowed forwarding, independent value cloning, and cell and cleanup reads.
 * Enum metadata forwarding is an explicit identity variant so optimizers
 * cannot treat it as an ordinary alias.
 */

#ifndef XR_COPY_CORE_H
#define XR_COPY_CORE_H

#include "xr_semantic_owner_ids_gen.h"
#include <stdbool.h>
#include <stdint.h>

#define XI_COPY_KIND_IDENTITY INT64_C(0)
#define XI_COPY_KIND_VALUE_CLONE INT64_C(0x58434F5059434C4E)
#define XI_COPY_KIND_CELL_READ INT64_C(0x5843454C4C524541)
#define XI_COPY_KIND_CLEANUP_RETURN INT64_C(0x58434C4E52545552)

typedef enum XrCopySemanticKind {
    XR_COPY_SEMANTIC_INVALID = 0,
    XR_COPY_SEMANTIC_IDENTITY,
    XR_COPY_SEMANTIC_VALUE_CLONE,
    XR_COPY_SEMANTIC_CELL_READ,
    XR_COPY_SEMANTIC_CLEANUP_RETURN,
    XR_COPY_SEMANTIC_ENUM_METADATA_FORWARD
} XrCopySemanticKind;

typedef struct XrCopyPlan {
    XrCopySemanticKind kind;
    bool preserves_value;
    bool borrows_source;
    bool requires_independent_value;
} XrCopyPlan;

static inline XrCopyPlan xr_copy_plan_core(int64_t semantic_immediate,
                                           bool has_enum_metadata) {
    XrCopyPlan plan = {XR_COPY_SEMANTIC_INVALID, false, false, false};
    if (semantic_immediate == XI_COPY_KIND_IDENTITY) {
        plan.kind = has_enum_metadata ? XR_COPY_SEMANTIC_ENUM_METADATA_FORWARD
                                      : XR_COPY_SEMANTIC_IDENTITY;
    } else if (has_enum_metadata) {
        return plan;
    } else if (semantic_immediate == XI_COPY_KIND_VALUE_CLONE) {
        plan.kind = XR_COPY_SEMANTIC_VALUE_CLONE;
        plan.requires_independent_value = true;
    } else if (semantic_immediate == XI_COPY_KIND_CELL_READ) {
        plan.kind = XR_COPY_SEMANTIC_CELL_READ;
    } else if (semantic_immediate == XI_COPY_KIND_CLEANUP_RETURN) {
        plan.kind = XR_COPY_SEMANTIC_CLEANUP_RETURN;
    } else {
        return plan;
    }
    plan.preserves_value = true;
    plan.borrows_source = !plan.requires_independent_value;
    return plan;
}

static inline bool xr_copy_plan_is_exact_core(XrCopyPlan plan) {
    if (plan.kind <= XR_COPY_SEMANTIC_INVALID ||
        plan.kind > XR_COPY_SEMANTIC_ENUM_METADATA_FORWARD || !plan.preserves_value)
        return false;
    if (plan.kind == XR_COPY_SEMANTIC_VALUE_CLONE)
        return plan.requires_independent_value && !plan.borrows_source;
    return plan.borrows_source && !plan.requires_independent_value;
}

#define XR_COPY_OWNER_GUARD(owner_hi, owner_lo)                                                   \
    ((void) sizeof(struct {                                                                       \
        unsigned int owner_id_must_be_shared_copy                                                \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_COPY_HI &&                       \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_COPY_LO)                         \
                   ? 1                                                                           \
                   : -1);                                                                        \
    }))

#define XR_COPY_CONSUMER_GUARD(consumer_bit)                                                      \
    ((void) sizeof(struct {                                                                       \
        unsigned int consumer_must_be_declared_for_shared_copy                                   \
            : (((uint32_t) (consumer_bit) != 0 &&                                                \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&          \
                (XR_SEM_OWNER_ID_SHARED_COPY_CONSUMERS & (uint32_t) (consumer_bit)) != 0)        \
                   ? 1                                                                           \
                   : -1);                                                                        \
    }))

#define XR_COPY_OWNER_PLAN(owner_hi, owner_lo, consumer_bit, semantic_immediate,                 \
                           has_enum_metadata)                                                     \
    (XR_COPY_OWNER_GUARD((owner_hi), (owner_lo)),                                                 \
     XR_COPY_CONSUMER_GUARD((consumer_bit)),                                                      \
     xr_copy_plan_core((semantic_immediate), (has_enum_metadata)))

#endif /* XR_COPY_CORE_H */
