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
#include "../plan/target/xr_target_profile.h"
#include "../shared/xr_bits_core.h"
#include "../shared/xr_compare_core.h"
#include "../shared/xr_int_arith_core.h"
#include "../shared/xr_semantic_owner_ids_gen.h"
#include <string.h>

/* Scalar dispatch owns no lifecycle executor.  Prove that boundary from the
 * verified plan before a frame exists, then repeat it from the allocated
 * frame's exact footprint before destruction. */
static bool function_has_zero_lifecycle(const XrTargetPlan *plan,
                                        uint32_t function) {
    uint32_t function_count = 0;
    uint32_t root_count = 0;
    uint32_t cleanup_count = 0;
    uint32_t coroutine_count = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(plan, &function_count);
    (void) xr_target_plan_root_maps(plan, &root_count);
    (void) xr_target_plan_cleanups(plan, &cleanup_count);
    (void) xr_target_plan_coroutines(plan, &coroutine_count);
    const XrTargetFunctionRecord *record =
        functions && function < function_count ? &functions[function] : NULL;
    return record && record->id == function &&
           record->root_begin <= root_count &&
           record->root_count <= root_count - record->root_begin &&
           record->cleanup_begin <= cleanup_count &&
           record->cleanup_count <= cleanup_count - record->cleanup_begin &&
           record->coroutine_begin <= coroutine_count &&
           record->coroutine_count <=
               coroutine_count - record->coroutine_begin &&
           record->root_count == 0 && record->cleanup_count == 0 &&
           record->coroutine_count == 0;
}

static XrTypedDispatchStatus free_scalar_frame(XrTypedFrame **frame) {
    if (!frame)
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    if (!*frame)
        return XR_TYPED_DISPATCH_OK;
    XrTypedFrameMemoryFootprint footprint = {0};
    if (xr_typed_frame_memory_footprint(*frame, &footprint) !=
            XR_TYPED_FRAME_OK ||
        footprint.lifecycle_state_metadata_bytes != 0)
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    return xr_typed_frame_free(frame) == XR_TYPED_FRAME_OK
               ? XR_TYPED_DISPATCH_OK
               : XR_TYPED_DISPATCH_FRAME_ERROR;
}

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
    const XrVmDynamicEntryContext *dynamic_entries;
    XrModuleGenerationIdentity generation_identity;
    bool generation_identity_present;
    bool use_dynamic_entry_cache;
    XrTypedDispatchProvider provider;
} XrTypedDispatchExecution;

static bool bytes_are_zero(const uint8_t *bytes, size_t size) {
    uint8_t combined = 0;
    for (size_t i = 0; i < size; i++)
        combined |= bytes[i];
    return combined == 0;
}

static bool generation_identity_equal(
    const XrModuleGenerationIdentity *left,
    const XrModuleGenerationIdentity *right) {
    return left && right && left->schema_version == right->schema_version &&
           left->target_plan_schema_version ==
               right->target_plan_schema_version &&
           left->generation_number == right->generation_number &&
           left->completed_family_mask == right->completed_family_mask &&
           left->required_capability_mask == right->required_capability_mask &&
           memcmp(left->semantic_fingerprint, right->semantic_fingerprint,
                  sizeof(left->semantic_fingerprint)) == 0 &&
           memcmp(left->target_profile_fingerprint,
                  right->target_profile_fingerprint,
                  sizeof(left->target_profile_fingerprint)) == 0 &&
           memcmp(left->target_plan_fingerprint,
                  right->target_plan_fingerprint,
                  sizeof(left->target_plan_fingerprint)) == 0 &&
           memcmp(left->runtime_abi_fingerprint,
                  right->runtime_abi_fingerprint,
                  sizeof(left->runtime_abi_fingerprint)) == 0 &&
           memcmp(left->provider_set_fingerprint,
                  right->provider_set_fingerprint,
                  sizeof(left->provider_set_fingerprint)) == 0 &&
           memcmp(left->object_header_fingerprint,
                  right->object_header_fingerprint,
                  sizeof(left->object_header_fingerprint)) == 0 &&
           memcmp(left->generation_fingerprint,
                  right->generation_fingerprint,
                  sizeof(left->generation_fingerprint)) == 0;
}

static bool generation_identity_matches_plan(
    const XrModuleGenerationIdentity *identity, const XrTargetPlan *plan,
    XrFingerprint plan_fingerprint) {
    const XrTargetProfile *profile = xr_target_plan_profile(plan);
    XrFingerprint semantic = xr_target_plan_semantic_fingerprint(plan);
    XrFingerprint profile_fingerprint =
        profile ? xr_target_profile_fingerprint(profile)
                : (XrFingerprint) {{0}};
    return identity && profile &&
           identity->schema_version == XR_RUNTIME_GENERATION_SCHEMA_VERSION &&
           identity->target_plan_schema_version ==
               xr_target_plan_schema_version(plan) &&
           identity->completed_family_mask ==
               xr_target_plan_completed_family_mask(plan) &&
           memcmp(identity->semantic_fingerprint, semantic.bytes,
                  sizeof(semantic.bytes)) == 0 &&
           memcmp(identity->target_profile_fingerprint,
                  profile_fingerprint.bytes,
                  sizeof(profile_fingerprint.bytes)) == 0 &&
           memcmp(identity->target_plan_fingerprint, plan_fingerprint.bytes,
                  sizeof(plan_fingerprint.bytes)) == 0 &&
           !bytes_are_zero(identity->generation_fingerprint,
                           sizeof(identity->generation_fingerprint));
}

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
    if (!xr_typed_debug_emit(
            execution->debug_session, execution->fingerprint,
            execution->generation_identity_present
                ? &execution->generation_identity
                : NULL,
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
    status = XR_TYPED_DISPATCH_FRAME_ERROR;
    if (!function_has_zero_lifecycle(execution->plan,
                                     call->callee_function))
        goto cleanup;
    if (xr_typed_frame_create(execution->plan, execution->fingerprint,
                              call->callee_function, &execution->limits,
                              &child) != XR_TYPED_FRAME_OK)
        goto cleanup;
    if (execution->generation_identity_present &&
        xr_typed_frame_bind_generation_identity(
            child, &execution->generation_identity) !=
            XR_TYPED_FRAME_OK) {
        status = XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH;
        goto cleanup;
    }
    if (xr_typed_frame_link_child(frame, child) != XR_TYPED_FRAME_OK)
        goto cleanup;
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
    /* Successful frame cleanup severs the parent link.  A lifecycle refusal
     * leaves the child linked and therefore reachable from its owner. */
    if (free_scalar_frame(&child) != XR_TYPED_DISPATCH_OK)
        status = XR_TYPED_DISPATCH_FRAME_ERROR;
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

static XrTypedDispatchStatus execute_entry_call(
    XrTypedFrame *frame, const XrTargetInstructionRecord *row,
    const XrTargetInstructionContract *contract,
    XrTypedDispatchRowContext *context) {
    (void) contract;
    XrTypedDispatchExecution *execution = context->execution;
    if (!execution || execution->call_depth >= XR_TYPED_DISPATCH_MAX_CALL_DEPTH)
        return XR_TYPED_DISPATCH_CALL_DEPTH_EXCEEDED;
    if (!execution->dynamic_entries ||
        execution->dynamic_entries->schema_version !=
            XR_VM_DYNAMIC_ENTRY_CONTEXT_SCHEMA_VERSION ||
        execution->dynamic_entries->reserved != 0 ||
        !execution->dynamic_entries->validate ||
        !execution->dynamic_entries->acquire ||
        !execution->dynamic_entries->release)
        return XR_TYPED_DISPATCH_ENTRY_UNAVAILABLE;

    uint32_t expectation_count = 0;
    uint32_t call_count = 0;
    uint32_t argument_count = 0;
    const XrTargetEntryExpectationRecord *expectations =
        xr_target_plan_entry_expectations(execution->plan,
                                          &expectation_count);
    const XrTargetCallRecord *calls =
        xr_target_plan_calls(execution->plan, &call_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(execution->plan, &argument_count);
    uint32_t expectation_index = (uint32_t) row->immediate_bits;
    const XrTargetEntryExpectationRecord *expectation =
        expectations && row->immediate_bits <= UINT32_MAX &&
                expectation_index < expectation_count
            ? &expectations[expectation_index]
            : NULL;
    const XrTargetCallRecord *call =
        expectation && expectation->call < call_count
            ? &calls[expectation->call]
            : NULL;
    if (!expectation || !call || call->argument_count >
                                     XR_TARGET_INSTRUCTION_MAX_PARAMETERS ||
        call->argument_begin > argument_count ||
        call->argument_count > argument_count - call->argument_begin)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;

    int64_t child_arguments[XR_TARGET_INSTRUCTION_MAX_PARAMETERS];
    for (uint16_t ordinal = 0; ordinal < call->argument_count; ordinal++) {
        uint64_t bits = 0;
        XrTypedDispatchStatus load = load_i64_bits(
            frame, arguments[call->argument_begin + ordinal].caller_slot,
            &bits);
        if (load != XR_TYPED_DISPATCH_OK)
            return load;
        memcpy(&child_arguments[ordinal], &bits,
               sizeof(child_arguments[ordinal]));
    }

    XrVmDynamicEntryResolution resolution;
    memset(&resolution, 0, sizeof(resolution));
    XrVmDynamicEntryStatus acquire = execution->dynamic_entries->acquire(
        execution->dynamic_entries, execution->plan, execution->fingerprint,
        expectation, execution->use_dynamic_entry_cache, &resolution);
    if (acquire != XR_VM_DYNAMIC_ENTRY_OK) {
        if (acquire == XR_VM_DYNAMIC_ENTRY_BUDGET_EXCEEDED)
            return XR_TYPED_DISPATCH_ENTRY_BUDGET_EXCEEDED;
        if (acquire == XR_VM_DYNAMIC_ENTRY_AUTHORITY_MISMATCH)
            return XR_TYPED_DISPATCH_ENTRY_AUTHORITY_MISMATCH;
        return XR_TYPED_DISPATCH_ENTRY_UNAVAILABLE;
    }
    bool acquired = true;
    XrTypedDispatchStatus status = XR_TYPED_DISPATCH_PROGRAM_INVALID;
    XrTypedFrame *child = NULL;
    uint32_t child_frame_id = execution->next_frame_id++;
    uint32_t child_function = resolution.function;
    const XrVmDynamicEntryContext *release_context =
        execution->dynamic_entries;
    uint64_t family = xr_target_plan_function_execution_family_mask(
        resolution.plan, resolution.function);
    if (!resolution.plan || !resolution.lease ||
        !xr_target_plan_is_verified(resolution.plan) ||
        !xr_fingerprint_equal(xr_target_plan_fingerprint(resolution.plan),
                              resolution.plan_fingerprint) ||
        !xr_target_plan_fingerprint_is_intact(resolution.plan) ||
        (family != XR_TARGET_EXECUTION_SCALAR_I64_CLOSED &&
         family != XR_TARGET_EXECUTION_SCALAR_I64_DYNAMIC) ||
        !function_has_zero_lifecycle(resolution.plan,
                                     resolution.function) ||
        !resolution.dynamic_entries ||
        resolution.dynamic_entries->schema_version !=
            XR_VM_DYNAMIC_ENTRY_CONTEXT_SCHEMA_VERSION ||
        !resolution.dynamic_entries->validate ||
        resolution.dynamic_entries->validate(
            resolution.dynamic_entries, resolution.plan,
            &resolution.plan_fingerprint,
            &resolution.generation_identity) != XR_VM_DYNAMIC_ENTRY_OK)
        goto cleanup;

    XrVmTraceEvent call_enter = make_trace_event(
        XR_VM_TRACE_CALL_ENTER, row->function, context->frame_id,
        XR_VM_TRACE_ID_NONE, execution->call_depth - 1u);
    call_enter.instruction = row->id;
    call_enter.opcode = row->opcode;
    call_enter.call = call->id;
    call_enter.related_function = resolution.function;
    call_enter.related_frame = child_frame_id;
    status = emit_trace_event(execution, &call_enter);
    if (status != XR_TYPED_DISPATCH_OK)
        goto cleanup;
    if (xr_typed_frame_create(resolution.plan, &resolution.plan_fingerprint,
                              resolution.function, &execution->limits,
                              &child) != XR_TYPED_FRAME_OK) {
        status = XR_TYPED_DISPATCH_FRAME_ERROR;
        goto cleanup;
    }
    if (xr_typed_frame_bind_generation_identity(
            child, &resolution.generation_identity) != XR_TYPED_FRAME_OK) {
        status = XR_TYPED_DISPATCH_ENTRY_AUTHORITY_MISMATCH;
        goto cleanup;
    }

    const XrTargetPlan *saved_plan = execution->plan;
    const XrFingerprint *saved_fingerprint = execution->fingerprint;
    const XrVmDecodedCache *saved_decoded_cache = execution->decoded_cache;
    const XrVmDynamicEntryContext *saved_dynamic_entries =
        execution->dynamic_entries;
    XrModuleGenerationIdentity saved_generation =
        execution->generation_identity;
    bool saved_generation_present = execution->generation_identity_present;
    execution->plan = resolution.plan;
    execution->fingerprint = &resolution.plan_fingerprint;
    execution->decoded_cache = resolution.decoded_cache;
    execution->dynamic_entries = resolution.dynamic_entries;
    execution->generation_identity = resolution.generation_identity;
    execution->generation_identity_present = true;
    uint64_t child_result = 0;
    execution->call_depth++;
    status = execute_function(
        execution, child, resolution.function,
        call->argument_count ? child_arguments : NULL, call->argument_count,
        false, child_frame_id, context->frame_id, &child_result);
    execution->call_depth--;
    execution->plan = saved_plan;
    execution->fingerprint = saved_fingerprint;
    execution->decoded_cache = saved_decoded_cache;
    execution->dynamic_entries = saved_dynamic_entries;
    execution->generation_identity = saved_generation;
    execution->generation_identity_present = saved_generation_present;
    if (status == XR_TYPED_DISPATCH_OK)
        status = store_i64_bits(frame, row->result_slot, child_result);

cleanup:;
    bool child_released =
        free_scalar_frame(&child) == XR_TYPED_DISPATCH_OK;
    if (!child_released)
        status = XR_TYPED_DISPATCH_FRAME_ERROR;
    if (acquired && child_released) {
        XrVmDynamicEntryStatus released =
            release_context->release(release_context, &resolution);
        if (released != XR_VM_DYNAMIC_ENTRY_OK &&
            status != XR_TYPED_DISPATCH_TRACE_REJECTED)
            status = XR_TYPED_DISPATCH_ENTRY_RELEASE_FAILED;
    }
    if (status != XR_TYPED_DISPATCH_TRACE_REJECTED) {
        XrVmTraceEvent call_return = make_trace_event(
            XR_VM_TRACE_CALL_RETURN, row->function, context->frame_id,
            XR_VM_TRACE_ID_NONE, execution->call_depth - 1u);
        call_return.instruction = row->id;
        call_return.opcode = row->opcode;
        call_return.call = call->id;
        call_return.related_function = child_function;
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
        (request->use_dynamic_entry_cache && !request->dynamic_entries) ||
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
    uint64_t execution_family =
        xr_target_plan_function_execution_family_mask(
            verified_plan, request->function);
    if ((execution_family != XR_TARGET_EXECUTION_SCALAR_I64_CLOSED &&
         execution_family != XR_TARGET_EXECUTION_SCALAR_I64_DYNAMIC) ||
        !function_has_zero_lifecycle(verified_plan, request->function) ||
        (execution_family == XR_TARGET_EXECUTION_SCALAR_I64_DYNAMIC &&
         (!request->generation_identity ||
          !request->dynamic_entries ||
          request->dynamic_entries->schema_version !=
              XR_VM_DYNAMIC_ENTRY_CONTEXT_SCHEMA_VERSION ||
           request->dynamic_entries->reserved != 0 ||
           !request->dynamic_entries->validate ||
           !request->dynamic_entries->acquire ||
          !request->dynamic_entries->release)))
        return XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE;
    if (request->generation_identity &&
        !generation_identity_matches_plan(
            request->generation_identity, verified_plan,
            *required_plan_fingerprint))
        return XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH;
    if (execution_family == XR_TARGET_EXECUTION_SCALAR_I64_DYNAMIC &&
        request->dynamic_entries->validate(
            request->dynamic_entries, verified_plan,
            required_plan_fingerprint, request->generation_identity) !=
            XR_VM_DYNAMIC_ENTRY_OK)
        return XR_TYPED_DISPATCH_ENTRY_AUTHORITY_MISMATCH;
    if (request->debug_session &&
        request->debug_session->generation_identity_present &&
        (!request->generation_identity ||
         !generation_identity_equal(
             &request->debug_session->generation_identity,
             request->generation_identity)))
        return XR_TYPED_DISPATCH_DEBUG_IDENTITY_MISMATCH;

    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    XrTypedFrame *frame = NULL;
    if (xr_typed_frame_create(verified_plan, required_plan_fingerprint,
                              request->function, &limits, &frame) !=
        XR_TYPED_FRAME_OK)
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    if (request->generation_identity &&
        xr_typed_frame_bind_generation_identity(
            frame, request->generation_identity) !=
            XR_TYPED_FRAME_OK) {
        return free_scalar_frame(&frame) == XR_TYPED_DISPATCH_OK
                   ? XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH
                   : XR_TYPED_DISPATCH_FRAME_ERROR;
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
        .dynamic_entries = request->dynamic_entries,
        .use_dynamic_entry_cache = request->use_dynamic_entry_cache,
        .provider = request->provider,
    };
    if (request->generation_identity) {
        execution.generation_identity = *request->generation_identity;
        execution.generation_identity_present = true;
    }
    XrTypedDispatchStatus status = execute_function(
        &execution, frame, request->function, request->arguments,
        request->argument_count, false, 0, XR_VM_TRACE_ID_NONE,
        &return_bits);
    if (free_scalar_frame(&frame) != XR_TYPED_DISPATCH_OK)
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    memcpy(request->result, &return_bits, sizeof(*request->result));
    return XR_TYPED_DISPATCH_OK;
}
