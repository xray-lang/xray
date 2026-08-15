/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_instruction_verify.h - Independent typed instruction verifier
 */

#ifndef XR_TARGET_INSTRUCTION_VERIFY_H
#define XR_TARGET_INSTRUCTION_VERIFY_H

#include "xr_target_plan.h"

XR_FUNC bool xr_target_instruction_program_verify(const XrTargetPlan *plan,
                                                   char *error,
                                                   size_t error_size);

/*
 * The one control-flow judgement for a function's row group, owned here and
 * reused by the production builder so that admission and verification cannot
 * drift apart. It derives the basic-block
 * partition from the terminators, proves every jump target is a block entry of
 * this same group, proves every block is reachable from the entry, and proves
 * by a definite-assignment fixed point that no operand is read on any path that
 * does not already define it, including the implicit caller-slot reads of a
 * call-record row. Slot indexes are interpreted relative to the
 * function's own slot range, and jump targets relative to the group's first
 * row. Anything it cannot prove is refused.
 */
XR_FUNC bool xr_target_instruction_rows_control_flow_is_exact(
    const XrTargetInstructionRecord *rows, uint32_t row_count,
    uint32_t slot_begin, uint32_t slot_count,
    const XrTargetCallRecord *calls, uint32_t call_count,
    const XrTargetCallArgumentRecord *call_arguments,
    uint32_t call_argument_count,
    const XrTargetEntryExpectationRecord *entry_expectations,
    uint32_t entry_expectation_count);

#endif  // XR_TARGET_INSTRUCTION_VERIFY_H
