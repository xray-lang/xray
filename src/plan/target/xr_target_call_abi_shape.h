/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_call_abi_shape.h - Shared judgements for call-boundary agreement
 */

/* Shared judgement: when two machine representations describe the same call ABI.
 *
 * A caller and a callee handing a value across a call boundary have to agree on
 * how the machine carries it, and that agreement is one fact -- same kind, same
 * width, same signedness, same legal conversions. It was stated twice: once in
 * the TargetPlan builder to bind the boundary, once in its verifier to say what
 * it expected. The two listed the same eleven fields in two different orders,
 * which is what a check looks like right before one side grows a twelfth field
 * the other lacks.
 *
 * The by-value container boundary is the same story, down to a near-identical
 * comment on each copy. Its two callers reach the machine-rep table by
 * different routes -- the builder through a plan it is still materializing, the
 * verifier through a frozen one -- so the judgement takes the table and its
 * extent directly instead of either layer's container. Each caller keeps its
 * own null check on that container, because neither may be dereferenced before
 * it has been proven non-null.
 */

#ifndef XR_TARGET_CALL_ABI_SHAPE_H
#define XR_TARGET_CALL_ABI_SHAPE_H

#include "xr_target_plan.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline bool
xr_target_machine_reps_have_same_call_abi_ignoring_detail(const XrTargetMachineRepRecord *caller,
                                                          const XrTargetMachineRepRecord *callee) {
    return caller && callee && caller->kind == callee->kind &&
           caller->register_bits == callee->register_bits &&
           caller->memory_size == callee->memory_size &&
           caller->memory_align == callee->memory_align &&
           caller->signedness == callee->signedness && caller->root_kind == callee->root_kind &&
           caller->null_encoding == callee->null_encoding &&
           caller->lane_count == callee->lane_count && caller->reserved == callee->reserved &&
           memcmp(caller->legal_conversion_mask, callee->legal_conversion_mask,
                  sizeof(caller->legal_conversion_mask)) == 0;
}

static inline bool
xr_target_machine_reps_have_same_call_abi(const XrTargetMachineRepRecord *caller,
                                          const XrTargetMachineRepRecord *callee) {
    return xr_target_machine_reps_have_same_call_abi_ignoring_detail(caller, callee) &&
           caller->detail == callee->detail;
}

/* A top-level const spelling remains a distinct frozen semantic type and may
 * therefore select a distinct detail row, but a read parameter does not grant
 * the callee mutation authority over the caller's binding. The semantic layer
 * must first prove that constness is the only type difference; this judgement
 * then proves that detail is the only ABI difference and that ownership did
 * not change along with it. */
static inline bool xr_target_const_read_call_boundary(const XrTargetMachineRepRecord *machine_reps,
                                                      uint32_t machine_rep_count,
                                                      const XrTargetValueRepRecord *caller,
                                                      const XrTargetValueRepRecord *callee,
                                                      bool semantic_const_read_admission) {
    return semantic_const_read_admission && caller && callee &&
           caller->register_rep < machine_rep_count && caller->memory_rep < machine_rep_count &&
           callee->register_rep < machine_rep_count && callee->memory_rep < machine_rep_count &&
           xr_target_machine_reps_have_same_call_abi_ignoring_detail(
               &machine_reps[caller->register_rep], &machine_reps[callee->register_rep]) &&
           xr_target_machine_reps_have_same_call_abi_ignoring_detail(
               &machine_reps[caller->memory_rep], &machine_reps[callee->memory_rep]) &&
           machine_reps[caller->register_rep].ownership ==
               machine_reps[callee->register_rep].ownership &&
           machine_reps[caller->memory_rep].ownership == machine_reps[callee->memory_rep].ownership;
}

/* A reference-capable container handed over by value.
 *
 * The callee always borrows it: the allocation stays the caller's for the
 * extent of the call and the callee releases nothing. What the caller holds is
 * its own business -- a freshly built container is owned, a shared read of a
 * local is borrowed -- so the two sides agree on representation and are allowed
 * to differ in ownership alone. An Array and a String reach this boundary in
 * the same tagged carrier, so they ask this one question instead of stating the
 * same rep agreement twice in spellings that could drift. */
static inline bool xr_target_tagged_container_value_boundary(
    const XrTargetMachineRepRecord *machine_reps, uint32_t machine_rep_count,
    const XrTargetValueRepRecord *caller, const XrTargetValueRepRecord *callee,
    uint8_t callee_ownership) {
    return caller && callee && caller->register_rep < machine_rep_count &&
           caller->memory_rep < machine_rep_count && callee->register_rep < machine_rep_count &&
           callee->memory_rep < machine_rep_count &&
           xr_target_machine_reps_have_same_call_abi(&machine_reps[caller->register_rep],
                                                     &machine_reps[callee->register_rep]) &&
           xr_target_machine_reps_have_same_call_abi(&machine_reps[caller->memory_rep],
                                                     &machine_reps[callee->memory_rep]) &&
           machine_reps[caller->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
           machine_reps[caller->memory_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
           machine_reps[callee->register_rep].ownership == callee_ownership &&
           machine_reps[callee->memory_rep].ownership == callee_ownership;
}

#endif  // XR_TARGET_CALL_ABI_SHAPE_H
