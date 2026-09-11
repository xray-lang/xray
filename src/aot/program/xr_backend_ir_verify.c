/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_backend_ir_verify.c - Independent AOT invariant and lowering checks
 */

#include "xr_backend_ir_internal.h"

#include "../../base/xmalloc.h"
#include "../../core/xr_core_spec_gen.h"
#include "../../shared/xr_assertion_plan.h"
#include "../../shared/xr_target_query_registry_gen.h"

#include <string.h>

static bool array_u32_equal(const uint32_t *left, const uint32_t *right, uint32_t count) {
    return count == 0u ||
           (left && right && memcmp(left, right, (size_t) count * sizeof(*left)) == 0);
}

static bool array_u16_equal(const uint16_t *left, const uint16_t *right, uint32_t count) {
    return count == 0u ||
           (left && right && memcmp(left, right, (size_t) count * sizeof(*left)) == 0);
}

static bool array_mode_equal(const XrParamMode *left, const XrParamMode *right, uint32_t count) {
    return count == 0u ||
           (left && right && memcmp(left, right, (size_t) count * sizeof(*left)) == 0);
}

static bool array_category_equal(const XrCoreIrValueCategory *left,
                                 const XrCoreIrValueCategory *right, uint32_t count) {
    return count == 0u ||
           (left && right && memcmp(left, right, (size_t) count * sizeof(*left)) == 0);
}

static bool array_ownership_equal(const XrCoreIrOwnershipDisposition *left,
                                  const XrCoreIrOwnershipDisposition *right, uint32_t count) {
    return count == 0u ||
           (left && right && memcmp(left, right, (size_t) count * sizeof(*left)) == 0);
}

static bool fingerprint_is_zero(XrFingerprint fingerprint) {
    uint8_t combined = 0u;
    for (size_t index = 0; index < sizeof(fingerprint.bytes); ++index)
        combined |= fingerprint.bytes[index];
    return combined == 0u;
}

static const XrValidatedSignature *
witness_invoke_signature(const XrBackendIR *ir, const XrBackendFunction *function,
                         const XrBackendInstruction *instruction) {
    if (!ir || !function || !instruction || instruction->operand_count == 0u)
        return NULL;
    uint32_t receiver_value = instruction->operands[0];
    if (receiver_value >= function->value_count)
        return NULL;
    const XrValidatedType *receiver =
        xr_validated_program_type(ir->program, function->value_types[receiver_value]);
    if (!receiver || receiver->kind != XR_CORE_IR_TYPE_EXISTENTIAL ||
        receiver->interface_id >= ir->program->interface_count)
        return NULL;
    const XrValidatedInterface *interface_row = &ir->program->interfaces[receiver->interface_id];
    if (instruction->immediate.u32 >= interface_row->slot_count ||
        interface_row->slot_signature_ids[instruction->immediate.u32] >=
            ir->program->signature_count)
        return NULL;
    return &ir->program->signatures[interface_row->slot_signature_ids[instruction->immediate.u32]];
}

static const XrValidatedSignature *
callable_invoke_signature(const XrBackendIR *ir, const XrBackendFunction *function,
                          const XrBackendInstruction *instruction) {
    if (!ir || !function || !instruction || instruction->operand_count == 0u)
        return NULL;
    uint32_t callable_value = instruction->operands[0];
    if (callable_value >= function->value_count)
        return NULL;
    const XrValidatedType *callable =
        xr_validated_program_type(ir->program, function->value_types[callable_value]);
    if (!callable || callable->kind != XR_CORE_IR_TYPE_CALLABLE ||
        callable->signature_id >= ir->program->signature_count)
        return NULL;
    const XrValidatedSignature *signature = &ir->program->signatures[callable->signature_id];
    return signature->has_receiver ? NULL : signature;
}

static uint32_t invoke_typed_successor_count(const XrBackendIR *ir,
                                             const XrBackendFunction *function,
                                             const XrBackendInstruction *instruction) {
    if (!ir || !instruction)
        return 0u;
    if (instruction->operation_id == XR_CORE_OP_CORE_CALL_SEALED_INVOKE) {
        if (instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_FUNCTION ||
            instruction->immediate.function_id >= ir->function_count)
            return 0u;
        const XrBackendFunction *callee = &ir->functions[instruction->immediate.function_id];
        return 1u + (callee->error_type_id != XR_CORE_TYPE_VOID ? 1u : 0u) +
               (callee->panic_type_id != XR_CORE_TYPE_VOID ? 1u : 0u);
    }
    if (instruction->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_INVOKE) {
        const XrValidatedSignature *signature = witness_invoke_signature(ir, function, instruction);
        return signature ? 1u + (signature->error_type_id != XR_CORE_TYPE_VOID ? 1u : 0u) +
                               (signature->panic_type_id != XR_CORE_TYPE_VOID ? 1u : 0u)
                         : 0u;
    }
    if (instruction->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE) {
        const XrValidatedSignature *signature =
            callable_invoke_signature(ir, function, instruction);
        return signature ? 1u + (signature->error_type_id != XR_CORE_TYPE_VOID ? 1u : 0u) +
                               (signature->panic_type_id != XR_CORE_TYPE_VOID ? 1u : 0u)
                         : 0u;
    }
    return 0u;
}

static uint32_t instruction_trap_successor(const XrBackendIR *ir, const XrBackendFunction *function,
                                           const XrBackendInstruction *instruction) {
    switch (instruction->operation_id) {
        case XR_CORE_OP_CORE_COROUTINE_CALL_SEALED:
        case XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT:
            return instruction->successor_count == 3u ? 2u : XR_PROGRAM_LOCATION_NONE;
        case XR_CORE_OP_CORE_PROVIDER_CALL:
        case XR_CORE_OP_CORE_CALL_SEALED_DIRECT:
        case XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT:
        case XR_CORE_OP_CORE_CALL_WITNESS_DIRECT:
            return instruction->successor_count == 1u ? 0u : XR_PROGRAM_LOCATION_NONE;
        case XR_CORE_OP_CORE_CALL_SEALED_INVOKE:
        case XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE:
        case XR_CORE_OP_CORE_CALL_WITNESS_INVOKE: {
            uint32_t typed = invoke_typed_successor_count(ir, function, instruction);
            return typed > 1u && instruction->successor_count == typed + 1u
                       ? typed
                       : XR_PROGRAM_LOCATION_NONE;
        }
        default:
            return XR_PROGRAM_LOCATION_NONE;
    }
}

static bool backend_instruction_is_terminal(uint16_t operation_id) {
    switch (operation_id) {
        case XR_CORE_OP_CORE_BRANCH:
        case XR_CORE_OP_CORE_CONDITIONAL_BRANCH:
        case XR_CORE_OP_CORE_CALL_SEALED_INVOKE:
        case XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE:
        case XR_CORE_OP_CORE_CALL_WITNESS_INVOKE:
        case XR_CORE_OP_CORE_COROUTINE_YIELD:
        case XR_CORE_OP_CORE_COROUTINE_SUSPEND:
        case XR_CORE_OP_CORE_COROUTINE_CALL_SEALED:
        case XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT:
        case XR_CORE_OP_CORE_RETURN:
        case XR_CORE_OP_CORE_TRAP:
        case XR_CORE_OP_CORE_CANCEL_PUBLISH:
        case XR_CORE_OP_CORE_ERROR_PUBLISH:
        case XR_CORE_OP_CORE_PANIC_PUBLISH:
            return true;
        default:
            return false;
    }
}

/* Validate storage before frame checks can follow a value into another block. */
static bool backend_storage_valid(const XrBackendIR *ir, XrBackendDiagnostic *diagnostic) {
    uint64_t blocks = 0u, instructions = 0u, values = 0u;
    if (ir->function_count > ir->options.max_functions) {
        xr_backend_set_diagnostic(diagnostic, XR_BACKEND_INVARIANT_REJECTED, 0u, 0u, 0u, 0u);
        return false;
    }
    for (uint32_t f = 0u; f < ir->function_count; ++f) {
        const XrBackendFunction *function = &ir->functions[f];
        blocks += function->block_count;
        values += function->value_count;
        if (blocks > ir->options.max_blocks || values > ir->options.max_values ||
            !function->blocks || function->block_count == 0u ||
            function->entry_block >= function->block_count ||
            (function->parameter_count &&
             (!function->parameter_types || !function->parameter_modes)) ||
            (function->value_count &&
             (!function->value_types || !function->value_categories ||
              !function->value_ownerships || !function->value_representations))) {
            xr_backend_set_diagnostic(diagnostic, XR_BACKEND_INVARIANT_REJECTED, 0u, f, 0u, 0u);
            return false;
        }
        for (uint32_t b = 0u; b < function->block_count; ++b) {
            const XrBackendBlock *block = &function->blocks[b];
            instructions += block->instruction_count;
            if (instructions > ir->options.max_instructions || !block->instructions ||
                block->instruction_count == 0u || block->argument_count > function->value_count ||
                (block->argument_count &&
                 (!block->argument_ids || !block->argument_types || !block->argument_categories ||
                  !block->argument_ownerships))) {
                xr_backend_set_diagnostic(diagnostic, XR_BACKEND_INVARIANT_REJECTED, 0u, f, b, 0u);
                return false;
            }
            for (uint32_t i = 0u; i < block->instruction_count; ++i) {
                const XrBackendInstruction *instruction = &block->instructions[i];
                if ((instruction->operand_count && !instruction->operands) ||
                    (instruction->successor_count && !instruction->successors) ||
                    instruction->successor_count > 4u ||
                    (size_t) instruction->operand_count > SIZE_MAX / sizeof(uint32_t)) {
                    xr_backend_set_diagnostic(diagnostic, XR_BACKEND_INVARIANT_REJECTED,
                                              instruction->operation_id, f, b, i);
                    return false;
                }
            }
        }
    }
    return true;
}

/* Successor roles are determined by the operation, never by the destination. */
static bool instruction_control_shape_valid(const XrBackendIR *ir,
                                            const XrBackendFunction *function,
                                            const XrBackendInstruction *instruction) {
    uint32_t successors = instruction->successor_count;
    switch (instruction->operation_id) {
        case XR_CORE_OP_CORE_BRANCH:
            return successors == 1u &&
                   instruction->operand_count ==
                       function->blocks[instruction->successors[0]].argument_count;
        case XR_CORE_OP_CORE_CONDITIONAL_BRANCH:
            return successors == 2u && instruction->operand_count != 0u &&
                   function->value_types[instruction->operands[0]] == XR_CORE_TYPE_BOOL &&
                   (uint64_t) instruction->operand_count ==
                       UINT64_C(1) + function->blocks[instruction->successors[0]].argument_count +
                           function->blocks[instruction->successors[1]].argument_count;
        case XR_CORE_OP_CORE_RETURN:
            return successors == 0u &&
                   instruction->operand_count ==
                       (function->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u);
        case XR_CORE_OP_CORE_CANCEL_PUBLISH:
        case XR_CORE_OP_CORE_TRAP:
            return successors == 0u && instruction->operand_count == 0u;
        case XR_CORE_OP_CORE_ERROR_PUBLISH:
        case XR_CORE_OP_CORE_PANIC_PUBLISH:
            return successors == 0u && instruction->operand_count == 1u;
        case XR_CORE_OP_CORE_COROUTINE_YIELD:
        case XR_CORE_OP_CORE_COROUTINE_SUSPEND:
            return successors == 2u;
        case XR_CORE_OP_CORE_COROUTINE_CALL_SEALED:
        case XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT:
            return successors == 2u || successors == 3u;
        case XR_CORE_OP_CORE_CALL_SEALED_INVOKE:
        case XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE:
        case XR_CORE_OP_CORE_CALL_WITNESS_INVOKE: {
            uint32_t typed = invoke_typed_successor_count(ir, function, instruction);
            return typed > 1u && (successors == typed || successors == typed + 1u);
        }
        case XR_CORE_OP_CORE_PROVIDER_CALL:
        case XR_CORE_OP_CORE_CALL_SEALED_DIRECT:
        case XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT:
        case XR_CORE_OP_CORE_CALL_WITNESS_DIRECT:
        case XR_CORE_OP_CORE_ASSERT_CONDITION:
            return successors <= 1u;
        default:
            return successors == 0u;
    }
}

typedef struct BackendCallAuthority {
    const XrParamMode *modes;
    uint32_t count;
    uint32_t prefix;
    uint16_t result_type;
    uint16_t error_type;
    uint16_t panic_type;
    XrCoreIrOwnershipDisposition result_ownership;
} BackendCallAuthority;

static bool backend_call_authority(const XrBackendIR *ir, const XrBackendFunction *function,
                                   const XrBackendInstruction *instruction,
                                   BackendCallAuthority *call) {
    memset(call, 0, sizeof(*call));
    const XrValidatedSignature *signature = NULL;
    uint32_t callee_id = XR_PROGRAM_LOCATION_NONE;
    switch (instruction->operation_id) {
        case XR_CORE_OP_CORE_CALL_SEALED_DIRECT:
        case XR_CORE_OP_CORE_CALL_SEALED_INVOKE:
            callee_id = instruction->immediate.function_id;
            break;
        case XR_CORE_OP_CORE_COROUTINE_CALL_SEALED:
            callee_id = instruction->immediate.coroutine_call.function_id;
            break;
        case XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT:
        case XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE:
        case XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT:
            signature = callable_invoke_signature(ir, function, instruction);
            call->prefix = 1u;
            break;
        case XR_CORE_OP_CORE_CALL_WITNESS_DIRECT:
        case XR_CORE_OP_CORE_CALL_WITNESS_INVOKE:
            signature = witness_invoke_signature(ir, function, instruction);
            break;
        default:
            return false;
    }
    if (callee_id < ir->function_count) {
        const XrBackendFunction *callee = &ir->functions[callee_id];
        call->modes = callee->parameter_modes;
        call->count = callee->parameter_count;
        call->result_type = callee->result_type_id;
        call->result_ownership = callee->result_ownership;
        call->error_type = callee->error_type_id;
        call->panic_type = callee->panic_type_id;
    } else if (signature) {
        call->modes = signature->parameter_modes;
        call->count = signature->parameter_count;
        call->result_type = signature->result_type_id;
        call->result_ownership = signature->result_ownership;
        call->error_type = signature->error_type_id;
        call->panic_type = signature->panic_type_id;
    } else {
        return false;
    }
    return call->prefix <= instruction->operand_count &&
           call->count <= instruction->operand_count - call->prefix &&
           (call->count == 0u || call->modes);
}

typedef struct BackendValueState {
    const XrBackendInstruction *definition;
    uint64_t edge_epoch;
    uint32_t block;
    uint32_t position;
    bool live_owner;
} BackendValueState;

typedef struct BackendOwnerCheck {
    const XrBackendIR *ir;
    const XrBackendFunction *function;
    BackendValueState *values;
    uint64_t edge_epoch;
    uint32_t block;
    uint32_t position;
    uint32_t live_count;
} BackendOwnerCheck;

static bool backend_parameter_is_class_receiver(const XrBackendIR *ir,
                                                const XrBackendFunction *function,
                                                uint32_t parameter) {
    if (!ir || !function || !function->parameter_types || !function->parameter_modes ||
        !function->parameter_count || parameter != 0u ||
        function->parameter_modes[0] != XR_PARAM_REF)
        return false;
    const XrValidatedType *type =
        xr_validated_program_type(ir->program, function->parameter_types[0]);
    return type && type->kind == XR_CORE_IR_TYPE_CLASS_REFERENCE;
}

static bool backend_value_definitions(BackendOwnerCheck *check) {
    const XrBackendFunction *function = check->function;
    for (uint32_t b = 0u; b < function->block_count; ++b) {
        const XrBackendBlock *block = &function->blocks[b];
        check->block = b;
        check->position = 0u;
        for (uint32_t a = 0u; a < block->argument_count; ++a) {
            BackendValueState *value = &check->values[block->argument_ids[a]];
            if (value->block != 0u)
                return false;
            value->block = b + 1u;
        }
        for (uint32_t i = 0u; i < block->instruction_count; ++i) {
            const XrBackendInstruction *instruction = &block->instructions[i];
            check->position = i;
            if (instruction->result_id == XR_PROGRAM_LOCATION_NONE)
                continue;
            BackendValueState *value = &check->values[instruction->result_id];
            if (value->block != 0u ||
                instruction->result_type_id != function->value_types[instruction->result_id])
                return false;
            value->block = b + 1u;
            value->position = i + 1u;
            value->definition = instruction;
        }
    }
    for (uint32_t v = 0u; v < function->value_count; ++v)
        if (check->values[v].block == 0u)
            return false;
    return true;
}

/* Only affine views and places retain a local owner dependency. Projected
 * trivial values are
 * snapshots and remain usable after their source is moved. */
static uint32_t backend_local_owner_root(const BackendOwnerCheck *check, uint32_t value) {
    const XrBackendFunction *function = check->function;
    for (uint32_t depth = 0u; depth < function->value_count; ++depth) {
        if (function->value_ownerships[value] == XR_CORE_IR_OWNER)
            return value;
        const XrBackendInstruction *definition = check->values[value].definition;
        if (definition && definition->operation_id == XR_CORE_OP_CORE_EXISTENTIAL_REBORROW_READ &&
            definition->operand_count == 1u) {
            value = definition->operands[0];
            continue;
        }
        if (function->value_categories[value] != XR_CORE_IR_PLACE &&
            xr_validated_program_type_ownership(check->ir->program, function->value_types[value]) !=
                XR_CORE_IR_TYPE_OWNERSHIP_AFFINE)
            return XR_PROGRAM_LOCATION_NONE;
        if (!definition || definition->operand_count == 0u)
            return XR_PROGRAM_LOCATION_NONE;
        switch (definition->operation_id) {
            case XR_CORE_OP_CORE_PLACE_LOCAL:
            case XR_CORE_OP_CORE_PLACE_PROJECT:
            case XR_CORE_OP_CORE_PLACE_LOAD:
            case XR_CORE_OP_CORE_CLASS_FIELD_LOAD:
            case XR_CORE_OP_CORE_CLASS_FIELD_PLACE:
            case XR_CORE_OP_CORE_AGGREGATE_PROJECT:
            case XR_CORE_OP_CORE_VARIANT_PROJECT:
            case XR_CORE_OP_CORE_EXISTENTIAL_PROJECT:
            case XR_CORE_OP_CORE_EXISTENTIAL_REBORROW_READ:
                value = definition->operands[0];
                break;
            default:
                return XR_PROGRAM_LOCATION_NONE;
        }
    }
    return XR_PROGRAM_LOCATION_NONE;
}

static bool backend_operand_consumed(const BackendOwnerCheck *check,
                                     const XrBackendInstruction *instruction, uint32_t operand) {
    switch (instruction->operation_id) {
        case XR_CORE_OP_CORE_OWNER_MOVE:
        case XR_CORE_OP_CORE_OWNER_DROP:
        case XR_CORE_OP_CORE_PLACE_TAKE:
        case XR_CORE_OP_CORE_RETURN:
        case XR_CORE_OP_CORE_ERROR_PUBLISH:
        case XR_CORE_OP_CORE_PANIC_PUBLISH:
        case XR_CORE_OP_CORE_CALLABLE_PACK:
        case XR_CORE_OP_CORE_EXISTENTIAL_PROJECT:
            return operand == 0u;
        case XR_CORE_OP_CORE_PLACE_STORE:
            return operand == 1u;
        case XR_CORE_OP_CORE_PLACE_EXCHANGE:
            return operand == 1u;
        case XR_CORE_OP_CORE_CLASS_CONSTRUCT:
        case XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT:
        case XR_CORE_OP_CORE_VARIANT_CONSTRUCT:
            return true;
        case XR_CORE_OP_CORE_VARIANT_PROJECT:
            return operand == 0u && instruction->result_ownership == XR_CORE_IR_OWNER;
        case XR_CORE_OP_CORE_EXISTENTIAL_PACK: {
            const XrValidatedType *type =
                xr_validated_program_type(check->ir->program, instruction->result_type_id);
            return operand == 0u && type && type->kind == XR_CORE_IR_TYPE_EXISTENTIAL &&
                   (type->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_MOVE ||
                    type->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE);
        }
        default: {
            BackendCallAuthority call;
            return backend_call_authority(check->ir, check->function, instruction, &call) &&
                   operand >= call.prefix && operand - call.prefix < call.count &&
                   call.modes[operand - call.prefix] == XR_PARAM_MOVE;
        }
    }
}

static bool backend_owner_operands(BackendOwnerCheck *check,
                                   const XrBackendInstruction *instruction) {
    const XrBackendFunction *function = check->function;
    for (uint32_t operand = 0u; operand < instruction->operand_count; ++operand) {
        uint32_t value_id = instruction->operands[operand];
        BackendValueState *value = &check->values[value_id];
        if (value->block != check->block + 1u || value->position > check->position)
            return false;
        uint32_t root = backend_local_owner_root(check, value_id);
        if (root != XR_PROGRAM_LOCATION_NONE && !check->values[root].live_owner)
            return false;
        if (!backend_operand_consumed(check, instruction, operand))
            continue;
        uint32_t owner = function->value_ownerships[value_id] == XR_CORE_IR_OWNER ? value_id
                         : instruction->operation_id == XR_CORE_OP_CORE_PLACE_TAKE
                             ? root
                             : XR_PROGRAM_LOCATION_NONE;
        if (owner != XR_PROGRAM_LOCATION_NONE) {
            check->values[owner].live_owner = false;
            --check->live_count;
        }
    }
    return true;
}

typedef struct BackendEdgeSegment {
    uint32_t start;
    uint32_t implicit;
    uint16_t implicit_type;
    XrCoreIrOwnershipDisposition implicit_ownership;
} BackendEdgeSegment;

static bool backend_edge_segment(const BackendOwnerCheck *check,
                                 const XrBackendInstruction *instruction, uint32_t edge,
                                 BackendEdgeSegment *segment) {
    const XrBackendFunction *function = check->function;
    const XrBackendBlock *target = &function->blocks[instruction->successors[edge]];
    memset(segment, 0, sizeof(*segment));
    uint64_t start = 0u;
    switch (instruction->operation_id) {
        case XR_CORE_OP_CORE_BRANCH:
            break;
        case XR_CORE_OP_CORE_CONDITIONAL_BRANCH:
            start = UINT64_C(1) +
                    (edge ? function->blocks[instruction->successors[0]].argument_count : 0u);
            break;
        case XR_CORE_OP_CORE_COROUTINE_YIELD:
            start = edge ? function->blocks[instruction->successors[0]].argument_count : 0u;
            break;
        case XR_CORE_OP_CORE_COROUTINE_SUSPEND:
            start = instruction->immediate.coroutine_suspend.request_operand_count +
                    (edge ? function->blocks[instruction->successors[0]].argument_count : 0u);
            break;
        case XR_CORE_OP_CORE_ASSERT_CONDITION:
            start = 1u;
            segment->implicit = 1u;
            segment->implicit_type = XR_CORE_TYPE_PANIC_INFO;
            segment->implicit_ownership = XR_CORE_IR_OWNER;
            break;
        case XR_CORE_OP_CORE_PROVIDER_CALL:
        case XR_CORE_OP_CORE_CALL_SEALED_DIRECT:
        case XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT:
        case XR_CORE_OP_CORE_CALL_WITNESS_DIRECT:
            if (target->argument_count > instruction->operand_count)
                return false;
            start = instruction->operand_count - target->argument_count;
            break;
        default: {
            BackendCallAuthority call;
            if (!backend_call_authority(check->ir, function, instruction, &call))
                return false;
            bool coroutine = instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED ||
                             instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT;
            uint32_t typed =
                coroutine ? 1u : invoke_typed_successor_count(check->ir, function, instruction);
            start = (uint64_t) call.prefix + call.count;
            for (uint32_t prior = 0u; prior <= edge; ++prior) {
                const XrBackendBlock *row = &function->blocks[instruction->successors[prior]];
                uint32_t implicit =
                    prior < typed && (prior != 0u || call.result_type != XR_CORE_TYPE_VOID);
                if (row->argument_count < implicit)
                    return false;
                if (prior == edge) {
                    segment->implicit = implicit;
                    segment->implicit_type = prior == 0u ? call.result_type
                                             : prior == 1u && call.error_type != XR_CORE_TYPE_VOID
                                                 ? call.error_type
                                                 : call.panic_type;
                    segment->implicit_ownership =
                        prior == 0u ? call.result_ownership
                        : xr_validated_program_type_ownership(check->ir->program,
                                                              segment->implicit_type) ==
                                XR_CORE_IR_TYPE_OWNERSHIP_AFFINE
                            ? XR_CORE_IR_OWNER
                            : XR_CORE_IR_NON_OWNER;
                } else {
                    start += row->argument_count - implicit;
                }
            }
            break;
        }
    }
    if (start > instruction->operand_count || target->argument_count < segment->implicit ||
        target->argument_count - segment->implicit > instruction->operand_count - start)
        return false;
    segment->start = (uint32_t) start;
    return true;
}

static bool backend_owner_edge(BackendOwnerCheck *check, const XrBackendInstruction *instruction,
                               uint32_t edge) {
    BackendEdgeSegment segment;
    if (!backend_edge_segment(check, instruction, edge, &segment))
        return false;
    const XrBackendFunction *function = check->function;
    const XrBackendBlock *target = &function->blocks[instruction->successors[edge]];
    if (segment.implicit && (target->argument_types[0] != segment.implicit_type ||
                             target->argument_categories[0] != XR_CORE_IR_VALUE ||
                             target->argument_ownerships[0] != segment.implicit_ownership))
        return false;
    uint32_t transferred = 0u;
    uint64_t epoch = ++check->edge_epoch;
    for (uint32_t a = segment.implicit; a < target->argument_count; ++a) {
        uint32_t source = instruction->operands[segment.start + a - segment.implicit];
        BackendValueState *value = &check->values[source];
        if (target->argument_types[a] != function->value_types[source] ||
            target->argument_categories[a] != function->value_categories[source] ||
            target->argument_ownerships[a] != function->value_ownerships[source])
            return false;
        if (function->value_ownerships[source] == XR_CORE_IR_OWNER) {
            if (!value->live_owner || value->edge_epoch == epoch)
                return false;
            value->edge_epoch = epoch;
            ++transferred;
        }
    }
    return transferred == check->live_count;
}

/* Edge checks observe parameter transfer before the result exists. A refusal
 * side edge checks a
 * snapshot; it does not consume the ordinary path's owners. */
static bool backend_owner_block(BackendOwnerCheck *check) {
    const XrBackendBlock *block = &check->function->blocks[check->block];
    check->live_count = 0u;
    for (uint32_t a = 0u; a < block->argument_count; ++a) {
        uint32_t value = block->argument_ids[a];
        bool owner = block->argument_ownerships[a] == XR_CORE_IR_OWNER;
        check->values[value].live_owner = owner;
        check->live_count += owner;
    }
    for (uint32_t i = 0u; i < block->instruction_count; ++i) {
        const XrBackendInstruction *instruction = &block->instructions[i];
        check->position = i;
        if (!backend_owner_operands(check, instruction))
            return false;
        for (uint32_t edge = 0u; edge < instruction->successor_count; ++edge)
            if (!backend_owner_edge(check, instruction, edge))
                return false;
        if (instruction->result_id != XR_PROGRAM_LOCATION_NONE) {
            bool owner = instruction->result_ownership == XR_CORE_IR_OWNER;
            check->values[instruction->result_id].live_owner = owner;
            check->live_count += owner;
        }
        if (backend_instruction_is_terminal(instruction->operation_id) &&
            instruction->successor_count == 0u && check->live_count != 0u)
            return false;
    }
    return true;
}

static bool backend_owner_flow_valid(const XrBackendIR *ir, uint32_t function_id,
                                     XrBackendDiagnostic *diagnostic) {
    const XrBackendFunction *function = &ir->functions[function_id];
    if ((size_t) function->value_count > SIZE_MAX / sizeof(BackendValueState)) {
        xr_backend_set_diagnostic(diagnostic, XR_BACKEND_RESOURCE_LIMIT, 0u, function_id, 0u, 0u);
        return false;
    }
    BackendValueState *values =
        function->value_count ? xr_calloc(function->value_count, sizeof(*values)) : NULL;
    if (function->value_count && !values) {
        xr_backend_set_diagnostic(diagnostic, XR_BACKEND_OUT_OF_MEMORY, 0u, function_id, 0u, 0u);
        return false;
    }
    BackendOwnerCheck check = {.ir = ir, .function = function, .values = values};
    bool valid = backend_value_definitions(&check);
    for (uint32_t b = 0u; valid && b < function->block_count; ++b) {
        check.block = b;
        valid = backend_owner_block(&check);
    }
    if (!valid) {
        const XrBackendInstruction *instruction =
            &function->blocks[check.block].instructions[check.position];
        xr_backend_set_diagnostic(diagnostic, XR_BACKEND_INVARIANT_REJECTED,
                                  instruction->operation_id, function_id, check.block,
                                  check.position);
    }
    xr_free(values);
    return valid;
}

typedef enum BackendCleanupReason {
    BACKEND_REASON_UNREACHED,
    BACKEND_REASON_NORMAL,
    BACKEND_REASON_TRAP7,
    BACKEND_REASON_CANCEL,
} BackendCleanupReason;

static bool cleanup_exit_valid(const XrBackendInstruction *instruction, uint8_t reason) {
    if (!backend_instruction_is_terminal(instruction->operation_id) ||
        instruction->successor_count != 0u)
        return true;
    if (reason == BACKEND_REASON_TRAP7)
        return instruction->operation_id == XR_CORE_OP_CORE_TRAP &&
               instruction->immediate.u32 == 7u;
    if (reason == BACKEND_REASON_CANCEL)
        return instruction->operation_id == XR_CORE_OP_CORE_CANCEL_PUBLISH;
    return instruction->operation_id != XR_CORE_OP_CORE_CANCEL_PUBLISH;
}

/* Each block is visited once. Same-reason cycles are legal; this is not a
 * termination proof.
 * Distinct reasons cannot share a block or explicit exit. */
static bool cleanup_reason_flow_valid(const XrBackendIR *ir, uint32_t function_id,
                                      XrBackendDiagnostic *diagnostic) {
    const XrBackendFunction *function = &ir->functions[function_id];
    if ((size_t) function->block_count > SIZE_MAX / sizeof(uint32_t)) {
        xr_backend_set_diagnostic(diagnostic, XR_BACKEND_RESOURCE_LIMIT, 0u, function_id, 0u, 0u);
        return false;
    }
    uint8_t *reasons = xr_calloc(function->block_count, sizeof(*reasons));
    uint32_t *queue = xr_malloc((size_t) function->block_count * sizeof(*queue));
    if (!reasons || !queue) {
        xr_free(reasons);
        xr_free(queue);
        xr_backend_set_diagnostic(diagnostic, XR_BACKEND_OUT_OF_MEMORY, 0u, function_id, 0u, 0u);
        return false;
    }
    uint32_t head = 0u, tail = 1u;
    queue[0] = function->entry_block;
    reasons[queue[0]] = BACKEND_REASON_NORMAL;
    bool valid = true;
    while (valid && head < tail) {
        uint32_t block_id = queue[head++];
        const XrBackendBlock *block = &function->blocks[block_id];
        uint8_t reason = reasons[block_id];
        for (uint32_t i = 0u; valid && i < block->instruction_count; ++i) {
            const XrBackendInstruction *instruction = &block->instructions[i];
            bool suspension = instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_YIELD ||
                              instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_SUSPEND ||
                              instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED ||
                              instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT;
            valid = cleanup_exit_valid(instruction, reason) &&
                    (!suspension || reason == BACKEND_REASON_NORMAL);
            uint32_t trap = instruction_trap_successor(ir, function, instruction);
            for (uint32_t s = 0u; valid && s < instruction->successor_count; ++s) {
                uint32_t target = instruction->successors[s];
                uint8_t next = s == trap ? BACKEND_REASON_TRAP7 : reason;
                if (suspension && s == 1u)
                    next = BACKEND_REASON_CANCEL;
                if (reasons[target] == BACKEND_REASON_UNREACHED) {
                    reasons[target] = next;
                    queue[tail++] = target;
                } else if (reasons[target] != next) {
                    valid = false;
                }
            }
            if (!valid)
                xr_backend_set_diagnostic(diagnostic, XR_BACKEND_INVARIANT_REJECTED,
                                          instruction->operation_id, function_id, block_id, i);
        }
    }
    if (valid && tail != function->block_count) {
        valid = false;
        for (uint32_t b = 0u; b < function->block_count; ++b) {
            if (reasons[b] == BACKEND_REASON_UNREACHED) {
                xr_backend_set_diagnostic(diagnostic, XR_BACKEND_INVARIANT_REJECTED, 0u,
                                          function_id, b, 0u);
                break;
            }
        }
    }
    xr_free(queue);
    xr_free(reasons);
    return valid;
}

static const XrBackendInstruction *backend_value_instruction(const XrBackendFunction *function,
                                                             uint32_t value_id) {
    const XrBackendInstruction *found = NULL;
    for (uint32_t block = 0u; function && block < function->block_count; ++block) {
        const XrBackendBlock *row = &function->blocks[block];
        for (uint32_t instruction = 0u; instruction < row->instruction_count; ++instruction) {
            const XrBackendInstruction *candidate = &row->instructions[instruction];
            if (candidate->result_id != value_id)
                continue;
            if (found)
                return NULL;
            found = candidate;
        }
    }
    return found;
}

static bool coroutine_ref_argument_is_frame_stable(const XrBackendIR *ir,
                                                   const XrBackendFunction *function,
                                                   const XrBackendInstruction *call,
                                                   const XrBackendCoroutineSafepoint *point,
                                                   uint32_t parameter) {
    if (!ir || !function || !call || !point || parameter >= call->operand_count)
        return false;
    uint32_t place = call->operands[parameter];
    for (uint32_t depth = 0u; depth < function->value_count; ++depth) {
        if (place >= function->value_count ||
            function->value_categories[place] != XR_CORE_IR_PLACE ||
            function->value_ownerships[place] != XR_CORE_IR_NON_OWNER)
            return false;
        const XrBackendInstruction *definition = backend_value_instruction(function, place);
        if (!definition)
            return depth == 0u;
        if (definition->operand_count != 1u || !definition->operands ||
            definition->operands[0] >= function->value_count ||
            definition->result_type_id != function->value_types[place])
            return false;
        uint32_t base = definition->operands[0];
        if (definition->operation_id == XR_CORE_OP_CORE_PLACE_PROJECT) {
            const XrValidatedType *aggregate =
                xr_validated_program_type(ir->program, function->value_types[base]);
            if (definition->immediate_kind != XR_CORE_IR_IMMEDIATE_FIELD || !aggregate ||
                aggregate->kind != XR_CORE_IR_TYPE_AGGREGATE ||
                definition->immediate.field_ordinal >= aggregate->field_count ||
                aggregate->field_types[definition->immediate.field_ordinal] !=
                    function->value_types[place])
                return false;
            place = base;
            continue;
        }
        if (definition->operation_id != XR_CORE_OP_CORE_PLACE_LOCAL ||
            definition->immediate_kind != XR_CORE_IR_IMMEDIATE_NONE ||
            function->value_categories[base] != XR_CORE_IR_VALUE ||
            function->value_types[base] != function->value_types[place])
            return false;
        uint32_t live_count = 0u;
        for (uint32_t live = 0u; live < point->live_value_count; ++live)
            live_count += point->live_value_ids[live] == base;
        return live_count == 1u;
    }
    return false;
}

static bool coroutine_read_argument_is_frame_stable(const XrBackendIR *ir,
                                                    const XrBackendFunction *function,
                                                    const XrBackendInstruction *call,
                                                    const XrBackendCoroutineSafepoint *point,
                                                    uint32_t block_id, uint32_t parameter) {
    if (!ir || !function || !call || !point || parameter >= call->operand_count)
        return false;
    uint32_t function_id = (uint32_t) (function - ir->functions);
    if (function_id >= ir->function_count || function_id >= ir->program->function_count)
        return false;
    uint32_t borrow = call->operands[parameter];
    if (borrow >= function->value_count)
        return false;
    if (function->value_categories[borrow] == XR_CORE_IR_VALUE &&
        function->value_ownerships[borrow] == XR_CORE_IR_OWNER) {
        uint32_t owner_live_count = 0u;
        for (uint32_t live = 0u; live < point->live_value_count; ++live)
            owner_live_count += point->live_value_ids[live] == borrow ? 1u : 0u;
        return owner_live_count == 1u;
    }
    uint16_t borrow_type_id = function->value_types[borrow];
    bool scalar = borrow_type_id >= XR_CORE_TYPE_BOOL &&
                  borrow_type_id <= XR_CORE_TYPE_TARGET_ENDIAN &&
                  borrow_type_id != XR_CORE_TYPE_ERROR && borrow_type_id != XR_CORE_TYPE_PANIC_INFO;
    if (scalar)
        return true;
    const XrValidatedFunction *validated = &ir->program->functions[function_id];
    if (xr_validated_function_frame_stable_callable(ir->program, validated, borrow))
        return true;
    uint32_t owner =
        xr_validated_function_scoped_affine_borrow_owner(ir->program, validated, borrow, block_id);
    if (owner >= function->value_count || function->value_categories[borrow] != XR_CORE_IR_VALUE ||
        function->value_ownerships[borrow] != XR_CORE_IR_NON_OWNER ||
        function->value_categories[owner] != XR_CORE_IR_VALUE ||
        function->value_ownerships[owner] != XR_CORE_IR_OWNER)
        return false;
    const XrValidatedType *borrow_type = xr_validated_program_type(ir->program, borrow_type_id);
    const XrValidatedType *owner_type =
        xr_validated_program_type(ir->program, function->value_types[owner]);
    uint32_t owner_live_count = 0u;
    for (uint32_t live = 0u; live < point->live_value_count; ++live)
        owner_live_count += point->live_value_ids[live] == owner;
    return borrow_type && owner_type && borrow_type->kind == XR_CORE_IR_TYPE_EXISTENTIAL &&
           owner_type->kind == XR_CORE_IR_TYPE_EXISTENTIAL &&
           borrow_type->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_READ &&
           (owner_type->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_MOVE ||
            owner_type->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE) &&
           borrow_type->interface_id == owner_type->interface_id && owner_live_count == 1u;
}

static bool safepoint_live_set_matches(const XrBackendFunction *function,
                                       const XrBackendInstruction *instruction,
                                       const XrBackendCoroutineSafepoint *point,
                                       uint32_t live_operand_start) {
    if (!function || !instruction || !point || live_operand_start > instruction->operand_count ||
        point->live_value_count > instruction->operand_count - live_operand_start)
        return false;
    for (uint32_t live = 0u; live < point->live_value_count; ++live) {
        uint32_t value = instruction->operands[live_operand_start + live];
        uint32_t occurrences = 0u;
        for (uint32_t candidate = 0u; candidate < point->live_value_count; ++candidate)
            occurrences += point->live_value_ids[candidate] == value;
        if (value >= function->value_count || occurrences != 1u)
            return false;
    }
    for (uint32_t live = 0u; live < point->live_value_count; ++live) {
        uint32_t occurrences = 0u;
        for (uint32_t operand = 0u; operand < point->live_value_count; ++operand)
            occurrences +=
                instruction->operands[live_operand_start + operand] == point->live_value_ids[live];
        if (occurrences != 1u)
            return false;
    }
    return true;
}

static bool cancel_continuation_matches(const XrBackendFunction *function,
                                        const XrBackendInstruction *instruction,
                                        const XrBackendCoroutineSafepoint *point,
                                        uint32_t live_operand_start) {
    if (!function || !instruction ||
        (instruction->successor_count != 2u &&
         !((instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED ||
            instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT) &&
           instruction->successor_count == 3u)) ||
        instruction->successors[1] >= function->block_count || !point ||
        live_operand_start > instruction->operand_count ||
        point->live_value_count > instruction->operand_count - live_operand_start ||
        !safepoint_live_set_matches(function, instruction, point, live_operand_start))
        return false;
    uint32_t cancel_operand_start = live_operand_start + point->live_value_count;
    const XrBackendBlock *cancel = &function->blocks[instruction->successors[1]];
    uint32_t cancel_count = cancel->argument_count;
    if (!cancel->instructions ||
        (cancel_count != 0u && (!cancel->argument_ids || !cancel->argument_types ||
                                !cancel->argument_categories || !cancel->argument_ownerships)) ||
        cancel_count > instruction->operand_count - cancel_operand_start ||
        (instruction->successor_count == 2u &&
         cancel_count != instruction->operand_count - cancel_operand_start) ||
        cancel->instruction_count == 0u)
        return false;
    for (uint32_t live = 0u; live < point->live_value_count; ++live) {
        uint32_t source = instruction->operands[live_operand_start + live];
        uint32_t cancel_occurrences = 0u;
        for (uint32_t index = 0u; index < cancel_count; ++index)
            cancel_occurrences += instruction->operands[cancel_operand_start + index] == source;
        bool owner = source < function->value_count &&
                     function->value_ownerships[source] == XR_CORE_IR_OWNER;
        if (source >= function->value_count || cancel_occurrences > 1u ||
            (owner && cancel_occurrences != 1u))
            return false;
    }
    for (uint32_t index = 0u; index < cancel_count; ++index) {
        uint32_t source = instruction->operands[cancel_operand_start + index];
        uint32_t target = cancel->argument_ids[index];
        uint32_t live_occurrences = 0u;
        for (uint32_t live = 0u; live < point->live_value_count; ++live)
            live_occurrences += point->live_value_ids[live] == source;
        if (source >= function->value_count || target >= function->value_count ||
            live_occurrences != 1u || function->value_categories[source] > XR_CORE_IR_PLACE ||
            cancel->argument_types[index] != function->value_types[source] ||
            cancel->argument_categories[index] != function->value_categories[source] ||
            cancel->argument_ownerships[index] != function->value_ownerships[source])
            return false;
    }
    return true;
}

static bool coroutine_trap_continuation_matches(const XrBackendFunction *function,
                                                const XrBackendInstruction *instruction,
                                                const XrBackendCoroutineSafepoint *point,
                                                uint32_t parameter_count) {
    if (instruction->successor_count == 2u)
        return true;
    if (instruction->successor_count != 3u || parameter_count > instruction->operand_count ||
        point->live_value_count > instruction->operand_count - parameter_count)
        return false;
    uint32_t start = parameter_count + point->live_value_count;
    const XrBackendBlock *cancel = &function->blocks[instruction->successors[1]];
    const XrBackendBlock *trap = &function->blocks[instruction->successors[2]];
    if (trap->argument_count != 0u && (!trap->argument_ids || !trap->argument_types ||
                                       !trap->argument_categories || !trap->argument_ownerships))
        return false;
    if (cancel->argument_count > instruction->operand_count - start)
        return false;
    start += cancel->argument_count;
    if (trap->argument_count != instruction->operand_count - start)
        return false;
    for (uint32_t live = 0u; live < point->live_value_count; ++live) {
        uint32_t value = point->live_value_ids[live];
        if (function->value_ownerships[value] != XR_CORE_IR_OWNER)
            continue;
        uint32_t occurrences = 0u;
        for (uint32_t argument = 0u; argument < trap->argument_count; ++argument)
            occurrences += instruction->operands[start + argument] == value;
        if (occurrences != 1u)
            return false;
    }
    for (uint32_t argument = 0u; argument < trap->argument_count; ++argument) {
        uint32_t value = instruction->operands[start + argument];
        uint32_t occurrences = 0u;
        for (uint32_t live = 0u; live < point->live_value_count; ++live)
            occurrences += point->live_value_ids[live] == value;
        if (occurrences != 1u || function->value_categories[value] != XR_CORE_IR_VALUE ||
            trap->argument_types[argument] != function->value_types[value] ||
            trap->argument_categories[argument] != function->value_categories[value] ||
            trap->argument_ownerships[argument] != function->value_ownerships[value])
            return false;
    }
    return true;
}

static bool instruction_shape_valid(const XrBackendIR *ir, const XrBackendFunction *function,
                                    const XrBackendInstruction *instruction) {
    if (instruction->result_id != XR_PROGRAM_LOCATION_NONE &&
        instruction->result_id >= function->value_count)
        return false;
    if (instruction->result_category > XR_CORE_IR_PLACE ||
        instruction->result_ownership > XR_CORE_IR_OWNER ||
        (instruction->result_id == XR_PROGRAM_LOCATION_NONE &&
         instruction->result_category != XR_CORE_IR_VALUE) ||
        (instruction->result_id != XR_PROGRAM_LOCATION_NONE &&
         (instruction->result_category != function->value_categories[instruction->result_id] ||
          instruction->result_ownership != function->value_ownerships[instruction->result_id])))
        return false;
    for (uint32_t operand = 0; operand < instruction->operand_count; ++operand) {
        if (!instruction->operands || instruction->operands[operand] >= function->value_count)
            return false;
    }
    for (uint32_t successor = 0; successor < instruction->successor_count; ++successor) {
        if (!instruction->successors || instruction->successors[successor] >= function->block_count)
            return false;
    }
    switch (instruction->operation_id) {
        case XR_CORE_OP_CORE_CONSTANT_I64:
        case XR_CORE_OP_CORE_CONSTANT_BOOL:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_CONSTANT &&
                   instruction->immediate.constant_id < ir->constant_count;
        case XR_CORE_OP_CORE_CONSTANT_TARGET_ENUM:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_U32 &&
                   instruction->immediate.u32 <= UINT16_MAX &&
                   xr_target_query_enum_value_valid(instruction->result_type_id,
                                                    (uint16_t) instruction->immediate.u32);
        case XR_CORE_OP_CORE_ADD_I64:
        case XR_CORE_OP_CORE_SUB_I64:
        case XR_CORE_OP_CORE_MUL_I64:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_U32 &&
                   instruction->immediate.u32 <= 1u;
        case XR_CORE_OP_CORE_DIV_I64:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_U32 &&
                   instruction->immediate.u32 == 0u;
        case XR_CORE_OP_CORE_LOGICAL_NOT:
        case XR_CORE_OP_CORE_LOGICAL_AND:
        case XR_CORE_OP_CORE_LOGICAL_OR:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_NONE;
        case XR_CORE_OP_CORE_COMPARE_I64:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_U32 &&
                   instruction->immediate.u32 <= 5u;
        case XR_CORE_OP_CORE_COMPARE_TARGET_ENUM:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_U32 &&
                   instruction->immediate.u32 <= 1u && instruction->operand_count == 2u &&
                   function->value_types[instruction->operands[0]] >= XR_CORE_TYPE_TARGET_OS &&
                   function->value_types[instruction->operands[0]] <= XR_CORE_TYPE_TARGET_ENDIAN &&
                   function->value_types[instruction->operands[0]] ==
                       function->value_types[instruction->operands[1]];
        case XR_CORE_OP_CORE_ASSERT_CONDITION:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_U32 &&
                   instruction->immediate.u32 == XR_ASSERTION_FAILURE_CONDITION_FALSE &&
                   instruction->successor_count <= 1u;
        case XR_CORE_OP_CORE_COROUTINE_YIELD:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_U32 &&
                   instruction->immediate.u32 < function->coroutine_safepoint_count &&
                   instruction->successor_count == 2u;
        case XR_CORE_OP_CORE_COROUTINE_SUSPEND:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_COROUTINE_SUSPEND &&
                   instruction->immediate.coroutine_suspend.safepoint_id <
                       function->coroutine_safepoint_count &&
                   instruction->immediate.coroutine_suspend.request_kind ==
                       XR_SUSPENSION_REQUEST_TIMER_AFTER_MS &&
                   instruction->immediate.coroutine_suspend.request_operand_count == 1u &&
                   instruction->operand_count != 0u &&
                   function->value_types[instruction->operands[0]] == XR_CORE_TYPE_I64 &&
                   instruction->successor_count == 2u;
        case XR_CORE_OP_CORE_COROUTINE_CALL_SEALED:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_COROUTINE_CALL &&
                   instruction->immediate.coroutine_call.function_id < ir->function_count &&
                   instruction->immediate.coroutine_call.safepoint_id <
                       function->coroutine_safepoint_count &&
                   (instruction->successor_count == 2u || instruction->successor_count == 3u);
        case XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_U32 &&
                   instruction->immediate.u32 < function->coroutine_safepoint_count &&
                   callable_invoke_signature(ir, function, instruction) != NULL &&
                   (instruction->successor_count == 2u || instruction->successor_count == 3u);
        case XR_CORE_OP_CORE_BLOCK_ARGUMENT:
        case XR_CORE_OP_CORE_BRANCH:
        case XR_CORE_OP_CORE_CONDITIONAL_BRANCH:
        case XR_CORE_OP_CORE_RETURN:
        case XR_CORE_OP_CORE_CANCEL_PUBLISH:
        case XR_CORE_OP_CORE_ERROR_PUBLISH:
        case XR_CORE_OP_CORE_PANIC_PUBLISH:
        case XR_CORE_OP_CORE_TARGET_POINTER_WIDTH:
        case XR_CORE_OP_CORE_TARGET_OPERATING_SYSTEM:
        case XR_CORE_OP_CORE_TARGET_ARCHITECTURE:
        case XR_CORE_OP_CORE_TARGET_NATIVE_ABI:
        case XR_CORE_OP_CORE_TARGET_ENDIANNESS:
        case XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT:
        case XR_CORE_OP_CORE_EXISTENTIAL_PACK:
        case XR_CORE_OP_CORE_EXISTENTIAL_REBORROW_READ:
        case XR_CORE_OP_CORE_OWNER_COPY:
        case XR_CORE_OP_CORE_OWNER_MOVE:
        case XR_CORE_OP_CORE_OWNER_DROP:
        case XR_CORE_OP_CORE_PLACE_LOCAL:
        case XR_CORE_OP_CORE_PLACE_LOAD:
        case XR_CORE_OP_CORE_PLACE_STORE:
        case XR_CORE_OP_CORE_PLACE_TAKE:
        case XR_CORE_OP_CORE_CLASS_CONSTRUCT:
        case XR_CORE_OP_CORE_CLASS_SHARE:
        case XR_CORE_OP_CORE_PLACE_EXCHANGE:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_NONE;
        case XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_NONE &&
                   instruction->successor_count <= 1u;
        case XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE: {
            uint32_t typed = invoke_typed_successor_count(ir, function, instruction);
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_NONE && typed > 1u &&
                   (instruction->successor_count == typed ||
                    instruction->successor_count == typed + 1u);
        }
        case XR_CORE_OP_CORE_PROVIDER_CALL:
        case XR_CORE_OP_CORE_OUTPUT_GROUP_I64: {
            uint32_t requirement = instruction->immediate.provider_operation.requirement_index;
            uint32_t operation = instruction->immediate.provider_operation.operation_index;
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION &&
                   (instruction->operation_id != XR_CORE_OP_CORE_PROVIDER_CALL ||
                    instruction->successor_count <= 1u) &&
                   requirement < ir->program->provider_requirement_count &&
                   operation < ir->program->provider_requirements[requirement].operation_count;
        }
        case XR_CORE_OP_CORE_TRAP:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_U32 &&
                   (instruction->immediate.u32 == 4u || instruction->immediate.u32 == 7u);
        case XR_CORE_OP_CORE_CALL_SEALED_DIRECT:
        case XR_CORE_OP_CORE_CALL_SEALED_INVOKE:
        case XR_CORE_OP_CORE_CALLABLE_PACK:
            if (instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_FUNCTION ||
                instruction->immediate.function_id >= ir->function_count)
                return false;
            if (instruction->operation_id == XR_CORE_OP_CORE_CALLABLE_PACK)
                return true;
            if (instruction->operation_id == XR_CORE_OP_CORE_CALL_SEALED_DIRECT)
                return instruction->successor_count <= 1u;
            {
                uint32_t typed = invoke_typed_successor_count(ir, function, instruction);
                return typed > 1u && (instruction->successor_count == typed ||
                                      instruction->successor_count == typed + 1u);
            }
        case XR_CORE_OP_CORE_CALL_WITNESS_DIRECT:
        case XR_CORE_OP_CORE_CALL_WITNESS_INVOKE:
            if (instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_U32)
                return false;
            if (instruction->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_DIRECT)
                return instruction->successor_count <= 1u;
            {
                uint32_t typed = invoke_typed_successor_count(ir, function, instruction);
                return typed > 1u && (instruction->successor_count == typed ||
                                      instruction->successor_count == typed + 1u);
            }
        case XR_CORE_OP_CORE_AGGREGATE_PROJECT:
        case XR_CORE_OP_CORE_AGGREGATE_UPDATE:
        case XR_CORE_OP_CORE_PLACE_PROJECT:
        case XR_CORE_OP_CORE_CLASS_FIELD_LOAD:
        case XR_CORE_OP_CORE_CLASS_FIELD_PLACE:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_FIELD;
        case XR_CORE_OP_CORE_VARIANT_CONSTRUCT:
        case XR_CORE_OP_CORE_VARIANT_TEST:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_VARIANT;
        case XR_CORE_OP_CORE_VARIANT_PROJECT:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_VARIANT_FIELD;
        case XR_CORE_OP_CORE_EXISTENTIAL_TEST:
        case XR_CORE_OP_CORE_EXISTENTIAL_PROJECT:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_TYPE &&
                   instruction->immediate.type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE;
        default:
            return false;
    }
}

bool xr_backend_ir_verify(const XrBackendIR *ir, XrBackendDiagnostic *diagnostic_out) {
    xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_OK, 0u, 0u, 0u, 0u);
    if (!ir || !ir->program || !ir->profile || !ir->functions || ir->function_count == 0u ||
        (ir->constant_count != 0u && !ir->constants) || ir->entry_function >= ir->function_count ||
        (ir->pointer_width != 32u && ir->pointer_width != 64u) ||
        ir->operating_system <= XR_TARGET_OS_NONE || ir->operating_system >= XR_TARGET_OS_COUNT ||
        ir->architecture <= XR_TARGET_ARCH_NONE || ir->architecture >= XR_TARGET_ARCH_COUNT ||
        ir->native_abi <= XR_TARGET_ABI_NONE || ir->native_abi >= XR_TARGET_ABI_COUNT ||
        (ir->endianness != XR_TARGET_ENDIAN_LITTLE && ir->endianness != XR_TARGET_ENDIAN_BIG) ||
        fingerprint_is_zero(ir->execution_id) || fingerprint_is_zero(ir->backend_id) ||
        fingerprint_is_zero(ir->optimization_policy_id) ||
        fingerprint_is_zero(ir->lowering_digest)) {
        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u, 0u, 0u, 0u);
        return false;
    }
    for (uint32_t constant = 0; constant < ir->constant_count; ++constant) {
        const XrValidatedConstant *value = &ir->constants[constant];
        if ((value->kind == XR_CORE_IR_CONSTANT_I64 && value->type_id != XR_CORE_TYPE_I64) ||
            (value->kind == XR_CORE_IR_CONSTANT_BOOL && value->type_id != XR_CORE_TYPE_BOOL) ||
            (value->kind != XR_CORE_IR_CONSTANT_I64 && value->kind != XR_CORE_IR_CONSTANT_BOOL)) {
            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u, 0u, 0u,
                                      0u);
            return false;
        }
    }
    char profile_error[256] = {0};
    if (!xr_target_profile_verify(ir->profile, profile_error, sizeof(profile_error))) {
        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u, 0u, 0u, 0u);
        return false;
    }
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(ir->profile);
    uint16_t pointer_width =
        machine ? (uint16_t) (machine->data_layout.pointer.size * UINT16_C(8)) : 0u;
    XrBackendId expected_backend_id = xr_backend_compute_id();
    XrOptimizationPolicyId expected_optimization_policy_id =
        xr_backend_compute_optimization_policy_id(&ir->options);
    if (ir->pointer_width != pointer_width || !machine ||
        ir->operating_system != machine->operating_system ||
        ir->architecture != machine->architecture || ir->native_abi != machine->native_abi ||
        ir->endianness != machine->data_layout.endian ||
        memcmp(ir->backend_id.bytes, expected_backend_id.bytes, sizeof(ir->backend_id.bytes)) !=
            0 ||
        memcmp(ir->optimization_policy_id.bytes, expected_optimization_policy_id.bytes,
               sizeof(ir->optimization_policy_id.bytes)) != 0) {
        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u, 0u, 0u, 0u);
        return false;
    }
    if (!backend_storage_valid(ir, diagnostic_out))
        return false;
    for (uint32_t function_id = 0; function_id < ir->function_count; ++function_id) {
        const XrBackendFunction *function = &ir->functions[function_id];
        if (!function->blocks || function->block_count == 0u ||
            function->entry_block >= function->block_count ||
            (function->parameter_count != 0u &&
             (!function->parameter_types || !function->parameter_modes)) ||
            (function->value_count != 0u &&
             (!function->value_types || !function->value_categories ||
              !function->value_ownerships || !function->value_representations))) {
            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u,
                                      function_id, 0u, 0u);
            return false;
        }
        bool coroutine =
            function->coroutine_state_count != 0u || function->coroutine_safepoint_count != 0u;
        if ((coroutine &&
             (function->coroutine_safepoint_count == 0u ||
              function->coroutine_safepoint_count == UINT32_MAX ||
              function->coroutine_safepoint_count > ir->options.max_instructions ||
              function->coroutine_state_count != function->coroutine_safepoint_count + 1u ||
              !function->coroutine_states || !function->coroutine_safepoints ||
              function->coroutine_states[0].continuation_block != function->entry_block)) ||
            (!coroutine && (function->coroutine_states || function->coroutine_safepoints))) {
            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u,
                                      function_id, 0u, 0u);
            return false;
        }
        uint32_t suspension_count = 0u;
        if (coroutine &&
            ((function->effect_mask & XR_CORE_EFFECT_SUSPEND) == 0u ||
             (function->capability_mask & XR_CORE_CAPABILITY_RUNTIME_COROUTINE_SUSPENSION) == 0u)) {
            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u,
                                      function_id, 0u, 0u);
            return false;
        }
        for (uint32_t state = 1u; state < function->coroutine_state_count; ++state) {
            if (function->coroutine_states[state].continuation_block >= function->block_count) {
                xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u,
                                          function_id, 0u, 0u);
                return false;
            }
        }
        for (uint32_t safepoint = 0u; safepoint < function->coroutine_safepoint_count;
             ++safepoint) {
            const XrBackendCoroutineSafepoint *point = &function->coroutine_safepoints[safepoint];
            if (point->resume_state_id != safepoint + 1u ||
                point->resume_state_id >= function->coroutine_state_count ||
                point->live_value_count > function->value_count ||
                (point->live_value_count != 0u && !point->live_value_ids)) {
                xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u,
                                          function_id, 0u, 0u);
                return false;
            }
            for (uint32_t live = 0; live < point->live_value_count; ++live) {
                uint32_t value = point->live_value_ids[live];
                if (value >= function->value_count ||
                    function->value_categories[value] > XR_CORE_IR_PLACE ||
                    (function->value_categories[value] == XR_CORE_IR_PLACE &&
                     backend_value_instruction(function, value) != NULL)) {
                    xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u,
                                              function_id, 0u, 0u);
                    return false;
                }
            }
        }
        for (uint32_t value = 0; value < function->value_count; ++value) {
            uint8_t expected = 0u;
            if (!xr_backend_representation_for_program_type(
                    ir->program, function->value_types[value], &expected) ||
                function->value_categories[value] > XR_CORE_IR_PLACE ||
                function->value_ownerships[value] > XR_CORE_IR_OWNER ||
                expected != function->value_representations[value]) {
                xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u,
                                          function_id, 0u, 0u);
                return false;
            }
        }
        for (uint32_t parameter = 0; parameter < function->parameter_count; ++parameter) {
            if (!xr_param_mode_is_valid(function->parameter_modes[parameter])) {
                xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u,
                                          function_id, function->entry_block, 0u);
                return false;
            }
        }
        for (uint32_t block_id = 0; block_id < function->block_count; ++block_id) {
            const XrBackendBlock *block = &function->blocks[block_id];
            if ((block->argument_count != 0u &&
                 (!block->argument_ids || !block->argument_types || !block->argument_categories ||
                  !block->argument_ownerships)) ||
                (block->instruction_count != 0u && !block->instructions)) {
                xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u,
                                          function_id, block_id, 0u);
                return false;
            }
            for (uint32_t argument = 0; argument < block->argument_count; ++argument) {
                if (block->argument_ids[argument] >= function->value_count ||
                    block->argument_types[argument] !=
                        function->value_types[block->argument_ids[argument]] ||
                    block->argument_categories[argument] !=
                        function->value_categories[block->argument_ids[argument]] ||
                    block->argument_ownerships[argument] !=
                        function->value_ownerships[block->argument_ids[argument]]) {
                    xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u,
                                              function_id, block_id, 0u);
                    return false;
                }
            }
            for (uint32_t instruction_id = 0; instruction_id < block->instruction_count;
                 ++instruction_id) {
                const XrBackendInstruction *instruction = &block->instructions[instruction_id];
                if (!instruction_shape_valid(ir, function, instruction) ||
                    !instruction_control_shape_valid(ir, function, instruction) ||
                    backend_instruction_is_terminal(instruction->operation_id) !=
                        (instruction_id + 1u == block->instruction_count)) {
                    xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED,
                                              instruction->operation_id, function_id, block_id,
                                              instruction_id);
                    return false;
                }
                uint32_t trap_successor = instruction_trap_successor(ir, function, instruction);
                if (trap_successor != XR_PROGRAM_LOCATION_NONE) {
                    const XrBackendBlock *target =
                        &function->blocks[instruction->successors[trap_successor]];
                    if (!target->instructions ||
                        target->argument_count > instruction->operand_count ||
                        target->instruction_count == 0u) {
                        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED,
                                                  instruction->operation_id, function_id, block_id,
                                                  instruction_id);
                        return false;
                    }
                }
                if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_YIELD) {
                    ++suspension_count;
                    const XrBackendCoroutineSafepoint *point =
                        &function->coroutine_safepoints[instruction->immediate.u32];
                    if (instruction->successors[0] !=
                            function->coroutine_states[point->resume_state_id].continuation_block ||
                        instruction->operand_count < point->live_value_count ||
                        !safepoint_live_set_matches(function, instruction, point, 0u) ||
                        !cancel_continuation_matches(function, instruction, point, 0u)) {
                        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED,
                                                  instruction->operation_id, function_id, block_id,
                                                  instruction_id);
                        return false;
                    }
                }
                if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_SUSPEND) {
                    ++suspension_count;
                    uint32_t safepoint_id = instruction->immediate.coroutine_suspend.safepoint_id;
                    uint32_t request_count =
                        instruction->immediate.coroutine_suspend.request_operand_count;
                    const XrBackendCoroutineSafepoint *point =
                        &function->coroutine_safepoints[safepoint_id];
                    if (instruction->successors[0] !=
                            function->coroutine_states[point->resume_state_id].continuation_block ||
                        instruction->operand_count < request_count + point->live_value_count ||
                        !safepoint_live_set_matches(function, instruction, point, request_count) ||
                        !cancel_continuation_matches(function, instruction, point, request_count)) {
                        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED,
                                                  instruction->operation_id, function_id, block_id,
                                                  instruction_id);
                        return false;
                    }
                }
                if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED) {
                    ++suspension_count;
                    uint32_t callee_id = instruction->immediate.coroutine_call.function_id;
                    uint32_t safepoint_id = instruction->immediate.coroutine_call.safepoint_id;
                    const XrBackendFunction *callee = &ir->functions[callee_id];
                    const XrBackendCoroutineSafepoint *point =
                        &function->coroutine_safepoints[safepoint_id];
                    const XrBackendBlock *normal = &function->blocks[instruction->successors[0]];
                    uint32_t implicit_result =
                        callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
                    if (callee_id == function_id || callee->coroutine_safepoint_count == 0u ||
                        callee->coroutine_state_count != callee->coroutine_safepoint_count + 1u ||
                        callee->error_type_id != XR_CORE_TYPE_VOID ||
                        callee->panic_type_id != XR_CORE_TYPE_VOID ||
                        function->coroutine_states[point->resume_state_id].continuation_block !=
                            block_id ||
                        instruction->operand_count <
                            callee->parameter_count + point->live_value_count ||
                        normal->argument_count != implicit_result + point->live_value_count ||
                        !cancel_continuation_matches(function, instruction, point,
                                                     callee->parameter_count) ||
                        !coroutine_trap_continuation_matches(function, instruction, point,
                                                             callee->parameter_count)) {
                        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED,
                                                  instruction->operation_id, function_id, block_id,
                                                  instruction_id);
                        return false;
                    }
                    if (implicit_result != 0u &&
                        (normal->argument_types[0] != callee->result_type_id ||
                         normal->argument_categories[0] != XR_CORE_IR_VALUE ||
                         normal->argument_ownerships[0] != callee->result_ownership)) {
                        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED,
                                                  instruction->operation_id, function_id, block_id,
                                                  instruction_id);
                        return false;
                    }
                    for (uint32_t parameter = 0u; parameter < callee->parameter_count;
                         ++parameter) {
                        uint32_t value = instruction->operands[parameter];
                        XrParamMode mode = callee->parameter_modes[parameter];
                        XrCoreIrValueCategory category =
                            mode == XR_PARAM_REF ? XR_CORE_IR_PLACE : XR_CORE_IR_VALUE;
                        bool owned_read = mode == XR_PARAM_READ &&
                                          function->value_ownerships[value] == XR_CORE_IR_OWNER;
                        if ((mode != XR_PARAM_READ && mode != XR_PARAM_REF) ||
                            (mode == XR_PARAM_READ &&
                             !coroutine_read_argument_is_frame_stable(
                                 ir, function, instruction, point, block_id, parameter)) ||
                            (mode == XR_PARAM_REF &&
                             !coroutine_ref_argument_is_frame_stable(ir, function, instruction,
                                                                     point, parameter)) ||
                            function->value_types[value] != callee->parameter_types[parameter] ||
                            function->value_categories[value] != category ||
                            (function->value_ownerships[value] != XR_CORE_IR_NON_OWNER &&
                             !owned_read)) {
                            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED,
                                                      instruction->operation_id, function_id,
                                                      block_id, instruction_id);
                            return false;
                        }
                    }
                    for (uint32_t live = 0u; live < point->live_value_count; ++live) {
                        uint32_t operand = callee->parameter_count + live;
                        uint32_t target = implicit_result + live;
                        uint32_t value = instruction->operands[operand];
                        if (normal->argument_types[target] != function->value_types[value] ||
                            normal->argument_categories[target] !=
                                function->value_categories[value] ||
                            normal->argument_ownerships[target] !=
                                function->value_ownerships[value]) {
                            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED,
                                                      instruction->operation_id, function_id,
                                                      block_id, instruction_id);
                            return false;
                        }
                    }
                }
                if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT) {
                    ++suspension_count;
                    const XrValidatedSignature *signature =
                        callable_invoke_signature(ir, function, instruction);
                    uint32_t carrier = instruction->operand_count != 0u ? instruction->operands[0]
                                                                        : XR_PROGRAM_LOCATION_NONE;
                    uint32_t safepoint_id = instruction->immediate.u32;
                    const XrBackendCoroutineSafepoint *point =
                        &function->coroutine_safepoints[safepoint_id];
                    const XrBackendBlock *normal = &function->blocks[instruction->successors[0]];
                    uint32_t implicit_result =
                        signature && signature->result_type_id != XR_CORE_TYPE_VOID ? 1u : 0u;
                    uint32_t live_start = signature ? signature->parameter_count + 1u : UINT32_MAX;
                    uint32_t carrier_live_count = 0u;
                    for (uint32_t live = 0u;
                         carrier < function->value_count && live < point->live_value_count; ++live)
                        carrier_live_count += point->live_value_ids[live] == carrier;
                    bool owner_carrier = carrier < function->value_count &&
                                         function->value_ownerships[carrier] == XR_CORE_IR_OWNER;
                    bool stable_carrier =
                        carrier < function->value_count &&
                        ((owner_carrier && carrier_live_count == 1u) ||
                         (!owner_carrier &&
                          xr_validated_function_frame_stable_callable(
                              ir->program, &ir->program->functions[function_id], carrier)));
                    if (!signature || !stable_carrier ||
                        signature->error_type_id != XR_CORE_TYPE_VOID ||
                        signature->panic_type_id != XR_CORE_TYPE_VOID ||
                        (signature->effect_mask &
                         (XR_CORE_EFFECT_CANCEL | XR_CORE_EFFECT_SUSPEND)) !=
                            (XR_CORE_EFFECT_CANCEL | XR_CORE_EFFECT_SUSPEND) ||
                        (signature->capability_mask &
                         XR_CORE_CAPABILITY_RUNTIME_COROUTINE_SUSPENSION) == 0u ||
                        function->coroutine_states[point->resume_state_id].continuation_block !=
                            block_id ||
                        live_start > instruction->operand_count ||
                        point->live_value_count > instruction->operand_count - live_start ||
                        normal->argument_count != implicit_result + point->live_value_count ||
                        !cancel_continuation_matches(function, instruction, point, live_start) ||
                        !coroutine_trap_continuation_matches(function, instruction, point,
                                                             live_start)) {
                        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED,
                                                  instruction->operation_id, function_id, block_id,
                                                  instruction_id);
                        return false;
                    }
                    if (implicit_result != 0u &&
                        (normal->argument_types[0] != signature->result_type_id ||
                         normal->argument_categories[0] != XR_CORE_IR_VALUE ||
                         normal->argument_ownerships[0] != signature->result_ownership)) {
                        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED,
                                                  instruction->operation_id, function_id, block_id,
                                                  instruction_id);
                        return false;
                    }
                    for (uint32_t parameter = 0u; parameter < signature->parameter_count;
                         ++parameter) {
                        uint32_t value = instruction->operands[parameter + 1u];
                        if (signature->parameter_modes[parameter] != XR_PARAM_READ ||
                            function->value_types[value] != signature->parameter_types[parameter] ||
                            function->value_categories[value] != XR_CORE_IR_VALUE ||
                            function->value_ownerships[value] != XR_CORE_IR_NON_OWNER) {
                            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED,
                                                      instruction->operation_id, function_id,
                                                      block_id, instruction_id);
                            return false;
                        }
                    }
                    for (uint32_t live = 0u; live < point->live_value_count; ++live) {
                        uint32_t value = instruction->operands[live_start + live];
                        uint32_t target = implicit_result + live;
                        if (normal->argument_types[target] != function->value_types[value] ||
                            normal->argument_categories[target] !=
                                function->value_categories[value] ||
                            normal->argument_ownerships[target] !=
                                function->value_ownerships[value]) {
                            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED,
                                                      instruction->operation_id, function_id,
                                                      block_id, instruction_id);
                            return false;
                        }
                    }
                }
            }
        }
        if (!cleanup_reason_flow_valid(ir, function_id, diagnostic_out))
            return false;
        if (!backend_owner_flow_valid(ir, function_id, diagnostic_out))
            return false;
        if (suspension_count != function->coroutine_safepoint_count) {
            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED,
                                      XR_CORE_OP_CORE_COROUTINE_CALL_SEALED, function_id, 0u, 0u);
            return false;
        }
        const XrBackendBlock *entry = &function->blocks[function->entry_block];
        if (entry->argument_count != function->parameter_count) {
            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u,
                                      function_id, function->entry_block, 0u);
            return false;
        }
        for (uint32_t parameter = 0; parameter < function->parameter_count; ++parameter) {
            XrCoreIrValueCategory expected =
                function->parameter_modes[parameter] == XR_PARAM_REF &&
                        !backend_parameter_is_class_receiver(ir, function, parameter)
                    ? XR_CORE_IR_PLACE
                    : XR_CORE_IR_VALUE;
            if (entry->argument_types[parameter] != function->parameter_types[parameter] ||
                entry->argument_categories[parameter] != expected) {
                xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u,
                                          function_id, function->entry_block, 0u);
                return false;
            }
        }
    }
    XrFingerprint expected_lowering_digest;
    xr_backend_compute_lowering_digest(ir, &expected_lowering_digest);
    if (memcmp(ir->lowering_digest.bytes, expected_lowering_digest.bytes,
               sizeof(ir->lowering_digest.bytes)) != 0) {
        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u, 0u, 0u, 0u);
        return false;
    }
    return true;
}

static bool immediate_equal(const XrValidatedInstruction *source,
                            const XrBackendInstruction *lowered) {
    if (source->immediate_kind != lowered->immediate_kind)
        return false;
    switch (source->immediate_kind) {
        case XR_CORE_IR_IMMEDIATE_NONE:
            return true;
        case XR_CORE_IR_IMMEDIATE_I64:
            return source->immediate.i64 == lowered->immediate.i64;
        case XR_CORE_IR_IMMEDIATE_U32:
            return source->immediate.u32 == lowered->immediate.u32;
        case XR_CORE_IR_IMMEDIATE_BOOL:
            return source->immediate.boolean == lowered->immediate.boolean;
        case XR_CORE_IR_IMMEDIATE_CONSTANT:
            return source->immediate.constant_id == lowered->immediate.constant_id;
        case XR_CORE_IR_IMMEDIATE_FUNCTION:
            return source->immediate.function_id == lowered->immediate.function_id;
        case XR_CORE_IR_IMMEDIATE_FIELD:
            return source->immediate.field_ordinal == lowered->immediate.field_ordinal;
        case XR_CORE_IR_IMMEDIATE_VARIANT:
            return source->immediate.variant_ordinal == lowered->immediate.variant_ordinal;
        case XR_CORE_IR_IMMEDIATE_VARIANT_FIELD:
            return source->immediate.variant_field.variant_ordinal ==
                       lowered->immediate.variant_field.variant_ordinal &&
                   source->immediate.variant_field.field_ordinal ==
                       lowered->immediate.variant_field.field_ordinal;
        case XR_CORE_IR_IMMEDIATE_TYPE:
            return source->immediate.type_id == lowered->immediate.type_id;
        case XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION:
            return source->immediate.provider_operation.requirement_index ==
                       lowered->immediate.provider_operation.requirement_index &&
                   source->immediate.provider_operation.operation_index ==
                       lowered->immediate.provider_operation.operation_index;
        case XR_CORE_IR_IMMEDIATE_COROUTINE_CALL:
            return source->immediate.coroutine_call.function_id ==
                       lowered->immediate.coroutine_call.function_id &&
                   source->immediate.coroutine_call.safepoint_id ==
                       lowered->immediate.coroutine_call.safepoint_id;
        case XR_CORE_IR_IMMEDIATE_COROUTINE_SUSPEND:
            return source->immediate.coroutine_suspend.safepoint_id ==
                       lowered->immediate.coroutine_suspend.safepoint_id &&
                   source->immediate.coroutine_suspend.request_kind ==
                       lowered->immediate.coroutine_suspend.request_kind &&
                   source->immediate.coroutine_suspend.request_operand_count ==
                       lowered->immediate.coroutine_suspend.request_operand_count;
    }
    return false;
}

bool xr_backend_ir_translation_validate(const XrBackendIR *ir,
                                        XrBackendDiagnostic *diagnostic_out) {
    xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_OK, 0u, 0u, 0u, 0u);
    if (!ir || !ir->program || ir->function_count != ir->program->function_count ||
        ir->entry_function != ir->program->entry_function ||
        ir->constant_count != ir->program->constant_count ||
        (ir->constant_count != 0u &&
         memcmp(ir->constants, ir->program->constants,
                (size_t) ir->constant_count * sizeof(*ir->constants)) != 0)) {
        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_TRANSLATION_REJECTED, 0u, 0u, 0u, 0u);
        return false;
    }
    for (uint32_t function_id = 0; function_id < ir->function_count; ++function_id) {
        const XrValidatedFunction *source = &ir->program->functions[function_id];
        const XrBackendFunction *lowered = &ir->functions[function_id];
        if (source->parameter_count != lowered->parameter_count ||
            source->result_type_id != lowered->result_type_id ||
            source->error_type_id != lowered->error_type_id ||
            source->panic_type_id != lowered->panic_type_id ||
            source->result_ownership != lowered->result_ownership ||
            source->effect_mask != lowered->effect_mask ||
            source->capability_mask != lowered->capability_mask ||
            source->entry_block != lowered->entry_block ||
            source->coroutine_state_count != lowered->coroutine_state_count ||
            source->coroutine_safepoint_count != lowered->coroutine_safepoint_count ||
            source->block_count != lowered->block_count ||
            source->value_count != lowered->value_count || source->flags != lowered->flags ||
            !array_u16_equal(source->parameter_types, lowered->parameter_types,
                             source->parameter_count) ||
            !array_mode_equal(source->parameter_modes, lowered->parameter_modes,
                              source->parameter_count) ||
            !array_u16_equal(source->value_types, lowered->value_types, source->value_count) ||
            !array_category_equal(source->value_categories, lowered->value_categories,
                                  source->value_count) ||
            !array_ownership_equal(source->value_ownerships, lowered->value_ownerships,
                                   source->value_count)) {
            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_TRANSLATION_REJECTED, 0u,
                                      function_id, 0u, 0u);
            return false;
        }
        for (uint32_t state = 0; state < source->coroutine_state_count; ++state) {
            if (source->coroutine_states[state].continuation_block !=
                lowered->coroutine_states[state].continuation_block) {
                xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_TRANSLATION_REJECTED, 0u,
                                          function_id, 0u, 0u);
                return false;
            }
        }
        for (uint32_t safepoint = 0; safepoint < source->coroutine_safepoint_count; ++safepoint) {
            const XrValidatedCoroutineSafepoint *source_point =
                &source->coroutine_safepoints[safepoint];
            const XrBackendCoroutineSafepoint *lowered_point =
                &lowered->coroutine_safepoints[safepoint];
            if (source_point->resume_state_id != lowered_point->resume_state_id ||
                source_point->live_value_count != lowered_point->live_value_count ||
                !array_u32_equal(source_point->live_value_ids, lowered_point->live_value_ids,
                                 source_point->live_value_count)) {
                xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_TRANSLATION_REJECTED, 0u,
                                          function_id, 0u, 0u);
                return false;
            }
        }
        for (uint32_t block_id = 0; block_id < source->block_count; ++block_id) {
            const XrValidatedBlock *source_block = &source->blocks[block_id];
            const XrBackendBlock *lowered_block = &lowered->blocks[block_id];
            if (source_block->argument_count != lowered_block->argument_count ||
                source_block->instruction_count != lowered_block->instruction_count ||
                !array_u32_equal(source_block->argument_ids, lowered_block->argument_ids,
                                 source_block->argument_count) ||
                !array_u16_equal(source_block->argument_types, lowered_block->argument_types,
                                 source_block->argument_count) ||
                !array_category_equal(source_block->argument_categories,
                                      lowered_block->argument_categories,
                                      source_block->argument_count) ||
                !array_ownership_equal(source_block->argument_ownerships,
                                       lowered_block->argument_ownerships,
                                       source_block->argument_count)) {
                xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_TRANSLATION_REJECTED, 0u,
                                          function_id, block_id, 0u);
                return false;
            }
            for (uint32_t instruction_id = 0; instruction_id < source_block->instruction_count;
                 ++instruction_id) {
                const XrValidatedInstruction *source_instruction =
                    &source_block->instructions[instruction_id];
                const XrBackendInstruction *lowered_instruction =
                    &lowered_block->instructions[instruction_id];
                if (source_instruction->operation_id != lowered_instruction->operation_id ||
                    source_instruction->result_id != lowered_instruction->result_id ||
                    source_instruction->result_type_id != lowered_instruction->result_type_id ||
                    source_instruction->result_category != lowered_instruction->result_category ||
                    source_instruction->result_ownership != lowered_instruction->result_ownership ||
                    source_instruction->operand_count != lowered_instruction->operand_count ||
                    source_instruction->successor_count != lowered_instruction->successor_count ||
                    !array_u32_equal(source_instruction->operands, lowered_instruction->operands,
                                     source_instruction->operand_count) ||
                    !array_u32_equal(source_instruction->successors,
                                     lowered_instruction->successors,
                                     source_instruction->successor_count) ||
                    !immediate_equal(source_instruction, lowered_instruction)) {
                    xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_TRANSLATION_REJECTED,
                                              source_instruction->operation_id, function_id,
                                              block_id, instruction_id);
                    return false;
                }
            }
        }
    }
    return true;
}
