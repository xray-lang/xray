/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_typed_dispatch.c - Typed TargetPlan scalar executor
 *
 * KEY CONCEPT:
 *   The dispatcher consumes only a verified function-local instruction table
 *   and exact typed slots. It does not inspect SemanticPlan or Xi and has no
 *   legacy bytecode, XrValue, AOT, or CGen fallback.
 */

#include "xr_typed_dispatch.h"
#include "xr_typed_frame.h"
#include "../plan/target/xr_target_instruction_verify.h"
#include "../shared/xr_bits_core.h"
#include "../shared/xr_int_arith_core.h"
#include "../shared/xr_semantic_owner_ids_gen.h"
#include <string.h>

static XrTypedDispatchStatus describe_i64(XrTypedFrame *frame, uint32_t slot,
                                          XrTypedSlotAccess *access) {
    if (xr_typed_frame_describe_slot(frame, slot, access) != XR_TYPED_FRAME_OK ||
        access->size != sizeof(uint64_t) || access->alignment != sizeof(uint64_t))
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    return XR_TYPED_DISPATCH_OK;
}

static XrTypedDispatchStatus load_i64_bits(XrTypedFrame *frame, uint32_t slot,
                                           uint64_t *bits) {
    XrTypedSlotAccess access = {0};
    XrTypedDispatchStatus status = describe_i64(frame, slot, &access);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    return xr_typed_frame_load(frame, &access, bits, sizeof(*bits)) ==
                   XR_TYPED_FRAME_OK
               ? XR_TYPED_DISPATCH_OK
               : XR_TYPED_DISPATCH_FRAME_ERROR;
}

static XrTypedDispatchStatus store_i64_bits(XrTypedFrame *frame, uint32_t slot,
                                            uint64_t bits) {
    XrTypedSlotAccess access = {0};
    XrTypedDispatchStatus status = describe_i64(frame, slot, &access);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    return xr_typed_frame_store(frame, &access, &bits, sizeof(bits)) ==
                   XR_TYPED_FRAME_OK
               ? XR_TYPED_DISPATCH_OK
               : XR_TYPED_DISPATCH_FRAME_ERROR;
}

/*
 * Executes one row. `next` arrives holding the row that follows in the table
 * and is rewritten only by a jump or a branch, so control flow is explicit
 * here rather than implied by the caller's loop. `returned` is set only by the
 * row that ends the program.
 */
static XrTypedDispatchStatus execute_row(XrTypedFrame *frame,
                                         const XrTargetInstructionRecord *row,
                                         const int64_t *arguments,
                                         uint32_t argument_count,
                                         uint32_t row_count, uint32_t *next,
                                         bool *returned,
                                         uint64_t *return_bits) {
    uint64_t left = 0;
    uint64_t right = 0;
    XrTypedDispatchStatus status = XR_TYPED_DISPATCH_OK;
    switch ((XrTargetInstructionOpcode) row->opcode) {
        case XR_TARGET_INSTRUCTION_CONST_I64:
            return store_i64_bits(frame, row->result_slot, row->immediate_bits);
        case XR_TARGET_INSTRUCTION_PARAM_I64:
            /* The immediate is the argument ordinal the verifier proved dense;
             * the bound is repeated here so a caller vector shorter than the
             * program can never read past its end. */
            if (!arguments || row->immediate_bits >= argument_count)
                return XR_TYPED_DISPATCH_ARGUMENT_MISMATCH;
            memcpy(&left, &arguments[row->immediate_bits], sizeof(left));
            return store_i64_bits(frame, row->result_slot, left);
        case XR_TARGET_INSTRUCTION_COPY_I64:
            status = load_i64_bits(frame, row->operand_slots[0], &left);
            return status == XR_TYPED_DISPATCH_OK
                       ? store_i64_bits(frame, row->result_slot, left)
                       : status;
        case XR_TARGET_INSTRUCTION_NEG_WRAP_I64:
            status = load_i64_bits(frame, row->operand_slots[0], &left);
            if (status != XR_TYPED_DISPATCH_OK)
                return status;
            /* Negation wraps like the other arithmetic rows: the plan carries
             * wrapping semantics, so INT64_MIN negates to itself rather than
             * trapping. */
            return store_i64_bits(frame, row->result_slot, (uint64_t) (0 - left));
        case XR_TARGET_INSTRUCTION_BNOT_I64:
            status = load_i64_bits(frame, row->operand_slots[0], &left);
            if (status != XR_TYPED_DISPATCH_OK)
                return status;
            return store_i64_bits(frame, row->result_slot, ~left);
        case XR_TARGET_INSTRUCTION_ADD_WRAP_I64:
        case XR_TARGET_INSTRUCTION_SUB_WRAP_I64:
        case XR_TARGET_INSTRUCTION_MUL_WRAP_I64:
        case XR_TARGET_INSTRUCTION_BAND_I64:
        case XR_TARGET_INSTRUCTION_BOR_I64:
        case XR_TARGET_INSTRUCTION_BXOR_I64:
            status = load_i64_bits(frame, row->operand_slots[0], &left);
            if (status != XR_TYPED_DISPATCH_OK)
                return status;
            status = load_i64_bits(frame, row->operand_slots[1], &right);
            if (status != XR_TYPED_DISPATCH_OK)
                return status;
            if (row->opcode == XR_TARGET_INSTRUCTION_ADD_WRAP_I64)
                left += right;
            else if (row->opcode == XR_TARGET_INSTRUCTION_SUB_WRAP_I64)
                left -= right;
            else if (row->opcode == XR_TARGET_INSTRUCTION_MUL_WRAP_I64)
                left *= right;
            else if (row->opcode == XR_TARGET_INSTRUCTION_BAND_I64)
                left &= right;
            else if (row->opcode == XR_TARGET_INSTRUCTION_BOR_I64)
                left |= right;
            else
                left ^= right;
            return store_i64_bits(frame, row->result_slot, left);
        case XR_TARGET_INSTRUCTION_SHL_MASKED_I64:
        case XR_TARGET_INSTRUCTION_SHR_ARITH_MASKED_I64: {
            status = load_i64_bits(frame, row->operand_slots[0], &left);
            if (status != XR_TYPED_DISPATCH_OK)
                return status;
            status = load_i64_bits(frame, row->operand_slots[1], &right);
            if (status != XR_TYPED_DISPATCH_OK)
                return status;
            int64_t value = 0;
            int64_t count = 0;
            memcpy(&value, &left, sizeof(value));
            memcpy(&count, &right, sizeof(count));
            /* The shared shift owner is the single definition of the count
             * rule: it takes the count modulo 64, so this row is defined for
             * every i64 count and agrees exactly with the bytecode VM, the AOT
             * runtime, and constant folding. Going through it is also what
             * keeps the executor out of C's undefined shift. */
            int64_t shifted = XR_SHIFT_OWNER_APPLY(
                XR_SEM_OWNER_ID_SHARED_SHIFT_HI, XR_SEM_OWNER_ID_SHARED_SHIFT_LO,
                XR_SEM_CONSUMER_VM,
                row->opcode == XR_TARGET_INSTRUCTION_SHL_MASKED_I64
                    ? XR_SHIFT_LEFT
                    : XR_SHIFT_RIGHT_SIGNED,
                value, count);
            memcpy(&left, &shifted, sizeof(left));
            return store_i64_bits(frame, row->result_slot, left);
        }
        case XR_TARGET_INSTRUCTION_DIV_TRAP_I64:
        case XR_TARGET_INSTRUCTION_MOD_TRAP_I64: {
            status = load_i64_bits(frame, row->operand_slots[0], &left);
            if (status != XR_TYPED_DISPATCH_OK)
                return status;
            status = load_i64_bits(frame, row->operand_slots[1], &right);
            if (status != XR_TYPED_DISPATCH_OK)
                return status;
            int64_t dividend = 0;
            int64_t divisor = 0;
            memcpy(&dividend, &left, sizeof(dividend));
            memcpy(&divisor, &right, sizeof(divisor));
            bool dividing = row->opcode == XR_TARGET_INSTRUCTION_DIV_TRAP_I64;
            /* The divisor is a runtime value, so this is the only place the
             * zero can be caught. The program stops here with the status that
             * names the operator, leaving the frame untouched, rather than
             * executing a division C leaves undefined. */
            if (divisor == 0)
                return dividing ? XR_TYPED_DISPATCH_DIVIDE_BY_ZERO
                                : XR_TYPED_DISPATCH_MODULO_BY_ZERO;
            /* INT64_MIN by -1 is the other C undefined case, and it is defined
             * rather than refused: the shared helpers give the wrapping negate
             * and the zero remainder that the bytecode VM, the AOT runtime,
             * and constant folding all produce for it. */
            /* The owner macro takes a constant kind, so the two rows resolve
             * it separately rather than selecting one at run time. */
            XrIntDivModResult evaluated =
                dividing ? XR_INT_DIV_MOD_OWNER_APPLY(
                               XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_HI,
                               XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_LO,
                               XR_SEM_CONSUMER_VM, XR_INT_DIV_MOD_DIV,
                               XR_INT_DIV_MOD_PROOF_NONZERO, dividend, divisor)
                         : XR_INT_DIV_MOD_OWNER_APPLY(
                               XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_HI,
                               XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_LO,
                               XR_SEM_CONSUMER_VM, XR_INT_DIV_MOD_MOD,
                               XR_INT_DIV_MOD_PROOF_NONZERO, dividend, divisor);
            int64_t computed = evaluated.value;
            memcpy(&left, &computed, sizeof(left));
            return store_i64_bits(frame, row->result_slot, left);
        }
        case XR_TARGET_INSTRUCTION_RETURN_I64:
            *returned = true;
            return load_i64_bits(frame, row->operand_slots[0], return_bits);
        case XR_TARGET_INSTRUCTION_JUMP:
        case XR_TARGET_INSTRUCTION_BRANCH_IF_NONZERO_I64: {
            uint32_t target = XR_TARGET_INSTRUCTION_TARGET_IF_NONZERO(
                row->immediate_bits);
            if (row->opcode == XR_TARGET_INSTRUCTION_BRANCH_IF_NONZERO_I64) {
                status = load_i64_bits(frame, row->operand_slots[0], &left);
                if (status != XR_TYPED_DISPATCH_OK)
                    return status;
                /* The condition is compared against zero as a bit pattern, so
                 * every i64 value selects an edge and no value is outside the
                 * test. */
                if (left == 0)
                    target = XR_TARGET_INSTRUCTION_TARGET_IF_ZERO(
                        row->immediate_bits);
            }
            /* The verifier proved the target is the first row of a block of
             * this same group; the bound is repeated here so a table that
             * changed underneath cannot move the instruction pointer out of
             * the group. */
            if (target >= row_count)
                return XR_TYPED_DISPATCH_PROGRAM_INVALID;
            *next = target;
            return XR_TYPED_DISPATCH_OK;
        }
        default:
            return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    }
}

XrTypedDispatchStatus xr_typed_dispatch_execute_i64(
    const XrTargetPlan *verified_plan,
    const XrFingerprint *required_plan_fingerprint, uint32_t function,
    const int64_t *arguments, uint32_t argument_count, int64_t *result) {
    if (result)
        *result = 0;
    if (!verified_plan || !required_plan_fingerprint || !result ||
        (!arguments && argument_count))
        return XR_TYPED_DISPATCH_INVALID_ARGUMENT;
    if (!xr_target_plan_is_verified(verified_plan))
        return XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED;
    if (!xr_fingerprint_equal(xr_target_plan_fingerprint(verified_plan),
                              *required_plan_fingerprint))
        return XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH;
    char error[512] = {0};
    if (!xr_target_plan_fingerprint_is_intact(verified_plan) ||
        !xr_target_instruction_program_verify(verified_plan, error,
                                               sizeof(error)))
        return XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED;
    if (xr_target_plan_function_execution_family_mask(verified_plan, function) !=
        XR_TARGET_EXECUTION_SCALAR_I64_CLOSED)
        return XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE;

    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *instructions =
        xr_target_plan_function_instructions(verified_plan, function,
                                             &instruction_count);
    if (!instructions || !instruction_count)
        return XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE;
    /* The verified group is the only declaration of the signature this
     * executor honours: it binds one dense argument ordinal per parameter
     * row. A caller vector of any other length is refused before the frame
     * exists, so no slot is ever filled from a truncated or padded vector. */
    uint32_t declared_parameters = 0;
    for (uint32_t i = 0; i < instruction_count; i++)
        declared_parameters +=
            instructions[i].opcode == XR_TARGET_INSTRUCTION_PARAM_I64;
    if (declared_parameters != argument_count)
        return XR_TYPED_DISPATCH_ARGUMENT_MISMATCH;
    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    XrTypedFrame *frame = NULL;
    if (xr_typed_frame_create(verified_plan, required_plan_fingerprint, function,
                              &limits, &frame) != XR_TYPED_FRAME_OK)
        return XR_TYPED_DISPATCH_FRAME_ERROR;

    /* The instruction pointer starts at the group's first row, which the
     * verifier proved is the entry block, and moves only where a row says. A
     * fixed step budget bounds a program the verifier had no reason to refuse
     * but that loops forever, so the executor returns instead of hanging. */
    uint64_t return_bits = 0;
    uint32_t row = 0;
    bool returned = false;
    XrTypedDispatchStatus status = XR_TYPED_DISPATCH_OK;
    for (uint32_t step = 0; step < XR_TYPED_DISPATCH_MAX_STEPS && !returned;
         step++) {
        if (row >= instruction_count) {
            status = XR_TYPED_DISPATCH_PROGRAM_INVALID;
            break;
        }
        uint32_t next = row + 1u;
        status = execute_row(frame, &instructions[row], arguments,
                             argument_count, instruction_count, &next, &returned,
                             &return_bits);
        if (status != XR_TYPED_DISPATCH_OK)
            break;
        row = next;
    }
    if (status == XR_TYPED_DISPATCH_OK && !returned)
        status = XR_TYPED_DISPATCH_STEP_LIMIT_EXCEEDED;
    xr_typed_frame_free(frame);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    memcpy(result, &return_bits, sizeof(*result));
    return XR_TYPED_DISPATCH_OK;
}
