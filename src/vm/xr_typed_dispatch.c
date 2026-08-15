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
#include "xr_vm_decoded_cache.h"
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
    struct XrTypedDispatchExecution *execution;
    const XrVmDecodedInstruction *decoded;
    bool parameters_prebound;
    uint32_t frame_id;
} XrTypedDispatchRowContext;

typedef XrTypedDispatchStatus (*XrTypedDispatchRowHandler)(
    XrTypedFrame *frame, const XrTargetInstructionRecord *row,
    const XrTargetInstructionContract *contract,
    XrTypedDispatchRowContext *context);

typedef struct XrTypedDispatchFunctionBinding {
    XrTypedDispatchRowHandler handler;
    uint8_t dispatch_kind;
    uint8_t dispatch_argument;
} XrTypedDispatchFunctionBinding;

typedef struct XrTypedDispatchExecution {
    const XrTargetPlan *plan;
    const XrFingerprint *fingerprint;
    XrTypedFrameLimits limits;
    uint32_t remaining_steps;
    uint32_t call_depth;
    uint32_t next_frame_id;
    uint64_t next_event_ordinal;
    const XrVmDebugSession *debug_session;
    const XrVmDecodedCache *decoded_cache;
    XrTypedDispatchProvider provider;
} XrTypedDispatchExecution;

static XrTypedDispatchStatus execute_function(
    XrTypedDispatchExecution *execution, XrTypedFrame *frame,
    uint32_t function, const int64_t *arguments, uint32_t argument_count,
    bool parameters_prebound, uint32_t frame_id, uint32_t parent_frame_id,
    uint64_t *return_bits);

static XrVmTraceEvent make_trace_event(XrVmTraceEventKind kind,
                                       uint32_t function, uint32_t frame,
                                       uint32_t parent_frame,
                                       uint32_t frame_depth) {
    XrVmTraceEvent event;
    memset(&event, 0, sizeof(event));
    event.kind = (uint8_t) kind;
    event.opcode = XR_TARGET_INSTRUCTION_INVALID;
    event.function = function;
    event.related_function = XR_VM_TRACE_ID_NONE;
    event.instruction = XR_VM_TRACE_ID_NONE;
    event.block = XR_VM_TRACE_ID_NONE;
    event.call = XR_VM_TRACE_ID_NONE;
    event.frame = frame;
    event.parent_frame = parent_frame;
    event.related_frame = XR_VM_TRACE_ID_NONE;
    event.frame_depth = frame_depth;
    return event;
}

static XrTypedDispatchStatus emit_trace_event(
    XrTypedDispatchExecution *execution, XrVmTraceEvent *event) {
    if (!execution->debug_session)
        return XR_TYPED_DISPATCH_OK;
    if (!xr_typed_debug_emit(execution->debug_session,
                          execution->next_event_ordinal, event))
        return XR_TYPED_DISPATCH_TRACE_REJECTED;
    execution->next_event_ordinal++;
    return XR_TYPED_DISPATCH_OK;
}

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
    if (context->parameters_prebound)
        return XR_TYPED_DISPATCH_OK;
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
    uint32_t target = context->decoded
                          ? context->decoded->target_if_nonzero
                          : XR_TARGET_INSTRUCTION_TARGET_IF_NONZERO(
                                row->immediate_bits);
    if (contract->dispatch_argument ==
        XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_I64) {
        uint64_t bits = 0;
        XrTypedDispatchStatus status =
            load_i64_bits(frame, row->operand_slots[0], &bits);
        if (status != XR_TYPED_DISPATCH_OK)
            return status;
        if (bits == 0)
            target = context->decoded
                         ? context->decoded->target_if_zero
                         : XR_TARGET_INSTRUCTION_TARGET_IF_ZERO(
                               row->immediate_bits);
    } else if (contract->dispatch_argument ==
               XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_BOOL) {
        uint8_t truth = 0;
        XrTypedDispatchStatus status =
            load_bool_byte(frame, row->operand_slots[0], &truth);
        if (status != XR_TYPED_DISPATCH_OK)
            return status;
        if (truth == 0)
            target = context->decoded
                         ? context->decoded->target_if_zero
                         : XR_TARGET_INSTRUCTION_TARGET_IF_ZERO(
                               row->immediate_bits);
    } else if (contract->dispatch_argument !=
               XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_JUMP) {
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    }
    if (target >= context->row_count)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    *context->next = target;
    return XR_TYPED_DISPATCH_OK;
}

static XrTypedDispatchStatus copy_call_arguments(
    XrTypedFrame *parent, XrTypedFrame *child,
    const XrTargetCallArgumentRecord *arguments, uint16_t argument_count) {
    for (uint16_t ordinal = 0; ordinal < argument_count; ordinal++) {
        uint64_t bits = 0;
        XrTypedDispatchStatus status =
            load_i64_bits(parent, arguments[ordinal].caller_slot, &bits);
        if (status != XR_TYPED_DISPATCH_OK)
            return status;
        status = store_i64_bits(child, arguments[ordinal].callee_slot, bits);
        if (status != XR_TYPED_DISPATCH_OK)
            return status;
    }
    return XR_TYPED_DISPATCH_OK;
}

static XrTypedDispatchStatus execute_call(
    XrTypedFrame *frame, const XrTargetInstructionRecord *row,
    const XrTargetInstructionContract *contract,
    XrTypedDispatchRowContext *context) {
    (void) contract;
    XrTypedDispatchExecution *execution = context->execution;
    if (!execution || execution->call_depth >= XR_TYPED_DISPATCH_MAX_CALL_DEPTH)
        return XR_TYPED_DISPATCH_CALL_DEPTH_EXCEEDED;
    uint32_t call_count = 0;
    uint32_t argument_count = 0;
    const XrTargetCallRecord *calls =
        xr_target_plan_calls(execution->plan, &call_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(execution->plan, &argument_count);
    uint32_t call_index = (uint32_t) row->immediate_bits;
    const XrTargetCallRecord *call =
        calls && call_index < call_count ? &calls[call_index] : NULL;
    if (!call || call->argument_begin > argument_count ||
        call->argument_count > argument_count - call->argument_begin)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;

    uint32_t child_frame_id = execution->next_frame_id++;
    XrVmTraceEvent call_enter = make_trace_event(
        XR_VM_TRACE_CALL_ENTER, row->function, context->frame_id,
        XR_VM_TRACE_ID_NONE, execution->call_depth - 1u);
    call_enter.instruction = row->id;
    call_enter.opcode = row->opcode;
    call_enter.call = call->id;
    call_enter.related_function = call->callee_function;
    call_enter.related_frame = child_frame_id;
    XrTypedDispatchStatus status =
        emit_trace_event(execution, &call_enter);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;

    XrTypedFrame *child = NULL;
    bool linked = false;
    status = XR_TYPED_DISPATCH_FRAME_ERROR;
    if (xr_typed_frame_create(execution->plan, execution->fingerprint,
                              call->callee_function, &execution->limits,
                              &child) != XR_TYPED_FRAME_OK)
        goto cleanup;
    if (execution->debug_session &&
        execution->debug_session->generation_identity_present &&
        xr_typed_frame_bind_generation_identity(
            child, &execution->debug_session->generation_identity) !=
            XR_TYPED_FRAME_OK) {
        status = XR_TYPED_DISPATCH_DEBUG_IDENTITY_MISMATCH;
        goto cleanup;
    }
    if (xr_typed_frame_link_child(frame, child) != XR_TYPED_FRAME_OK)
        goto cleanup;
    linked = true;
    status = copy_call_arguments(frame, child,
                                 call->argument_count
                                     ? &arguments[call->argument_begin]
                                     : NULL,
                                 call->argument_count);
    if (status != XR_TYPED_DISPATCH_OK)
        goto cleanup;
    uint64_t child_result = 0;
    execution->call_depth++;
    status = execute_function(execution, child, call->callee_function, NULL,
                              call->argument_count, true, child_frame_id,
                              context->frame_id, &child_result);
    execution->call_depth--;
    if (status == XR_TYPED_DISPATCH_OK)
        status = store_i64_bits(frame, row->result_slot, child_result);

cleanup:
    if (linked && xr_typed_frame_unlink_child(frame, child) != XR_TYPED_FRAME_OK &&
        status == XR_TYPED_DISPATCH_OK)
        status = XR_TYPED_DISPATCH_FRAME_ERROR;
    xr_typed_frame_free(child);
    if (status != XR_TYPED_DISPATCH_TRACE_REJECTED) {
        XrVmTraceEvent call_return = make_trace_event(
            XR_VM_TRACE_CALL_RETURN, row->function, context->frame_id,
            XR_VM_TRACE_ID_NONE, execution->call_depth - 1u);
        call_return.instruction = row->id;
        call_return.opcode = row->opcode;
        call_return.call = call->id;
        call_return.related_function = call->callee_function;
        call_return.related_frame = child_frame_id;
        call_return.status = (uint32_t) status;
        XrTypedDispatchStatus trace_status =
            emit_trace_event(execution, &call_return);
        if (trace_status != XR_TYPED_DISPATCH_OK)
            status = trace_status;
    }
    return status;
}

/* The generated providers consume the same dense registry. The function table
 * deliberately uses only standard function pointers and sequential
 * initialization so it compiles as ordinary C11 under MSVC. */
static const XrTypedDispatchFunctionBinding generated_function_table[] = {
    {NULL, 0, 0},
#define XR_VM_OP(symbol, handler, kind, argument)                                      \
    {execute_##handler, XR_TARGET_INSTRUCTION_DISPATCH_##kind,                        \
     XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_##argument},
#include "xr_vm_ops.def"
#undef XR_VM_OP
};

_Static_assert(sizeof(generated_function_table) /
                       sizeof(generated_function_table[0]) ==
                   XR_TARGET_INSTRUCTION_COUNT,
               "generated function table must cover every typed opcode");

static bool optional_text_equal(const char *left, const char *right) {
    if (!left || !right)
        return left == right;
    return strcmp(left, right) == 0;
}

static bool instruction_contract_is_exact(
    uint16_t opcode, const XrTargetInstructionContract *contract) {
    const XrTargetInstructionContract *expected =
        xr_target_instruction_contract(opcode);
    return expected && contract &&
           optional_text_equal(contract->name, expected->name) &&
           optional_text_equal(contract->semantic_name,
                               expected->semantic_name) &&
           contract->arity == expected->arity &&
           contract->terminator == expected->terminator &&
           contract->result_rep == expected->result_rep &&
           contract->operand_rep[0] == expected->operand_rep[0] &&
           contract->operand_rep[1] == expected->operand_rep[1] &&
           contract->result_ownership == expected->result_ownership &&
           contract->operand_ownership == expected->operand_ownership &&
           contract->effects == expected->effects &&
           contract->error_kind == expected->error_kind &&
           contract->may_suspend == expected->may_suspend &&
           contract->immediate_kind == expected->immediate_kind &&
           contract->control_kind == expected->control_kind &&
           contract->dispatch_kind == expected->dispatch_kind &&
           contract->dispatch_argument == expected->dispatch_argument;
}

XR_FUNC bool xr_typed_dispatch_provider_contract_is_exact(
    XrTypedDispatchProvider provider, uint16_t opcode,
    const XrTargetInstructionContract *contract) {
    if (!instruction_contract_is_exact(opcode, contract))
        return false;
    if (provider == XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH) {
        switch ((XrTargetInstructionOpcode) opcode) {
#define XR_VM_OP(symbol, handler, kind, argument)                                  \
            case XR_TARGET_INSTRUCTION_##symbol:                                  \
                return contract->dispatch_kind ==                                 \
                           XR_TARGET_INSTRUCTION_DISPATCH_##kind &&                \
                       contract->dispatch_argument ==                             \
                           XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_##argument;
#include "xr_vm_ops.def"
#undef XR_VM_OP
            default: return false;
        }
    }
    if (provider == XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE &&
        opcode < sizeof(generated_function_table) /
                     sizeof(generated_function_table[0])) {
        const XrTypedDispatchFunctionBinding *binding =
            &generated_function_table[opcode];
        return binding->handler &&
               binding->dispatch_kind == contract->dispatch_kind &&
               binding->dispatch_argument == contract->dispatch_argument;
    }
    return false;
}

static XrTypedDispatchStatus execute_row_with_switch(
    XrTypedFrame *frame, const XrTargetInstructionRecord *row,
    const XrTargetInstructionContract *contract,
    XrTypedDispatchRowContext *context) {
    switch ((XrTargetInstructionOpcode) row->opcode) {
#define XR_VM_OP(symbol, handler, kind, argument)                                  \
        case XR_TARGET_INSTRUCTION_##symbol:                                      \
            return execute_##handler(frame, row, contract, context);
#include "xr_vm_ops.def"
#undef XR_VM_OP
        default: return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    }
}

static XrTypedDispatchStatus execute_row_with_function_table(
    XrTypedFrame *frame, const XrTargetInstructionRecord *row,
    const XrTargetInstructionContract *contract,
    XrTypedDispatchRowContext *context) {
    if (row->opcode >= sizeof(generated_function_table) /
                          sizeof(generated_function_table[0]))
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    const XrTypedDispatchFunctionBinding *binding =
        &generated_function_table[row->opcode];
    return binding->handler ? binding->handler(frame, row, contract, context)
                            : XR_TYPED_DISPATCH_PROGRAM_INVALID;
}

static XrTypedDispatchStatus execute_row(XrTypedFrame *frame,
                                         const XrTargetInstructionRecord *row,
                                         const XrVmDecodedInstruction *decoded,
                                         const int64_t *arguments,
                                         uint32_t argument_count,
                                         uint32_t row_count, uint32_t *next,
                                         bool *returned,
                                         uint64_t *return_bits,
                                         XrTypedDispatchExecution *execution,
                                         bool parameters_prebound,
                                         uint32_t frame_id) {
    const XrTargetInstructionContract *contract =
        decoded ? decoded->contract
                : xr_target_instruction_contract(row->opcode);
    if (!contract)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    XrTypedDispatchRowContext context = {
        arguments, argument_count, row_count, next, returned, return_bits,
        execution, decoded, parameters_prebound, frame_id,
    };
    if (!xr_typed_dispatch_provider_contract_is_exact(
            execution->provider, row->opcode, contract))
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    if (execution->provider ==
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH)
        return execute_row_with_switch(frame, row, contract, &context);
    if (execution->provider ==
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE)
        return execute_row_with_function_table(frame, row, contract, &context);
    return XR_TYPED_DISPATCH_PROGRAM_INVALID;
}

static XrTypedDispatchStatus execute_function(
    XrTypedDispatchExecution *execution, XrTypedFrame *frame,
    uint32_t function, const int64_t *arguments, uint32_t argument_count,
    bool parameters_prebound, uint32_t frame_id, uint32_t parent_frame_id,
    uint64_t *return_bits) {
    uint32_t frame_depth = execution->call_depth - 1u;
    XrVmTraceEvent frame_enter = make_trace_event(
        XR_VM_TRACE_FRAME_ENTER, function, frame_id, parent_frame_id,
        frame_depth);
    XrTypedDispatchStatus status =
        emit_trace_event(execution, &frame_enter);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;

    uint32_t current = 0;
    uint32_t last_block = XR_VM_TRACE_ID_NONE;
    uint32_t current_instruction = XR_VM_TRACE_ID_NONE;
    uint16_t current_opcode = XR_TARGET_INSTRUCTION_INVALID;
    bool returned = false;
    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *instructions = NULL;
    XrVmDecodedFunctionView decoded_function = {0};
    if (execution->decoded_cache) {
        if (!xr_typed_decoded_cache_function(execution->decoded_cache, function,
                                             &decoded_function)) {
            status = XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE;
            goto finish;
        }
        instruction_count = decoded_function.instruction_count;
    } else {
        instructions = xr_target_plan_function_instructions(
            execution->plan, function, &instruction_count);
    }
    if ((!instructions && !decoded_function.instructions) ||
        !instruction_count) {
        status = XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE;
        goto finish;
    }
    uint32_t declared_parameters = decoded_function.instructions
                                       ? decoded_function.parameter_count
                                       : 0;
    if (!decoded_function.instructions)
        for (uint32_t i = 0; i < instruction_count; i++)
            declared_parameters +=
                instructions[i].opcode == XR_TARGET_INSTRUCTION_PARAM_I64;
    if (declared_parameters != argument_count) {
        status = XR_TYPED_DISPATCH_ARGUMENT_MISMATCH;
        goto finish;
    }

    while (!returned) {
        if (current >= instruction_count) {
            status = XR_TYPED_DISPATCH_PROGRAM_INVALID;
            goto finish_with_instruction;
        }
        if (execution->remaining_steps == 0) {
            status = XR_TYPED_DISPATCH_STEP_LIMIT_EXCEEDED;
            goto finish_with_instruction;
        }
        execution->remaining_steps--;
        const XrVmDecodedInstruction *decoded =
            decoded_function.instructions
                ? &decoded_function.instructions[current]
                : NULL;
        const XrTargetInstructionRecord *row =
            decoded ? &decoded->row : &instructions[current];
        current_instruction = row->id;
        current_opcode = row->opcode;
        uint32_t block_entry_instruction = XR_VM_TRACE_ID_NONE;
        if (decoded) {
            if (decoded->block >= decoded_function.block_count) {
                status = XR_TYPED_DISPATCH_PROGRAM_INVALID;
                goto finish_with_instruction;
            }
            uint32_t block_first =
                decoded_function.blocks[decoded->block].first_row;
            if (block_first >= instruction_count) {
                status = XR_TYPED_DISPATCH_PROGRAM_INVALID;
                goto finish_with_instruction;
            }
            block_entry_instruction =
                decoded_function.instructions[block_first].row.id;
            if (xr_typed_frame_enter_decoded_instruction(
                    frame, current_instruction,
                    block_entry_instruction) != XR_TYPED_FRAME_OK) {
                status = XR_TYPED_DISPATCH_FRAME_ERROR;
                goto finish_with_instruction;
            }
        } else {
            if (xr_typed_frame_enter_instruction(frame,
                                                  current_instruction) !=
                XR_TYPED_FRAME_OK) {
                status = XR_TYPED_DISPATCH_FRAME_ERROR;
                goto finish_with_instruction;
            }
            XrTypedFrameContext frame_context;
            if (xr_typed_frame_context(frame, &frame_context) !=
                XR_TYPED_FRAME_OK) {
                status = XR_TYPED_DISPATCH_FRAME_ERROR;
                goto finish_with_instruction;
            }
            block_entry_instruction =
                frame_context.block_entry_instruction;
        }
        if (block_entry_instruction != last_block) {
            XrVmTraceEvent block_enter = make_trace_event(
                XR_VM_TRACE_BLOCK_ENTER, function, frame_id,
                parent_frame_id, frame_depth);
            block_enter.instruction = current_instruction;
            block_enter.block = block_entry_instruction;
            block_enter.opcode = current_opcode;
            status = emit_trace_event(execution, &block_enter);
            if (status != XR_TYPED_DISPATCH_OK)
                return status;
            last_block = block_entry_instruction;
        }
        XrVmTraceEvent instruction = make_trace_event(
            XR_VM_TRACE_INSTRUCTION, function, frame_id, parent_frame_id,
            frame_depth);
        instruction.instruction = current_instruction;
        instruction.block = block_entry_instruction;
        instruction.opcode = current_opcode;
        status = emit_trace_event(execution, &instruction);
        if (status != XR_TYPED_DISPATCH_OK)
            return status;
        uint32_t next = current + 1u;
        status = execute_row(
            frame, row, decoded, arguments, argument_count,
            instruction_count, &next, &returned, return_bits, execution,
            parameters_prebound, frame_id);
        if (status != XR_TYPED_DISPATCH_OK)
            goto finish_with_instruction;
        current = next;
    }
    status = XR_TYPED_DISPATCH_OK;
    goto finish;

finish_with_instruction:
    if (status != XR_TYPED_DISPATCH_TRACE_REJECTED) {
        XrVmTraceEvent error = make_trace_event(
            XR_VM_TRACE_ERROR, function, frame_id, parent_frame_id,
            frame_depth);
        error.instruction = current_instruction;
        error.opcode = current_opcode;
        error.block = last_block;
        error.status = (uint32_t) status;
        XrTypedDispatchStatus trace_status =
            emit_trace_event(execution, &error);
        if (trace_status != XR_TYPED_DISPATCH_OK)
            return trace_status;
    }

finish:
    if (status != XR_TYPED_DISPATCH_OK &&
        status != XR_TYPED_DISPATCH_TRACE_REJECTED &&
        current_instruction == XR_VM_TRACE_ID_NONE) {
        XrVmTraceEvent error = make_trace_event(
            XR_VM_TRACE_ERROR, function, frame_id, parent_frame_id,
            frame_depth);
        error.status = (uint32_t) status;
        XrTypedDispatchStatus trace_status =
            emit_trace_event(execution, &error);
        if (trace_status != XR_TYPED_DISPATCH_OK)
            return trace_status;
    }
    if (status == XR_TYPED_DISPATCH_TRACE_REJECTED)
        return status;
    XrVmTraceEvent frame_exit = make_trace_event(
        XR_VM_TRACE_FRAME_EXIT, function, frame_id, parent_frame_id,
        frame_depth);
    frame_exit.status = (uint32_t) status;
    XrTypedDispatchStatus trace_status =
        emit_trace_event(execution, &frame_exit);
    return trace_status == XR_TYPED_DISPATCH_OK ? status : trace_status;
}

XrTypedDispatchStatus xr_typed_dispatch_execute_i64(
    const XrTypedDispatchI64Request *request) {
    if (request && request->result)
        *request->result = 0;
    if (!request || !request->verified_plan ||
        !request->required_plan_fingerprint || !request->result ||
        (!request->arguments && request->argument_count) ||
        (request->provider !=
             XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH &&
         request->provider !=
             XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE))
        return XR_TYPED_DISPATCH_INVALID_ARGUMENT;
    const XrTargetPlan *verified_plan = request->verified_plan;
    const XrFingerprint *required_plan_fingerprint =
        request->required_plan_fingerprint;
    if (!xr_target_plan_is_verified(verified_plan))
        return XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED;
    if (!xr_fingerprint_equal(xr_target_plan_fingerprint(verified_plan),
                              *required_plan_fingerprint))
        return XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH;
    if (request->decoded_cache) {
        XrVmDecodedCacheStatus cache_status =
            xr_typed_decoded_cache_require_exact(
                request->decoded_cache, verified_plan,
                required_plan_fingerprint);
        if (cache_status == XR_VM_DECODED_CACHE_PLAN_IDENTITY_MISMATCH)
            return XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH;
        if (cache_status != XR_VM_DECODED_CACHE_OK)
            return cache_status == XR_VM_DECODED_CACHE_PLAN_NOT_VERIFIED
                       ? XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED
                       : XR_TYPED_DISPATCH_PROGRAM_INVALID;
    } else {
        char error[512] = {0};
        if (!xr_target_plan_fingerprint_is_intact(verified_plan) ||
            !xr_target_instruction_program_verify(verified_plan, error,
                                                   sizeof(error)))
            return XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED;
    }
    if (request->debug_session &&
        !xr_typed_debug_session_matches_plan(
            request->debug_session,
            xr_target_plan_fingerprint(verified_plan)))
        return XR_TYPED_DISPATCH_DEBUG_IDENTITY_MISMATCH;
    if (xr_target_plan_function_execution_family_mask(
            verified_plan, request->function) !=
        XR_TARGET_EXECUTION_SCALAR_I64_CLOSED)
        return XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE;

    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    XrTypedFrame *frame = NULL;
    if (xr_typed_frame_create(verified_plan, required_plan_fingerprint,
                              request->function, &limits, &frame) !=
        XR_TYPED_FRAME_OK)
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    if (request->debug_session &&
        request->debug_session->generation_identity_present &&
        xr_typed_frame_bind_generation_identity(
            frame, &request->debug_session->generation_identity) !=
            XR_TYPED_FRAME_OK) {
        xr_typed_frame_free(frame);
        return XR_TYPED_DISPATCH_DEBUG_IDENTITY_MISMATCH;
    }

    uint64_t return_bits = 0;
    XrTypedDispatchExecution execution = {
        .plan = verified_plan,
        .fingerprint = required_plan_fingerprint,
        .limits = limits,
        .remaining_steps = XR_TYPED_DISPATCH_MAX_STEPS,
        .call_depth = 1,
        .next_frame_id = 1,
        .debug_session = request->debug_session,
        .decoded_cache = request->decoded_cache,
        .provider = request->provider,
    };
    XrTypedDispatchStatus status = execute_function(
        &execution, frame, request->function, request->arguments,
        request->argument_count, false, 0, XR_VM_TRACE_ID_NONE,
        &return_bits);
    xr_typed_frame_free(frame);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    memcpy(request->result, &return_bits, sizeof(*request->result));
    return XR_TYPED_DISPATCH_OK;
}
