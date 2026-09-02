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
        (instruction->result_id == XR_PROGRAM_LOCATION_NONE &&
         instruction->result_category != XR_CORE_IR_VALUE) ||
        (instruction->result_id != XR_PROGRAM_LOCATION_NONE &&
         instruction->result_category != function->value_categories[instruction->result_id]))
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
        case XR_CORE_OP_CORE_ADD_I64:
        case XR_CORE_OP_CORE_SUB_I64:
        case XR_CORE_OP_CORE_MUL_I64:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_U32 &&
                   instruction->immediate.u32 <= 1u;
        case XR_CORE_OP_CORE_DIV_I64:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_U32 &&
                   instruction->immediate.u32 == 0u;
        case XR_CORE_OP_CORE_COMPARE_I64:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_U32 &&
                   instruction->immediate.u32 <= 5u;
        case XR_CORE_OP_CORE_BLOCK_ARGUMENT:
        case XR_CORE_OP_CORE_BRANCH:
        case XR_CORE_OP_CORE_CONDITIONAL_BRANCH:
        case XR_CORE_OP_CORE_RETURN:
        case XR_CORE_OP_CORE_ERROR_PUBLISH:
        case XR_CORE_OP_CORE_TARGET_POINTER_WIDTH:
        case XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_NONE;
        case XR_CORE_OP_CORE_TRAP:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_U32 &&
                   instruction->immediate.u32 == 4u;
        case XR_CORE_OP_CORE_CALL_SEALED_DIRECT:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_FUNCTION &&
                   instruction->immediate.function_id < ir->function_count;
        case XR_CORE_OP_CORE_AGGREGATE_PROJECT:
        case XR_CORE_OP_CORE_AGGREGATE_UPDATE:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_FIELD;
        case XR_CORE_OP_CORE_VARIANT_CONSTRUCT:
        case XR_CORE_OP_CORE_VARIANT_TEST:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_VARIANT;
        case XR_CORE_OP_CORE_VARIANT_PROJECT:
            return instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_VARIANT_FIELD;
        default:
            return false;
    }
}

bool xr_backend_ir_verify(const XrBackendIR *ir, XrBackendDiagnostic *diagnostic_out) {
    xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_OK, 0u, 0u, 0u, 0u);
    if (!ir || !ir->program || !ir->profile || !ir->functions || ir->function_count == 0u ||
        (ir->constant_count != 0u && !ir->constants) || ir->entry_function >= ir->function_count ||
        (ir->pointer_width != 32u && ir->pointer_width != 64u) ||
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
    uint32_t pointer_width = machine ? machine->data_layout.pointer.size * 8u : 0u;
    XrBackendId expected_backend_id = xr_backend_compute_id();
    XrOptimizationPolicyId expected_optimization_policy_id =
        xr_backend_compute_optimization_policy_id(&ir->options);
    if (ir->pointer_width != pointer_width ||
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
              !function->value_representations))) {
            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVARIANT_REJECTED, 0u,
                                      function_id, 0u, 0u);
            return false;
        }
        for (uint32_t value = 0; value < function->value_count; ++value) {
            uint8_t expected = 0u;
            if (!xr_backend_representation_for_type(function->value_types[value], &expected) ||
                function->value_categories[value] > XR_CORE_IR_PLACE ||
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
                 (!block->argument_ids || !block->argument_types || !block->argument_categories)) ||
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
                        function->value_categories[block->argument_ids[argument]]) {
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
            }
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
            source->effect_mask != lowered->effect_mask ||
            source->capability_mask != lowered->capability_mask ||
            source->entry_block != lowered->entry_block ||
            source->block_count != lowered->block_count ||
            source->value_count != lowered->value_count || source->flags != lowered->flags ||
            !array_u16_equal(source->parameter_types, lowered->parameter_types,
                             source->parameter_count) ||
            !array_mode_equal(source->parameter_modes, lowered->parameter_modes,
                              source->parameter_count) ||
            !array_u16_equal(source->value_types, lowered->value_types, source->value_count) ||
            !array_category_equal(source->value_categories, lowered->value_categories,
                                  source->value_count)) {
            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_TRANSLATION_REJECTED, 0u,
                                      function_id, 0u, 0u);
            return false;
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
