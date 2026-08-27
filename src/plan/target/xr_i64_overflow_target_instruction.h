/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Function-qualified TargetPlan authority for sealed i64 overflow predicates.
 */

#ifndef XR_I64_OVERFLOW_TARGET_INSTRUCTION_H
#define XR_I64_OVERFLOW_TARGET_INSTRUCTION_H

#include "xr_target_plan.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

XR_FUNC bool xr_i64_overflow_target_predicate_project(
    const XrSemanticPlan *semantic, uint32_t operation, uint32_t target_function,
    uint32_t result_slot, uint32_t receiver_slot, uint32_t argument_slot,
    uint32_t row_id, XrTargetI64OverflowPredicateRecord *out);

XR_FUNC bool xr_i64_overflow_target_program_verify(
    const XrTargetPlan *plan, char *error, size_t error_size);

#endif  // XR_I64_OVERFLOW_TARGET_INSTRUCTION_H
