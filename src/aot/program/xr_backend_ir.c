/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_backend_ir.c - Validated-program to private AOT realization lowering
 */

#include "xr_backend_ir_internal.h"

#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include "../../core/xr_core_spec_gen.h"

#include <string.h>

static bool operation_is_supported(uint16_t operation_id) {
    switch (operation_id) {
        case XR_CORE_OP_CORE_CONSTANT_I64:
        case XR_CORE_OP_CORE_CONSTANT_BOOL:
        case XR_CORE_OP_CORE_ADD_I64:
        case XR_CORE_OP_CORE_SUB_I64:
        case XR_CORE_OP_CORE_MUL_I64:
        case XR_CORE_OP_CORE_DIV_I64:
        case XR_CORE_OP_CORE_COMPARE_I64:
        case XR_CORE_OP_CORE_BLOCK_ARGUMENT:
        case XR_CORE_OP_CORE_BRANCH:
        case XR_CORE_OP_CORE_CONDITIONAL_BRANCH:
        case XR_CORE_OP_CORE_RETURN:
        case XR_CORE_OP_CORE_CALL_SEALED_DIRECT:
        case XR_CORE_OP_CORE_CALL_SEALED_INVOKE:
        case XR_CORE_OP_CORE_CALL_WITNESS_DIRECT:
        case XR_CORE_OP_CORE_CALL_WITNESS_INVOKE:
        case XR_CORE_OP_CORE_TRAP:
        case XR_CORE_OP_CORE_ERROR_PUBLISH:
        case XR_CORE_OP_CORE_PANIC_PUBLISH:
        case XR_CORE_OP_CORE_TARGET_POINTER_WIDTH:
        case XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT:
        case XR_CORE_OP_CORE_AGGREGATE_PROJECT:
        case XR_CORE_OP_CORE_AGGREGATE_UPDATE:
        case XR_CORE_OP_CORE_VARIANT_CONSTRUCT:
        case XR_CORE_OP_CORE_VARIANT_TEST:
        case XR_CORE_OP_CORE_VARIANT_PROJECT:
        case XR_CORE_OP_CORE_EXISTENTIAL_PACK:
        case XR_CORE_OP_CORE_EXISTENTIAL_TEST:
        case XR_CORE_OP_CORE_EXISTENTIAL_PROJECT:
        case XR_CORE_OP_CORE_OWNER_COPY:
        case XR_CORE_OP_CORE_OWNER_MOVE:
        case XR_CORE_OP_CORE_OWNER_DROP:
        case XR_CORE_OP_CORE_PLACE_LOCAL:
        case XR_CORE_OP_CORE_PLACE_LOAD:
        case XR_CORE_OP_CORE_PLACE_STORE:
            return true;
        default:
            return false;
    }
}

static void hash_u16(XrSHA256Context *context, uint16_t value) {
    uint8_t bytes[2];
    bytes[0] = (uint8_t) value;
    bytes[1] = (uint8_t) (value >> 8u);
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_u32(XrSHA256Context *context, uint32_t value) {
    uint8_t bytes[4];
    for (size_t index = 0; index < sizeof(bytes); ++index)
        bytes[index] = (uint8_t) (value >> (index * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_u64(XrSHA256Context *context, uint64_t value) {
    uint8_t bytes[8];
    for (size_t index = 0; index < sizeof(bytes); ++index)
        bytes[index] = (uint8_t) (value >> (index * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static XrFingerprint hash_text(const char *domain, const void *bytes, size_t size) {
    XrSHA256Context context;
    XrFingerprint result;
    xr_sha256_init(&context);
    xr_sha256_update(&context, (const uint8_t *) domain, strlen(domain));
    if (bytes && size != 0u)
        xr_sha256_update(&context, bytes, size);
    xr_sha256_final(&context, result.bytes);
    return result;
}

XrBackendId xr_backend_compute_id(void) {
    const uint8_t backend_version[] = {1u, 0u, 0u, 0u};
    return hash_text(XR_AOT_BACKEND_NAME, backend_version, sizeof(backend_version));
}

XrOptimizationPolicyId xr_backend_compute_optimization_policy_id(const XrBackendOptions *options) {
    uint8_t bytes[8] = {0};
    bytes[0] = (uint8_t) options->schema_version;
    bytes[1] = (uint8_t) (options->schema_version >> 8u);
    bytes[2] = (uint8_t) (options->schema_version >> 16u);
    bytes[3] = (uint8_t) (options->schema_version >> 24u);
    bytes[4] = options->optimization_policy;
    return hash_text("xray:aot:optimization-policy:v1", bytes, sizeof(bytes));
}

static void free_instruction(XrBackendInstruction *instruction) {
    xr_free(instruction->successors);
    xr_free(instruction->operands);
    memset(instruction, 0, sizeof(*instruction));
}

static void free_function(XrBackendFunction *function) {
    if (!function)
        return;
    for (uint32_t block = 0; block < function->block_count; ++block) {
        XrBackendBlock *row = &function->blocks[block];
        for (uint32_t instruction = 0; instruction < row->instruction_count; ++instruction)
            free_instruction(&row->instructions[instruction]);
        xr_free(row->instructions);
        xr_free(row->argument_types);
        xr_free(row->argument_categories);
        xr_free(row->argument_ownerships);
        xr_free(row->argument_ids);
    }
    xr_free(function->value_representations);
    xr_free(function->value_types);
    xr_free(function->value_categories);
    xr_free(function->value_ownerships);
    xr_free(function->blocks);
    xr_free(function->parameter_types);
    xr_free(function->parameter_modes);
    memset(function, 0, sizeof(*function));
}

void xr_backend_set_diagnostic(XrBackendDiagnostic *diagnostic, XrBackendStatus status,
                               uint16_t operation_id, uint32_t function_id, uint32_t block_id,
                               uint32_t instruction_id) {
    if (!diagnostic)
        return;
    *diagnostic = (XrBackendDiagnostic) {.status = status,
                                         .operation_id = operation_id,
                                         .function_id = function_id,
                                         .block_id = block_id,
                                         .instruction_id = instruction_id};
}

bool xr_backend_representation_for_type(uint16_t type_id, uint8_t *representation_out) {
    uint8_t representation = XR_BACKEND_VALUE_VOID;
    switch (type_id) {
        case XR_CORE_TYPE_VOID:
            representation = XR_BACKEND_VALUE_VOID;
            break;
        case XR_CORE_TYPE_BOOL:
            representation = XR_BACKEND_VALUE_BOOL_U8;
            break;
        case XR_CORE_TYPE_I64:
            representation = XR_BACKEND_VALUE_I64;
            break;
        case XR_CORE_TYPE_U32:
            representation = XR_BACKEND_VALUE_U32;
            break;
        case XR_CORE_TYPE_ERROR:
            representation = XR_BACKEND_VALUE_ERROR_U32;
            break;
        case XR_CORE_TYPE_PANIC_INFO:
            representation = XR_BACKEND_VALUE_PANIC_U32;
            break;
        default:
            if (type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE)
                return false;
            representation = XR_BACKEND_VALUE_AGGREGATE;
            break;
    }
    if (representation_out)
        *representation_out = representation;
    return true;
}

XrBackendOptions xr_backend_default_options(void) {
    return (XrBackendOptions) {.schema_version = XR_BACKEND_IR_SCHEMA_VERSION,
                               .optimization_policy = XR_BACKEND_OPTIMIZATION_PORTABLE,
                               .max_functions = UINT32_C(65536),
                               .max_blocks = UINT32_C(1000000),
                               .max_instructions = UINT32_C(10000000),
                               .max_values = UINT32_C(10000000)};
}

static bool options_valid(const XrBackendOptions *options) {
    return options && options->schema_version == XR_BACKEND_IR_SCHEMA_VERSION &&
           (options->optimization_policy == XR_BACKEND_OPTIMIZATION_NONE ||
            options->optimization_policy == XR_BACKEND_OPTIMIZATION_PORTABLE) &&
           options->max_functions != 0u && options->max_blocks != 0u &&
           options->max_instructions != 0u && options->max_values != 0u;
}

static bool copy_u32_array(const uint32_t *source, uint32_t count, uint32_t **destination) {
    *destination = NULL;
    if (count == 0u)
        return true;
    if ((size_t) count > SIZE_MAX / sizeof(uint32_t))
        return false;
    uint32_t *copy = xr_malloc((size_t) count * sizeof(*copy));
    if (!copy)
        return false;
    memcpy(copy, source, (size_t) count * sizeof(*copy));
    *destination = copy;
    return true;
}

static bool copy_u16_array(const uint16_t *source, uint32_t count, uint16_t **destination) {
    *destination = NULL;
    if (count == 0u)
        return true;
    if ((size_t) count > SIZE_MAX / sizeof(uint16_t))
        return false;
    uint16_t *copy = xr_malloc((size_t) count * sizeof(*copy));
    if (!copy)
        return false;
    memcpy(copy, source, (size_t) count * sizeof(*copy));
    *destination = copy;
    return true;
}

static bool copy_mode_array(const XrParamMode *source, uint32_t count, XrParamMode **destination) {
    *destination = NULL;
    if (count == 0u)
        return true;
    XrParamMode *copy = xr_malloc((size_t) count * sizeof(*copy));
    if (!copy)
        return false;
    memcpy(copy, source, (size_t) count * sizeof(*copy));
    *destination = copy;
    return true;
}

static bool copy_category_array(const XrCoreIrValueCategory *source, uint32_t count,
                                XrCoreIrValueCategory **destination) {
    *destination = NULL;
    if (count == 0u)
        return true;
    XrCoreIrValueCategory *copy = xr_malloc((size_t) count * sizeof(*copy));
    if (!copy)
        return false;
    memcpy(copy, source, (size_t) count * sizeof(*copy));
    *destination = copy;
    return true;
}

static bool copy_ownership_array(const XrCoreIrOwnershipDisposition *source, uint32_t count,
                                 XrCoreIrOwnershipDisposition **destination) {
    *destination = NULL;
    if (count == 0u)
        return true;
    XrCoreIrOwnershipDisposition *copy = xr_malloc((size_t) count * sizeof(*copy));
    if (!copy)
        return false;
    memcpy(copy, source, (size_t) count * sizeof(*copy));
    *destination = copy;
    return true;
}

static bool lower_instruction(const XrValidatedInstruction *source,
                              XrBackendInstruction *destination) {
    destination->operation_id = source->operation_id;
    destination->result_type_id = source->result_type_id;
    destination->result_category = source->result_category;
    destination->result_ownership = source->result_ownership;
    destination->result_id = source->result_id;
    destination->operand_count = source->operand_count;
    destination->immediate_kind = source->immediate_kind;
    memcpy(&destination->immediate, &source->immediate, sizeof(destination->immediate));
    destination->successor_count = source->successor_count;
    return copy_u32_array(source->operands, source->operand_count, &destination->operands) &&
           copy_u32_array(source->successors, source->successor_count, &destination->successors);
}

static bool lower_block(const XrValidatedBlock *source, XrBackendBlock *destination) {
    destination->argument_count = source->argument_count;
    destination->instruction_count = source->instruction_count;
    if (!copy_u32_array(source->argument_ids, source->argument_count, &destination->argument_ids) ||
        !copy_u16_array(source->argument_types, source->argument_count,
                        &destination->argument_types) ||
        !copy_category_array(source->argument_categories, source->argument_count,
                             &destination->argument_categories) ||
        !copy_ownership_array(source->argument_ownerships, source->argument_count,
                              &destination->argument_ownerships))
        return false;
    if (source->instruction_count == 0u)
        return true;
    if ((size_t) source->instruction_count > SIZE_MAX / sizeof(*destination->instructions))
        return false;
    destination->instructions =
        xr_calloc(source->instruction_count, sizeof(*destination->instructions));
    if (!destination->instructions)
        return false;
    for (uint32_t index = 0; index < source->instruction_count; ++index) {
        if (!lower_instruction(&source->instructions[index], &destination->instructions[index]))
            return false;
    }
    return true;
}

static bool lower_function(const XrValidatedFunction *source, XrBackendFunction *destination) {
    destination->parameter_count = source->parameter_count;
    destination->result_type_id = source->result_type_id;
    destination->result_ownership = source->result_ownership;
    destination->error_type_id = source->error_type_id;
    destination->panic_type_id = source->panic_type_id;
    destination->effect_mask = source->effect_mask;
    destination->capability_mask = source->capability_mask;
    destination->entry_block = source->entry_block;
    destination->block_count = source->block_count;
    destination->value_count = source->value_count;
    destination->flags = source->flags;
    if (!copy_u16_array(source->parameter_types, source->parameter_count,
                        &destination->parameter_types) ||
        !copy_mode_array(source->parameter_modes, source->parameter_count,
                         &destination->parameter_modes) ||
        !copy_u16_array(source->value_types, source->value_count, &destination->value_types) ||
        !copy_category_array(source->value_categories, source->value_count,
                             &destination->value_categories) ||
        !copy_ownership_array(source->value_ownerships, source->value_count,
                              &destination->value_ownerships))
        return false;
    if (source->value_count != 0u) {
        if ((size_t) source->value_count > SIZE_MAX / sizeof(*destination->value_representations))
            return false;
        destination->value_representations =
            xr_calloc(source->value_count, sizeof(*destination->value_representations));
        if (!destination->value_representations)
            return false;
        for (uint32_t value = 0; value < source->value_count; ++value) {
            if (!xr_backend_representation_for_type(source->value_types[value],
                                                    &destination->value_representations[value]))
                return false;
        }
    }
    if (source->block_count == 0u)
        return false;
    if ((size_t) source->block_count > SIZE_MAX / sizeof(*destination->blocks))
        return false;
    destination->blocks = xr_calloc(source->block_count, sizeof(*destination->blocks));
    if (!destination->blocks)
        return false;
    for (uint32_t block = 0; block < source->block_count; ++block) {
        if (!lower_block(&source->blocks[block], &destination->blocks[block]))
            return false;
    }
    return true;
}

static void hash_immediate(XrSHA256Context *context, const XrBackendInstruction *instruction) {
    switch (instruction->immediate_kind) {
        case XR_CORE_IR_IMMEDIATE_NONE:
            return;
        case XR_CORE_IR_IMMEDIATE_I64:
            hash_u64(context, (uint64_t) instruction->immediate.i64);
            return;
        case XR_CORE_IR_IMMEDIATE_U32:
            hash_u32(context, instruction->immediate.u32);
            return;
        case XR_CORE_IR_IMMEDIATE_BOOL:
            hash_u32(context, instruction->immediate.boolean ? 1u : 0u);
            return;
        case XR_CORE_IR_IMMEDIATE_CONSTANT:
            hash_u32(context, instruction->immediate.constant_id);
            return;
        case XR_CORE_IR_IMMEDIATE_FUNCTION:
            hash_u32(context, instruction->immediate.function_id);
            return;
        case XR_CORE_IR_IMMEDIATE_FIELD:
            hash_u32(context, instruction->immediate.field_ordinal);
            return;
        case XR_CORE_IR_IMMEDIATE_VARIANT:
            hash_u32(context, instruction->immediate.variant_ordinal);
            return;
        case XR_CORE_IR_IMMEDIATE_VARIANT_FIELD:
            hash_u32(context, instruction->immediate.variant_field.variant_ordinal);
            hash_u32(context, instruction->immediate.variant_field.field_ordinal);
            return;
        case XR_CORE_IR_IMMEDIATE_TYPE:
            hash_u16(context, instruction->immediate.type_id);
            return;
    }
}

void xr_backend_compute_lowering_digest(const XrBackendIR *ir, XrFingerprint *digest_out) {
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, (const uint8_t *) "xray:backend-ir:v1",
                     strlen("xray:backend-ir:v1"));
    xr_sha256_update(&context, ir->execution_id.bytes, sizeof(ir->execution_id.bytes));
    xr_sha256_update(&context, ir->backend_id.bytes, sizeof(ir->backend_id.bytes));
    xr_sha256_update(&context, ir->optimization_policy_id.bytes,
                     sizeof(ir->optimization_policy_id.bytes));
    hash_u32(&context, ir->pointer_width);
    hash_u32(&context, ir->entry_function);
    hash_u32(&context, ir->constant_count);
    for (uint32_t constant = 0; constant < ir->constant_count; ++constant) {
        const XrValidatedConstant *value = &ir->constants[constant];
        hash_u16(&context, value->type_id);
        hash_u32(&context, (uint32_t) value->kind);
        if (value->kind == XR_CORE_IR_CONSTANT_I64)
            hash_u64(&context, (uint64_t) value->value.i64);
        else if (value->kind == XR_CORE_IR_CONSTANT_BOOL)
            hash_u32(&context, value->value.boolean ? 1u : 0u);
    }
    hash_u32(&context, ir->function_count);
    for (uint32_t function = 0; function < ir->function_count; ++function) {
        const XrBackendFunction *fn = &ir->functions[function];
        hash_u32(&context, fn->parameter_count);
        for (uint32_t parameter = 0; parameter < fn->parameter_count; ++parameter) {
            hash_u16(&context, fn->parameter_types[parameter]);
            hash_u32(&context, (uint32_t) fn->parameter_modes[parameter]);
        }
        hash_u16(&context, fn->result_type_id);
        hash_u32(&context, (uint32_t) fn->result_ownership);
        hash_u16(&context, fn->error_type_id);
        hash_u16(&context, fn->panic_type_id);
        hash_u32(&context, fn->effect_mask);
        hash_u32(&context, fn->capability_mask);
        hash_u32(&context, fn->entry_block);
        hash_u32(&context, fn->flags);
        hash_u32(&context, fn->value_count);
        for (uint32_t value = 0; value < fn->value_count; ++value) {
            hash_u16(&context, fn->value_types[value]);
            hash_u32(&context, (uint32_t) fn->value_categories[value]);
            hash_u32(&context, (uint32_t) fn->value_ownerships[value]);
            hash_u16(&context, fn->value_representations[value]);
        }
        hash_u32(&context, fn->block_count);
        for (uint32_t block = 0; block < fn->block_count; ++block) {
            const XrBackendBlock *row = &fn->blocks[block];
            hash_u32(&context, row->argument_count);
            for (uint32_t argument = 0; argument < row->argument_count; ++argument) {
                hash_u32(&context, row->argument_ids[argument]);
                hash_u16(&context, row->argument_types[argument]);
                hash_u32(&context, (uint32_t) row->argument_categories[argument]);
                hash_u32(&context, (uint32_t) row->argument_ownerships[argument]);
            }
            hash_u32(&context, row->instruction_count);
            for (uint32_t instruction = 0; instruction < row->instruction_count; ++instruction) {
                const XrBackendInstruction *op = &row->instructions[instruction];
                hash_u16(&context, op->operation_id);
                hash_u16(&context, op->result_type_id);
                hash_u32(&context, (uint32_t) op->result_category);
                hash_u32(&context, (uint32_t) op->result_ownership);
                hash_u32(&context, op->result_id);
                hash_u32(&context, op->operand_count);
                for (uint32_t operand = 0; operand < op->operand_count; ++operand)
                    hash_u32(&context, op->operands[operand]);
                hash_u32(&context, (uint32_t) op->immediate_kind);
                hash_immediate(&context, op);
                hash_u32(&context, op->successor_count);
                for (uint32_t successor = 0; successor < op->successor_count; ++successor)
                    hash_u32(&context, op->successors[successor]);
            }
        }
    }
    xr_sha256_final(&context, digest_out->bytes);
}

XrBackendStatus xr_backend_ir_build(XrInstance *instance, const XrBackendOptions *options,
                                    XrBackendIR **ir_out, XrBackendDiagnostic *diagnostic_out) {
    if (ir_out)
        *ir_out = NULL;
    xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_OK, 0u, 0u, 0u, 0u);
    if (!instance || !ir_out || !options_valid(options)) {
        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVALID_INPUT, 0u, 0u, 0u, 0u);
        return XR_BACKEND_INVALID_INPUT;
    }
    if (!xr_execution_instance_pin(instance)) {
        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INSTANCE_UNAVAILABLE, 0u, 0u, 0u, 0u);
        return XR_BACKEND_INSTANCE_UNAVAILABLE;
    }
    const XrValidatedProgram *program = xr_execution_instance_program(instance);
    const XrTargetProfile *profile = xr_execution_instance_profile(instance);
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(profile);
    XrBackendIR *ir = xr_calloc(1u, sizeof(*ir));
    if (!ir) {
        xr_execution_instance_unpin(instance);
        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_OUT_OF_MEMORY, 0u, 0u, 0u, 0u);
        return XR_BACKEND_OUT_OF_MEMORY;
    }
    ir->program = xr_validated_program_retain(program);
    ir->profile = xr_target_profile_retain(profile);
    ir->execution_id = xr_execution_instance_id(instance);
    ir->backend_id = xr_backend_compute_id();
    ir->optimization_policy_id = xr_backend_compute_optimization_policy_id(options);
    ir->options = *options;
    ir->function_count = program->function_count;
    ir->entry_function = program->entry_function;
    ir->pointer_width = machine ? machine->data_layout.pointer.size * 8u : 0u;
    ir->constant_count = program->constant_count;
    if (program->function_count > options->max_functions ||
        (ir->pointer_width != 32u && ir->pointer_width != 64u)) {
        xr_execution_instance_unpin(instance);
        xr_backend_ir_free(ir);
        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_RESOURCE_LIMIT, 0u, 0u, 0u, 0u);
        return XR_BACKEND_RESOURCE_LIMIT;
    }
    if (program->constant_count != 0u) {
        if ((size_t) program->constant_count > SIZE_MAX / sizeof(*ir->constants)) {
            xr_execution_instance_unpin(instance);
            xr_backend_ir_free(ir);
            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_RESOURCE_LIMIT, 0u, 0u, 0u, 0u);
            return XR_BACKEND_RESOURCE_LIMIT;
        }
        ir->constants = xr_malloc((size_t) program->constant_count * sizeof(*ir->constants));
        if (!ir->constants) {
            xr_execution_instance_unpin(instance);
            xr_backend_ir_free(ir);
            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_OUT_OF_MEMORY, 0u, 0u, 0u, 0u);
            return XR_BACKEND_OUT_OF_MEMORY;
        }
        memcpy(ir->constants, program->constants,
               (size_t) program->constant_count * sizeof(*ir->constants));
    }
    if ((size_t) program->function_count > SIZE_MAX / sizeof(*ir->functions)) {
        xr_execution_instance_unpin(instance);
        xr_backend_ir_free(ir);
        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_RESOURCE_LIMIT, 0u, 0u, 0u, 0u);
        return XR_BACKEND_RESOURCE_LIMIT;
    }
    ir->functions = xr_calloc(program->function_count, sizeof(*ir->functions));
    if (!ir->functions) {
        xr_execution_instance_unpin(instance);
        xr_backend_ir_free(ir);
        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_OUT_OF_MEMORY, 0u, 0u, 0u, 0u);
        return XR_BACKEND_OUT_OF_MEMORY;
    }
    uint64_t blocks = 0u;
    uint64_t instructions = 0u;
    uint64_t values = 0u;
    for (uint32_t function = 0; function < program->function_count; ++function) {
        const XrValidatedFunction *source = &program->functions[function];
        blocks += source->block_count;
        values += source->value_count;
        for (uint32_t block = 0; block < source->block_count; ++block) {
            instructions += source->blocks[block].instruction_count;
            for (uint32_t instruction = 0; instruction < source->blocks[block].instruction_count;
                 ++instruction) {
                uint16_t operation_id =
                    source->blocks[block].instructions[instruction].operation_id;
                if (!operation_is_supported(operation_id)) {
                    xr_execution_instance_unpin(instance);
                    xr_backend_ir_free(ir);
                    xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_UNSUPPORTED_OPERATION,
                                              operation_id, function, block, instruction);
                    return XR_BACKEND_UNSUPPORTED_OPERATION;
                }
            }
        }
        if (blocks > options->max_blocks || instructions > options->max_instructions ||
            values > options->max_values || !lower_function(source, &ir->functions[function])) {
            xr_execution_instance_unpin(instance);
            xr_backend_ir_free(ir);
            XrBackendStatus status = blocks > options->max_blocks ||
                                             instructions > options->max_instructions ||
                                             values > options->max_values
                                         ? XR_BACKEND_RESOURCE_LIMIT
                                         : XR_BACKEND_OUT_OF_MEMORY;
            xr_backend_set_diagnostic(diagnostic_out, status, 0u, function, 0u, 0u);
            return status;
        }
    }
    ir->instruction_count = (size_t) instructions;
    xr_backend_compute_lowering_digest(ir, &ir->lowering_digest);
    bool verified = xr_backend_ir_verify(ir, diagnostic_out) &&
                    xr_backend_ir_translation_validate(ir, diagnostic_out);
    xr_execution_instance_unpin(instance);
    if (!verified) {
        XrBackendStatus status =
            diagnostic_out ? diagnostic_out->status : XR_BACKEND_INVARIANT_REJECTED;
        xr_backend_ir_free(ir);
        return status;
    }
    ir->verified = true;
    *ir_out = ir;
    return XR_BACKEND_OK;
}

void xr_backend_ir_free(XrBackendIR *ir) {
    if (!ir)
        return;
    for (uint32_t function = 0; function < ir->function_count; ++function)
        free_function(&ir->functions[function]);
    xr_free(ir->functions);
    xr_free(ir->constants);
    xr_target_profile_free(ir->profile);
    xr_validated_program_free(ir->program);
    xr_free(ir);
}

XrExecutionId xr_backend_ir_execution_id(const XrBackendIR *ir) {
    return ir ? ir->execution_id : (XrExecutionId) {0};
}

XrBackendId xr_backend_ir_backend_id(const XrBackendIR *ir) {
    return ir ? ir->backend_id : (XrBackendId) {0};
}

XrOptimizationPolicyId xr_backend_ir_optimization_policy_id(const XrBackendIR *ir) {
    return ir ? ir->optimization_policy_id : (XrOptimizationPolicyId) {0};
}

XrFingerprint xr_backend_ir_lowering_digest(const XrBackendIR *ir) {
    return ir ? ir->lowering_digest : (XrFingerprint) {0};
}

size_t xr_backend_ir_instruction_count(const XrBackendIR *ir) {
    return ir ? ir->instruction_count : 0u;
}

const char *xr_backend_status_name(XrBackendStatus status) {
    switch (status) {
        case XR_BACKEND_OK:
            return "ok";
        case XR_BACKEND_INVALID_INPUT:
            return "invalid-input";
        case XR_BACKEND_INSTANCE_UNAVAILABLE:
            return "instance-unavailable";
        case XR_BACKEND_UNSUPPORTED_OPERATION:
            return "unsupported-operation";
        case XR_BACKEND_RESOURCE_LIMIT:
            return "resource-limit";
        case XR_BACKEND_OUT_OF_MEMORY:
            return "out-of-memory";
        case XR_BACKEND_INVARIANT_REJECTED:
            return "invariant-rejected";
        case XR_BACKEND_TRANSLATION_REJECTED:
            return "translation-rejected";
        case XR_BACKEND_EMISSION_REJECTED:
            return "emission-rejected";
        case XR_BACKEND_TOOLCHAIN_REJECTED:
            return "toolchain-rejected";
        case XR_BACKEND_ARTIFACT_REJECTED:
            return "artifact-rejected";
    }
    return "unknown";
}
