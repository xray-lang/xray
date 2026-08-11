/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_typed_dispatch.c - Test-only typed TargetPlan scalar executor
 *
 * KEY CONCEPT:
 *   The dispatcher consumes only a verified function-local instruction table
 *   and exact typed slots. It does not inspect SemanticPlan or Xi and has no
 *   legacy bytecode, XrValue, AOT, or CGen fallback.
 */

#include "xr_typed_dispatch.h"
#include "xr_typed_frame.h"
#include "../plan/target/xr_target_instruction_verify.h"
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

static XrTypedDispatchStatus execute_row(XrTypedFrame *frame,
                                         const XrTargetInstructionRecord *row,
                                         uint64_t *return_bits) {
    uint64_t left = 0;
    uint64_t right = 0;
    XrTypedDispatchStatus status = XR_TYPED_DISPATCH_OK;
    switch ((XrTargetInstructionOpcode) row->opcode) {
        case XR_TARGET_INSTRUCTION_CONST_I64:
            return store_i64_bits(frame, row->result_slot, row->immediate_bits);
        case XR_TARGET_INSTRUCTION_COPY_I64:
            status = load_i64_bits(frame, row->operand_slots[0], &left);
            return status == XR_TYPED_DISPATCH_OK
                       ? store_i64_bits(frame, row->result_slot, left)
                       : status;
        case XR_TARGET_INSTRUCTION_ADD_WRAP_I64:
        case XR_TARGET_INSTRUCTION_SUB_WRAP_I64:
        case XR_TARGET_INSTRUCTION_MUL_WRAP_I64:
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
            else
                left *= right;
            return store_i64_bits(frame, row->result_slot, left);
        case XR_TARGET_INSTRUCTION_RETURN_I64:
            return load_i64_bits(frame, row->operand_slots[0], return_bits);
        default:
            return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    }
}

XrTypedDispatchStatus xr_typed_dispatch_execute_i64(
    const XrTargetPlan *verified_plan,
    const XrFingerprint *required_plan_fingerprint, uint32_t function,
    int64_t *result) {
    if (result)
        *result = 0;
    if (!verified_plan || !required_plan_fingerprint || !result)
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
        XR_TARGET_EXECUTION_SCALAR_I64_STRAIGHT_LINE)
        return XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE;

    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *instructions =
        xr_target_plan_function_instructions(verified_plan, function,
                                             &instruction_count);
    if (!instructions || !instruction_count)
        return XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE;
    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    XrTypedFrame *frame = NULL;
    if (xr_typed_frame_create(verified_plan, required_plan_fingerprint, function,
                              &limits, &frame) != XR_TYPED_FRAME_OK)
        return XR_TYPED_DISPATCH_FRAME_ERROR;

    uint64_t return_bits = 0;
    XrTypedDispatchStatus status = XR_TYPED_DISPATCH_OK;
    for (uint32_t i = 0; i < instruction_count; i++) {
        status = execute_row(frame, &instructions[i], &return_bits);
        if (status != XR_TYPED_DISPATCH_OK)
            break;
    }
    xr_typed_frame_free(frame);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    memcpy(result, &return_bits, sizeof(*result));
    return XR_TYPED_DISPATCH_OK;
}
