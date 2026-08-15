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
#include "../shared/xr_compare_core.h"
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

/* The truth slot a comparison writes. It is one byte, so it is the second and
 * only other width this executor reads; the width comes from the opcode, and
 * this repeats the plan's own answer rather than choosing one. */
static XrTypedDispatchStatus describe_bool(XrTypedFrame *frame, uint32_t slot,
                                           XrTypedSlotAccess *access) {
    if (xr_typed_frame_describe_slot(frame, slot, access) != XR_TYPED_FRAME_OK ||
        access->size != sizeof(uint8_t) || access->alignment != sizeof(uint8_t))
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

static XrTypedDispatchStatus load_bool_byte(XrTypedFrame *frame, uint32_t slot,
                                            uint8_t *byte) {
    XrTypedSlotAccess access = {0};
    XrTypedDispatchStatus status = describe_bool(frame, slot, &access);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    return xr_typed_frame_load(frame, &access, byte, sizeof(*byte)) ==
                   XR_TYPED_FRAME_OK
               ? XR_TYPED_DISPATCH_OK
               : XR_TYPED_DISPATCH_FRAME_ERROR;
}

static XrTypedDispatchStatus store_bool_byte(XrTypedFrame *frame, uint32_t slot,
                                             uint8_t byte) {
    XrTypedSlotAccess access = {0};
    XrTypedDispatchStatus status = describe_bool(frame, slot, &access);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    return xr_typed_frame_store(frame, &access, &byte, sizeof(byte)) ==
                   XR_TYPED_FRAME_OK
               ? XR_TYPED_DISPATCH_OK
               : XR_TYPED_DISPATCH_FRAME_ERROR;
}

typedef struct XrTypedDispatchRowContext {
    const int64_t *arguments;
    uint32_t argument_count;
    uint32_t row_count;
    uint32_t *next;
    bool *returned;
    uint64_t *return_bits;
} XrTypedDispatchRowContext;

static XrTypedDispatchStatus execute_const(
    XrTypedFrame *frame, const XrTargetInstructionRecord *row,
    const XrTargetInstructionContract *contract,
    XrTypedDispatchRowContext *context) {
    (void) contract;
    (void) context;
    return store_i64_bits(frame, row->result_slot, row->immediate_bits);
}

static XrTypedDispatchStatus execute_param(
    XrTypedFrame *frame, const XrTargetInstructionRecord *row,
    const XrTargetInstructionContract *contract,
    XrTypedDispatchRowContext *context) {
    (void) contract;
    if (!context->arguments || row->immediate_bits >= context->argument_count)
        return XR_TYPED_DISPATCH_ARGUMENT_MISMATCH;
    uint64_t bits = 0;
    memcpy(&bits, &context->arguments[row->immediate_bits], sizeof(bits));
    return store_i64_bits(frame, row->result_slot, bits);
}

static XrTypedDispatchStatus execute_copy(
    XrTypedFrame *frame, const XrTargetInstructionRecord *row,
    const XrTargetInstructionContract *contract,
    XrTypedDispatchRowContext *context) {
    (void) contract;
    (void) context;
    uint64_t bits = 0;
    XrTypedDispatchStatus status =
        load_i64_bits(frame, row->operand_slots[0], &bits);
    return status == XR_TYPED_DISPATCH_OK
               ? store_i64_bits(frame, row->result_slot, bits)
               : status;
}

static XrTypedDispatchStatus execute_unary(
    XrTypedFrame *frame, const XrTargetInstructionRecord *row,
    const XrTargetInstructionContract *contract,
    XrTypedDispatchRowContext *context) {
    (void) context;
    uint64_t bits = 0;
    XrTypedDispatchStatus status =
        load_i64_bits(frame, row->operand_slots[0], &bits);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    if (contract->dispatch_argument ==
        XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_NEG)
        bits = (uint64_t) (0 - bits);
    else if (contract->dispatch_argument ==
             XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_BNOT)
        bits = ~bits;
    else
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    return store_i64_bits(frame, row->result_slot, bits);
}

static XrTypedDispatchStatus execute_binary(
    XrTypedFrame *frame, const XrTargetInstructionRecord *row,
    const XrTargetInstructionContract *contract,
    XrTypedDispatchRowContext *context) {
    (void) context;
    uint64_t left = 0;
    uint64_t right = 0;
    XrTypedDispatchStatus status =
        load_i64_bits(frame, row->operand_slots[0], &left);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    status = load_i64_bits(frame, row->operand_slots[1], &right);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    switch ((XrTargetInstructionDispatchArgument) contract->dispatch_argument) {
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_ADD: left += right; break;
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_SUB: left -= right; break;
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_MUL: left *= right; break;
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_BAND: left &= right; break;
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_BOR: left |= right; break;
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_BXOR: left ^= right; break;
        default: return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    }
    return store_i64_bits(frame, row->result_slot, left);
}

static XrTypedDispatchStatus execute_shift(
    XrTypedFrame *frame, const XrTargetInstructionRecord *row,
    const XrTargetInstructionContract *contract,
    XrTypedDispatchRowContext *context) {
    (void) context;
    uint64_t left = 0;
    uint64_t right = 0;
    XrTypedDispatchStatus status =
        load_i64_bits(frame, row->operand_slots[0], &left);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    status = load_i64_bits(frame, row->operand_slots[1], &right);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    int64_t value = 0;
    int64_t count = 0;
    memcpy(&value, &left, sizeof(value));
    memcpy(&count, &right, sizeof(count));
    XrShiftKind kind = XR_SHIFT_LEFT;
    if (contract->dispatch_argument ==
        XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_RIGHT)
        kind = XR_SHIFT_RIGHT_SIGNED;
    else if (contract->dispatch_argument !=
             XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_LEFT)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    int64_t shifted = XR_SHIFT_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_SHIFT_HI, XR_SEM_OWNER_ID_SHARED_SHIFT_LO,
        XR_SEM_CONSUMER_VM, kind, value, count);
    memcpy(&left, &shifted, sizeof(left));
    return store_i64_bits(frame, row->result_slot, left);
}

static XrTypedDispatchStatus execute_divmod(
    XrTypedFrame *frame, const XrTargetInstructionRecord *row,
    const XrTargetInstructionContract *contract,
    XrTypedDispatchRowContext *context) {
    (void) context;
    uint64_t left = 0;
    uint64_t right = 0;
    XrTypedDispatchStatus status =
        load_i64_bits(frame, row->operand_slots[0], &left);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    status = load_i64_bits(frame, row->operand_slots[1], &right);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    int64_t dividend = 0;
    int64_t divisor = 0;
    memcpy(&dividend, &left, sizeof(dividend));
    memcpy(&divisor, &right, sizeof(divisor));
    bool dividing = contract->dispatch_argument ==
                    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_DIV;
    if (!dividing && contract->dispatch_argument !=
                         XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_MOD)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    if (divisor == 0) {
        if (contract->error_kind ==
            XR_TARGET_INSTRUCTION_ERROR_DIVIDE_BY_ZERO)
            return XR_TYPED_DISPATCH_DIVIDE_BY_ZERO;
        if (contract->error_kind ==
            XR_TARGET_INSTRUCTION_ERROR_MODULO_BY_ZERO)
            return XR_TYPED_DISPATCH_MODULO_BY_ZERO;
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    }
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

static XrTypedDispatchStatus execute_compare(
    XrTypedFrame *frame, const XrTargetInstructionRecord *row,
    const XrTargetInstructionContract *contract,
    XrTypedDispatchRowContext *context) {
    (void) context;
    XrCompareKind kind = XR_COMPARE_EQ;
    switch ((XrTargetInstructionDispatchArgument) contract->dispatch_argument) {
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_EQ: kind = XR_COMPARE_EQ; break;
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_NE: kind = XR_COMPARE_NE; break;
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_LT: kind = XR_COMPARE_LT; break;
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_LE: kind = XR_COMPARE_LE; break;
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_GT: kind = XR_COMPARE_GT; break;
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_GE: kind = XR_COMPARE_GE; break;
        default: return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    }
    uint64_t left = 0;
    uint64_t right = 0;
    XrTypedDispatchStatus status =
        load_i64_bits(frame, row->operand_slots[0], &left);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    status = load_i64_bits(frame, row->operand_slots[1], &right);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    int64_t first = 0;
    int64_t second = 0;
    memcpy(&first, &left, sizeof(first));
    memcpy(&second, &right, sizeof(second));
    bool holds = XR_COMPARE_OWNER_APPLY_I64(
        XR_SEM_OWNER_ID_SHARED_COMPARE_HI, XR_SEM_OWNER_ID_SHARED_COMPARE_LO,
        XR_SEM_CONSUMER_VM, kind, first, second);
    return store_bool_byte(frame, row->result_slot,
                           holds ? (uint8_t) 1u : (uint8_t) 0u);
}

static XrTypedDispatchStatus execute_return(
    XrTypedFrame *frame, const XrTargetInstructionRecord *row,
    const XrTargetInstructionContract *contract,
    XrTypedDispatchRowContext *context) {
    (void) contract;
    *context->returned = true;
    return load_i64_bits(frame, row->operand_slots[0], context->return_bits);
}

static XrTypedDispatchStatus execute_branch(
    XrTypedFrame *frame, const XrTargetInstructionRecord *row,
    const XrTargetInstructionContract *contract,
    XrTypedDispatchRowContext *context) {
    uint32_t target =
        XR_TARGET_INSTRUCTION_TARGET_IF_NONZERO(row->immediate_bits);
    if (contract->dispatch_argument ==
        XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_I64) {
        uint64_t bits = 0;
        XrTypedDispatchStatus status =
            load_i64_bits(frame, row->operand_slots[0], &bits);
        if (status != XR_TYPED_DISPATCH_OK)
            return status;
        if (bits == 0)
            target = XR_TARGET_INSTRUCTION_TARGET_IF_ZERO(row->immediate_bits);
    } else if (contract->dispatch_argument ==
               XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_BOOL) {
        uint8_t truth = 0;
        XrTypedDispatchStatus status =
            load_bool_byte(frame, row->operand_slots[0], &truth);
        if (status != XR_TYPED_DISPATCH_OK)
            return status;
        if (truth == 0)
            target = XR_TARGET_INSTRUCTION_TARGET_IF_ZERO(row->immediate_bits);
    } else if (contract->dispatch_argument !=
               XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_JUMP) {
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    }
    if (target >= context->row_count)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    *context->next = target;
    return XR_TYPED_DISPATCH_OK;
}

/* The generated cases make missing and duplicate opcode handlers compilation
 * errors. The contract repeats the generated handler binding at runtime so a
 * corrupted row cannot select a handler whose metadata disagrees. */
static XrTypedDispatchStatus execute_row(XrTypedFrame *frame,
                                         const XrTargetInstructionRecord *row,
                                         const int64_t *arguments,
                                         uint32_t argument_count,
                                         uint32_t row_count, uint32_t *next,
                                         bool *returned,
                                         uint64_t *return_bits) {
    const XrTargetInstructionContract *contract =
        xr_target_instruction_contract(row->opcode);
    if (!contract)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    XrTypedDispatchRowContext context = {
        arguments, argument_count, row_count, next, returned, return_bits,
    };
    switch ((XrTargetInstructionOpcode) row->opcode) {
#define XR_VM_OP(symbol, handler, kind, argument)                                                   \
        case XR_TARGET_INSTRUCTION_##symbol:                                                       \
            if (contract->dispatch_kind != XR_TARGET_INSTRUCTION_DISPATCH_##kind ||                 \
                contract->dispatch_argument !=                                                     \
                    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_##argument)                             \
                return XR_TYPED_DISPATCH_PROGRAM_INVALID;                                          \
            return execute_##handler(frame, row, contract, &context);
#include "xr_vm_ops.def"
#undef XR_VM_OP
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
