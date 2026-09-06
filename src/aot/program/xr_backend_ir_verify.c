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

#include "../../core/xr_core_spec_gen.h"
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
                   function->value_types[instruction->operands[0]] <=
                       XR_CORE_TYPE_TARGET_ENDIAN &&
                   function->value_types[instruction->operands[0]] ==
                       function->value_types[instruction->operands[1]];
        case XR_CORE_OP_CORE_COROUTINE_YIELD:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_U32 &&
                   instruction->immediate.u32 < function->coroutine_safepoint_count &&
                   instruction->successor_count == 1u;
        case XR_CORE_OP_CORE_COROUTINE_CALL_SEALED:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_COROUTINE_CALL &&
                   instruction->immediate.coroutine_call.function_id < ir->function_count &&
                   instruction->immediate.coroutine_call.safepoint_id <
                       function->coroutine_safepoint_count &&
                   instruction->successor_count == 1u;
        case XR_CORE_OP_CORE_BLOCK_ARGUMENT:
        case XR_CORE_OP_CORE_BRANCH:
        case XR_CORE_OP_CORE_CONDITIONAL_BRANCH:
        case XR_CORE_OP_CORE_RETURN:
        case XR_CORE_OP_CORE_ERROR_PUBLISH:
        case XR_CORE_OP_CORE_PANIC_PUBLISH:
        case XR_CORE_OP_CORE_TARGET_POINTER_WIDTH:
        case XR_CORE_OP_CORE_TARGET_OPERATING_SYSTEM:
        case XR_CORE_OP_CORE_TARGET_ARCHITECTURE:
        case XR_CORE_OP_CORE_TARGET_NATIVE_ABI:
        case XR_CORE_OP_CORE_TARGET_ENDIANNESS:
        case XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT:
        case XR_CORE_OP_CORE_EXISTENTIAL_PACK:
        case XR_CORE_OP_CORE_OWNER_COPY:
        case XR_CORE_OP_CORE_OWNER_MOVE:
        case XR_CORE_OP_CORE_OWNER_DROP:
        case XR_CORE_OP_CORE_PLACE_LOCAL:
        case XR_CORE_OP_CORE_PLACE_LOAD:
        case XR_CORE_OP_CORE_PLACE_STORE:
        case XR_CORE_OP_CORE_PLACE_TAKE:
        case XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT:
        case XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_NONE &&
                   (instruction->operation_id != XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT ||
                    instruction->successor_count <= 1u);
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
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_FUNCTION &&
                   (instruction->operation_id != XR_CORE_OP_CORE_CALL_SEALED_DIRECT ||
                    instruction->successor_count <= 1u) &&
                   instruction->immediate.function_id < ir->function_count;
        case XR_CORE_OP_CORE_CALL_WITNESS_DIRECT:
        case XR_CORE_OP_CORE_CALL_WITNESS_INVOKE:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_U32 &&
                   (instruction->operation_id != XR_CORE_OP_CORE_CALL_WITNESS_DIRECT ||
                    instruction->successor_count <= 1u);
        case XR_CORE_OP_CORE_AGGREGATE_PROJECT:
        case XR_CORE_OP_CORE_AGGREGATE_UPDATE:
        case XR_CORE_OP_CORE_PLACE_PROJECT:
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
        ir->operating_system <= XR_TARGET_OS_NONE ||
        ir->operating_system >= XR_TARGET_OS_COUNT ||
        ir->architecture <= XR_TARGET_ARCH_NONE || ir->architecture >= XR_TARGET_ARCH_COUNT ||
        ir->native_abi <= XR_TARGET_ABI_NONE || ir->native_abi >= XR_TARGET_ABI_COUNT ||
        (ir->endianness != XR_TARGET_ENDIAN_LITTLE &&
         ir->endianness != XR_TARGET_ENDIAN_BIG) ||
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
             (function->coroutine_state_count != 2u || function->coroutine_safepoint_count != 1u ||
              !function->coroutine_states || !function->coroutine_safepoints ||
              function->coroutine_states[0].continuation_block != function->entry_block ||
              function->coroutine_states[1].continuation_block >= function->block_count ||
              function->coroutine_safepoints[0].resume_state_id != 1u ||
              (function->coroutine_safepoints[0].live_value_count != 0u &&
               !function->coroutine_safepoints[0].live_value_ids))) ||
            (!coroutine && (function->coroutine_states || function->coroutine_safepoints))) {
            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u,
                                      function_id, 0u, 0u);
            return false;
        }
        uint32_t suspension_count = 0u;
        if (coroutine) {
            const XrBackendCoroutineSafepoint *point = &function->coroutine_safepoints[0];
            if ((function->effect_mask & XR_CORE_EFFECT_SUSPEND) == 0u ||
                (function->capability_mask & XR_CORE_CAPABILITY_RUNTIME_COOPERATIVE_YIELD) == 0u ||
                point->resume_state_id >= function->coroutine_state_count) {
                xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u,
                                          function_id, 0u, 0u);
                return false;
            }
            for (uint32_t live = 0; live < point->live_value_count; ++live) {
                if (point->live_value_ids[live] >= function->value_count ||
                    function->value_categories[point->live_value_ids[live]] != XR_CORE_IR_VALUE ||
                    function->value_ownerships[point->live_value_ids[live]] !=
                        XR_CORE_IR_NON_OWNER) {
                    xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u,
                                              function_id, 0u, 0u);
                    return false;
                }
            }
        }
        for (uint32_t value = 0; value < function->value_count; ++value) {
            uint8_t expected = 0u;
            if (!xr_backend_representation_for_type(function->value_types[value], &expected) ||
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
                if (!instruction_shape_valid(ir, function, instruction)) {
                    xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED,
                                              instruction->operation_id, function_id, block_id,
                                              instruction_id);
                    return false;
                }
                if ((instruction->operation_id == XR_CORE_OP_CORE_PROVIDER_CALL ||
                     instruction->operation_id == XR_CORE_OP_CORE_CALL_SEALED_DIRECT ||
                     instruction->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT ||
                     instruction->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_DIRECT) &&
                    instruction->successor_count == 1u) {
                    const XrBackendBlock *target =
                        &function->blocks[instruction->successors[0]];
                    if (target->argument_count > instruction->operand_count ||
                        target->instruction_count == 0u) {
                        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED,
                                                  instruction->operation_id, function_id, block_id,
                                                  instruction_id);
                        return false;
                    }
                    const XrBackendInstruction *trap =
                        &target->instructions[target->instruction_count - 1u];
                    if (trap->operation_id != XR_CORE_OP_CORE_TRAP ||
                        trap->immediate_kind != XR_CORE_IR_IMMEDIATE_U32 ||
                        trap->immediate.u32 != 7u) {
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
                        instruction->operand_count != point->live_value_count ||
                        !array_u32_equal(instruction->operands, point->live_value_ids,
                                         point->live_value_count)) {
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
                    const XrBackendBlock *normal =
                        &function->blocks[instruction->successors[0]];
                    uint32_t implicit_result =
                        callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
                    if (callee_id == function_id || callee->coroutine_state_count != 2u ||
                        callee->coroutine_safepoint_count != 1u ||
                        callee->error_type_id != XR_CORE_TYPE_VOID ||
                        callee->panic_type_id != XR_CORE_TYPE_VOID ||
                        function->coroutine_states[point->resume_state_id].continuation_block !=
                            block_id ||
                        instruction->operand_count !=
                            callee->parameter_count + point->live_value_count ||
                        normal->argument_count != implicit_result + point->live_value_count) {
                        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED,
                                                  instruction->operation_id, function_id, block_id,
                                                  instruction_id);
                        return false;
                    }
                    if (implicit_result != 0u &&
                        (normal->argument_types[0] != callee->result_type_id ||
                         normal->argument_categories[0] != XR_CORE_IR_VALUE ||
                         normal->argument_ownerships[0] != XR_CORE_IR_NON_OWNER)) {
                        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED,
                                                  instruction->operation_id, function_id, block_id,
                                                  instruction_id);
                        return false;
                    }
                    for (uint32_t parameter = 0u; parameter < callee->parameter_count;
                         ++parameter) {
                        uint32_t value = instruction->operands[parameter];
                        if (callee->parameter_modes[parameter] != XR_PARAM_READ ||
                            function->value_types[value] != callee->parameter_types[parameter] ||
                            function->value_categories[value] != XR_CORE_IR_VALUE ||
                            function->value_ownerships[value] != XR_CORE_IR_NON_OWNER) {
                            xr_backend_set_diagnostic(
                                diagnostic_out, XR_BACKEND_INVARIANT_REJECTED,
                                instruction->operation_id, function_id, block_id, instruction_id);
                            return false;
                        }
                    }
                    for (uint32_t live = 0u; live < point->live_value_count; ++live) {
                        uint32_t operand = callee->parameter_count + live;
                        uint32_t target = implicit_result + live;
                        uint32_t value = point->live_value_ids[live];
                        if (instruction->operands[operand] != value ||
                            normal->argument_types[target] != function->value_types[value] ||
                            normal->argument_categories[target] != XR_CORE_IR_VALUE ||
                            normal->argument_ownerships[target] != XR_CORE_IR_NON_OWNER) {
                            xr_backend_set_diagnostic(
                                diagnostic_out, XR_BACKEND_INVARIANT_REJECTED,
                                instruction->operation_id, function_id, block_id, instruction_id);
                            return false;
                        }
                    }
                }
            }
        }
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
            XrCoreIrValueCategory expected = function->parameter_modes[parameter] == XR_PARAM_REF
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
