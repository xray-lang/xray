/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_verify.c - Bounded semantic admission for canonical XrProgram
 */

#include "xr_program_verify.h"

#include "../base/xmalloc.h"
#include "../core/xr_core_spec_gen.h"
#include "xr_validated_program_internal.h"

#include <string.h>

typedef struct VerifyReader {
    const uint8_t *bytes;
    size_t size;
    size_t offset;
    bool valid;
} VerifyReader;

typedef struct VerifyContext {
    XrProgramVerifyBudget budget;
    uint64_t work;
    XrProgramDiagnostic diagnostic;
    XrValidatedProgram *program;
} VerifyContext;

static XrProgramSemanticLocation no_location(void) {
    XrProgramSemanticLocation location = {
        .section_id = 0,
        .function_id = XR_PROGRAM_LOCATION_NONE,
        .block_id = XR_PROGRAM_LOCATION_NONE,
        .instruction_id = XR_PROGRAM_LOCATION_NONE,
        .value_id = XR_PROGRAM_LOCATION_NONE,
    };
    return location;
}

static void reject(VerifyContext *context, XrProgramDiagnosticKind kind,
                   XrProgramSemanticLocation location) {
    if (context->diagnostic.kind != XR_PROGRAM_DIAGNOSTIC_NONE)
        return;
    context->diagnostic.kind = kind;
    context->diagnostic.location = location;
}

static bool spend(VerifyContext *context, uint64_t amount, XrProgramSemanticLocation location) {
    if (amount > context->budget.max_work || context->work > context->budget.max_work - amount) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT, location);
        return false;
    }
    context->work += amount;
    return true;
}

static uint64_t take_uvar(VerifyReader *reader) {
    uint64_t value = 0;
    unsigned shift = 0;
    for (unsigned count = 0; count < 10u; ++count) {
        if (!reader->valid || reader->offset >= reader->size) {
            reader->valid = false;
            return 0;
        }
        uint8_t byte = reader->bytes[reader->offset++];
        value |= (uint64_t) (byte & UINT8_C(0x7f)) << shift;
        if ((byte & UINT8_C(0x80)) == 0)
            return value;
        shift += 7u;
    }
    reader->valid = false;
    return 0;
}

static int64_t take_svar(VerifyReader *reader) {
    uint64_t value = take_uvar(reader);
    if ((value & 1u) == 0u)
        return (int64_t) (value >> 1u);
    return -(int64_t) (value >> 1u) - 1;
}

static VerifyReader section_reader(const XrProgramView *view, uint32_t section_index) {
    XrProgramSectionView section = view->sections[section_index];
    VerifyReader reader = {
        .bytes = view->artifact + section.offset,
        .size = (size_t) section.size,
        .valid = true,
    };
    return reader;
}

static bool reader_done(const VerifyReader *reader) {
    return reader->valid && reader->offset == reader->size;
}

static bool type_is_runtime(uint64_t type_id) {
    return type_id <= XR_CORE_TYPE_ERROR;
}

static bool parse_current_core_spec_fingerprint(uint8_t output[XR_PROGRAM_DIGEST_SIZE]) {
    const char *hex = XR_CORE_SPEC_SEMANTIC_SHA256;
    for (size_t index = 0; index < XR_PROGRAM_DIGEST_SIZE; ++index) {
        unsigned high = (unsigned) (hex[index * 2u] >= 'a' ? hex[index * 2u] - 'a' + 10
                                                           : hex[index * 2u] - '0');
        unsigned low = (unsigned) (hex[index * 2u + 1u] >= 'a' ? hex[index * 2u + 1u] - 'a' + 10
                                                               : hex[index * 2u + 1u] - '0');
        if (high > 15u || low > 15u)
            return false;
        output[index] = (uint8_t) ((high << 4u) | low);
    }
    return hex[XR_PROGRAM_DIGEST_SIZE * 2u] == '\0';
}

static void free_instruction(XrValidatedInstruction *instruction) {
    xr_free(instruction->operands);
    xr_free(instruction->successors);
}

static void free_function(XrValidatedFunction *function) {
    if (!function)
        return;
    for (uint32_t block = 0; block < function->block_count; ++block) {
        XrValidatedBlock *row = &function->blocks[block];
        for (uint32_t instruction = 0; instruction < row->instruction_count; ++instruction)
            free_instruction(&row->instructions[instruction]);
        xr_free(row->instructions);
        xr_free(row->argument_ids);
        xr_free(row->argument_types);
    }
    xr_free(function->blocks);
    xr_free(function->parameter_types);
    xr_free(function->value_types);
    xr_free(function->value_blocks);
    xr_free(function->value_positions);
}

void xr_validated_program_free(XrValidatedProgram *program) {
    if (!program)
        return;
    if (atomic_fetch_sub_explicit(&program->references, 1u, memory_order_acq_rel) != 1u)
        return;
    for (uint32_t function = 0; function < program->function_count; ++function)
        free_function(&program->functions[function]);
    xr_free(program->functions);
    xr_free(program->constants);
    xr_free(program->bytes);
    xr_free(program);
}

XrValidatedProgram *xr_validated_program_retain(const XrValidatedProgram *program) {
    XrValidatedProgram *retained = (XrValidatedProgram *) program;
    if (retained)
        atomic_fetch_add_explicit(&retained->references, 1u, memory_order_relaxed);
    return retained;
}

static bool parse_constants(VerifyContext *context, const XrProgramView *view) {
    VerifyReader reader = section_reader(view, XR_PROGRAM_SECTION_CONSTANTS - 1u);
    uint64_t count = take_uvar(&reader);
    XrProgramSemanticLocation location = no_location();
    location.section_id = XR_PROGRAM_SECTION_CONSTANTS;
    if (count > XR_PROGRAM_LIMIT_CONSTANTS || count > SIZE_MAX / sizeof(XrValidatedConstant) ||
        !spend(context, count + 1u, location))
        return false;
    context->program->constant_count = (uint32_t) count;
    if (count != 0) {
        context->program->constants = xr_calloc((size_t) count, sizeof(XrValidatedConstant));
        if (!context->program->constants) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
            return false;
        }
    }
    for (uint32_t index = 0; index < (uint32_t) count; ++index) {
        uint64_t id = take_uvar(&reader);
        uint64_t type_id = take_uvar(&reader);
        uint64_t kind = take_uvar(&reader);
        XrValidatedConstant *constant = &context->program->constants[index];
        location.value_id = index;
        if (id != index || !type_is_runtime(type_id) ||
            (kind != XR_CORE_IR_CONSTANT_I64 && kind != XR_CORE_IR_CONSTANT_BOOL)) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
            return false;
        }
        constant->type_id = (uint16_t) type_id;
        constant->kind = (XrCoreIrConstantKind) kind;
        if (kind == XR_CORE_IR_CONSTANT_I64) {
            constant->value.i64 = take_svar(&reader);
            if (type_id != XR_CORE_TYPE_I64) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
                return false;
            }
        } else {
            uint64_t value = take_uvar(&reader);
            if (type_id != XR_CORE_TYPE_BOOL || value > 1u) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
                return false;
            }
            constant->value.boolean = value != 0u;
        }
    }
    if (!reader_done(&reader)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_STRUCTURAL, location);
        return false;
    }
    return true;
}

static bool parse_functions(VerifyContext *context, const XrProgramView *view) {
    VerifyReader reader = section_reader(view, XR_PROGRAM_SECTION_FUNCTIONS - 1u);
    uint64_t count = take_uvar(&reader);
    XrProgramSemanticLocation location = no_location();
    location.section_id = XR_PROGRAM_SECTION_FUNCTIONS;
    if (count == 0 || count > context->budget.max_functions ||
        count > SIZE_MAX / sizeof(XrValidatedFunction) || !spend(context, count + 1u, location)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT, location);
        return false;
    }
    context->program->function_count = (uint32_t) count;
    context->program->functions = xr_calloc((size_t) count, sizeof(XrValidatedFunction));
    if (!context->program->functions) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
        return false;
    }
    for (uint32_t id = 0; id < (uint32_t) count; ++id) {
        XrValidatedFunction *function = &context->program->functions[id];
        uint64_t encoded_id = take_uvar(&reader);
        uint64_t parameter_count = take_uvar(&reader);
        location.function_id = id;
        if (encoded_id != id || parameter_count > XR_PROGRAM_LIMIT_OPERANDS_PER_OPERATION ||
            parameter_count > SIZE_MAX / sizeof(uint16_t) ||
            !spend(context, parameter_count, location)) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_FUNCTION, location);
            return false;
        }
        function->parameter_count = (uint32_t) parameter_count;
        if (parameter_count != 0) {
            function->parameter_types = xr_calloc((size_t) parameter_count, sizeof(uint16_t));
            if (!function->parameter_types) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
                return false;
            }
        }
        for (uint32_t parameter = 0; parameter < (uint32_t) parameter_count; ++parameter) {
            uint64_t type_id = take_uvar(&reader);
            if (!type_is_runtime(type_id) || type_id == XR_CORE_TYPE_VOID) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
                return false;
            }
            function->parameter_types[parameter] = (uint16_t) type_id;
        }
        uint64_t result_type = take_uvar(&reader);
        uint64_t effect_mask = take_uvar(&reader);
        uint64_t capability_mask = take_uvar(&reader);
        uint64_t entry_block = take_uvar(&reader);
        uint64_t block_count = take_uvar(&reader);
        uint64_t value_count = take_uvar(&reader);
        uint64_t flags = take_uvar(&reader);
        if (!type_is_runtime(result_type) || effect_mask > UINT32_MAX ||
            capability_mask > UINT32_MAX || block_count == 0 ||
            block_count > context->budget.max_blocks_per_function || entry_block >= block_count ||
            value_count > context->budget.max_values_per_function || flags > UINT32_MAX ||
            block_count > SIZE_MAX / sizeof(XrValidatedBlock) ||
            value_count > SIZE_MAX / sizeof(uint16_t) ||
            !spend(context, block_count + value_count, location)) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_FUNCTION, location);
            return false;
        }
        function->result_type_id = (uint16_t) result_type;
        function->effect_mask = (uint32_t) effect_mask;
        function->capability_mask = (uint32_t) capability_mask;
        function->entry_block = (uint32_t) entry_block;
        function->block_count = (uint32_t) block_count;
        function->value_count = (uint32_t) value_count;
        function->flags = (uint32_t) flags;
        function->blocks = xr_calloc((size_t) block_count, sizeof(XrValidatedBlock));
        function->value_types =
            xr_calloc((size_t) (value_count ? value_count : 1u), sizeof(uint16_t));
        function->value_blocks =
            xr_calloc((size_t) (value_count ? value_count : 1u), sizeof(uint32_t));
        function->value_positions =
            xr_calloc((size_t) (value_count ? value_count : 1u), sizeof(uint32_t));
        if (!function->blocks || !function->value_types || !function->value_blocks ||
            !function->value_positions) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
            return false;
        }
    }
    if (!reader_done(&reader)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_STRUCTURAL, location);
        return false;
    }
    return true;
}

static bool allocate_u32(uint64_t count, uint32_t **output) {
    *output = NULL;
    if (count == 0)
        return true;
    if (count > SIZE_MAX / sizeof(uint32_t))
        return false;
    *output = xr_calloc((size_t) count, sizeof(uint32_t));
    return *output != NULL;
}

static bool parse_instruction(VerifyContext *context, VerifyReader *reader,
                              XrValidatedInstruction *instruction,
                              XrProgramSemanticLocation location) {
    uint64_t operation_id = take_uvar(reader);
    uint64_t result_plus_one = take_uvar(reader);
    uint64_t result_type = take_uvar(reader);
    uint64_t operand_count = take_uvar(reader);
    if (!reader->valid) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_STRUCTURAL, location);
        return false;
    }
    if (operation_id > UINT16_MAX || !xr_core_spec_operation_by_id((uint16_t) operation_id)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_ARITY, location);
        return false;
    }
    if (result_plus_one > UINT32_MAX || !type_is_runtime(result_type)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
        return false;
    }
    if (operand_count > XR_PROGRAM_LIMIT_OPERANDS_PER_OPERATION) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT, location);
        return false;
    }
    if (!spend(context, operand_count + 1u, location))
        return false;
    if (!allocate_u32(operand_count, &instruction->operands)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
        return false;
    }
    instruction->operation_id = (uint16_t) operation_id;
    instruction->result_id =
        result_plus_one == 0 ? XR_PROGRAM_LOCATION_NONE : (uint32_t) result_plus_one - 1u;
    instruction->result_type_id = (uint16_t) result_type;
    instruction->operand_count = (uint32_t) operand_count;
    for (uint32_t operand = 0; operand < (uint32_t) operand_count; ++operand) {
        uint64_t value = take_uvar(reader);
        if (value > UINT32_MAX) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_VALUE_USE, location);
            return false;
        }
        instruction->operands[operand] = (uint32_t) value;
    }
    uint64_t immediate_kind = take_uvar(reader);
    if (immediate_kind > XR_CORE_IR_IMMEDIATE_FUNCTION) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
        return false;
    }
    instruction->immediate_kind = (XrCoreIrImmediateKind) immediate_kind;
    switch (instruction->immediate_kind) {
        case XR_CORE_IR_IMMEDIATE_NONE:
            break;
        case XR_CORE_IR_IMMEDIATE_I64:
            instruction->immediate.i64 = take_svar(reader);
            break;
        case XR_CORE_IR_IMMEDIATE_U32: {
            uint64_t value = take_uvar(reader);
            if (value > UINT32_MAX) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
                return false;
            }
            instruction->immediate.u32 = (uint32_t) value;
            break;
        }
        case XR_CORE_IR_IMMEDIATE_BOOL: {
            uint64_t value = take_uvar(reader);
            if (value > 1u) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
                return false;
            }
            instruction->immediate.boolean = value != 0u;
            break;
        }
        case XR_CORE_IR_IMMEDIATE_CONSTANT: {
            uint64_t value = take_uvar(reader);
            if (value > UINT32_MAX) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
                return false;
            }
            instruction->immediate.constant_id = (uint32_t) value;
            break;
        }
        case XR_CORE_IR_IMMEDIATE_FUNCTION: {
            uint64_t value = take_uvar(reader);
            if (value > UINT32_MAX) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
                return false;
            }
            instruction->immediate.function_id = (uint32_t) value;
            break;
        }
        default:
            return false;
    }
    uint64_t successor_count = take_uvar(reader);
    if (!reader->valid) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_STRUCTURAL, location);
        return false;
    }
    if (successor_count > XR_PROGRAM_LIMIT_SUCCESSORS_PER_OPERATION) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT, location);
        return false;
    }
    if (!spend(context, successor_count, location))
        return false;
    if (!allocate_u32(successor_count, &instruction->successors)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
        return false;
    }
    instruction->successor_count = (uint32_t) successor_count;
    for (uint32_t successor = 0; successor < (uint32_t) successor_count; ++successor) {
        uint64_t value = take_uvar(reader);
        if (value > UINT32_MAX) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW, location);
            return false;
        }
        instruction->successors[successor] = (uint32_t) value;
    }
    if (!reader->valid) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_STRUCTURAL, location);
        return false;
    }
    return true;
}

static bool parse_code(VerifyContext *context, const XrProgramView *view) {
    VerifyReader reader = section_reader(view, XR_PROGRAM_SECTION_CODE - 1u);
    uint64_t function_count = take_uvar(&reader);
    XrProgramSemanticLocation location = no_location();
    location.section_id = XR_PROGRAM_SECTION_CODE;
    if (function_count != context->program->function_count) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_FUNCTION, location);
        return false;
    }
    uint64_t operation_count = 0;
    for (uint32_t function_id = 0; function_id < context->program->function_count; ++function_id) {
        XrValidatedFunction *function = &context->program->functions[function_id];
        uint64_t encoded_function = take_uvar(&reader);
        uint64_t block_count = take_uvar(&reader);
        location.function_id = function_id;
        if (encoded_function != function_id || block_count != function->block_count) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_FUNCTION, location);
            return false;
        }
        uint32_t next_value = 0;
        for (uint32_t block_id = 0; block_id < function->block_count; ++block_id) {
            XrValidatedBlock *block = &function->blocks[block_id];
            uint64_t encoded_block = take_uvar(&reader);
            uint64_t argument_count = take_uvar(&reader);
            location.block_id = block_id;
            if (encoded_block != block_id || argument_count > function->value_count ||
                argument_count > SIZE_MAX / sizeof(uint16_t)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_VALUE_DEFINITION, location);
                return false;
            }
            if (!allocate_u32(argument_count, &block->argument_ids)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
                return false;
            }
            block->argument_count = (uint32_t) argument_count;
            if (argument_count != 0) {
                block->argument_types = xr_calloc((size_t) argument_count, sizeof(uint16_t));
                if (!block->argument_types) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
                    return false;
                }
            }
            for (uint32_t argument = 0; argument < (uint32_t) argument_count; ++argument) {
                uint64_t value_id = take_uvar(&reader);
                uint64_t type_id = take_uvar(&reader);
                location.value_id = (uint32_t) value_id;
                if (value_id != next_value || value_id >= function->value_count ||
                    !type_is_runtime(type_id) || type_id == XR_CORE_TYPE_VOID) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_VALUE_DEFINITION, location);
                    return false;
                }
                block->argument_ids[argument] = next_value;
                block->argument_types[argument] = (uint16_t) type_id;
                function->value_types[next_value] = (uint16_t) type_id;
                function->value_blocks[next_value] = block_id;
                function->value_positions[next_value] = 0u;
                ++next_value;
            }
            uint64_t instruction_count = take_uvar(&reader);
            if (instruction_count > context->budget.max_operations ||
                instruction_count > SIZE_MAX / sizeof(XrValidatedInstruction) ||
                operation_count > context->budget.max_operations - instruction_count ||
                !spend(context, instruction_count, location)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT, location);
                return false;
            }
            operation_count += instruction_count;
            block->instruction_count = (uint32_t) instruction_count;
            if (instruction_count != 0) {
                block->instructions =
                    xr_calloc((size_t) instruction_count, sizeof(XrValidatedInstruction));
                if (!block->instructions) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
                    return false;
                }
            }
            for (uint32_t instruction_id = 0; instruction_id < (uint32_t) instruction_count;
                 ++instruction_id) {
                location.instruction_id = instruction_id;
                if (!parse_instruction(context, &reader, &block->instructions[instruction_id],
                                       location))
                    return false;
                XrValidatedInstruction *instruction = &block->instructions[instruction_id];
                if (instruction->result_id != XR_PROGRAM_LOCATION_NONE) {
                    location.value_id = instruction->result_id;
                    if (instruction->result_id != next_value ||
                        instruction->result_id >= function->value_count ||
                        instruction->result_type_id == XR_CORE_TYPE_VOID) {
                        reject(context, XR_PROGRAM_DIAGNOSTIC_VALUE_DEFINITION, location);
                        return false;
                    }
                    function->value_types[next_value] = instruction->result_type_id;
                    function->value_blocks[next_value] = block_id;
                    function->value_positions[next_value] = instruction_id + 1u;
                    ++next_value;
                } else if (instruction->result_type_id != XR_CORE_TYPE_VOID) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_VALUE_DEFINITION, location);
                    return false;
                }
            }
        }
        if (next_value != function->value_count) {
            location.value_id = next_value;
            reject(context, XR_PROGRAM_DIAGNOSTIC_VALUE_DEFINITION, location);
            return false;
        }
    }
    if (!reader_done(&reader)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_STRUCTURAL, location);
        return false;
    }
    return true;
}

static bool value_is_available(const XrValidatedFunction *function, uint32_t block_id,
                               uint32_t instruction_id, uint32_t value_id) {
    if (value_id >= function->value_count)
        return false;
    return function->value_blocks[value_id] == block_id &&
           function->value_positions[value_id] <= instruction_id;
}

static bool instruction_is_terminator(uint16_t operation_id) {
    return operation_id == XR_CORE_OP_CORE_BRANCH ||
           operation_id == XR_CORE_OP_CORE_CONDITIONAL_BRANCH ||
           operation_id == XR_CORE_OP_CORE_RETURN || operation_id == XR_CORE_OP_CORE_TRAP ||
           operation_id == XR_CORE_OP_CORE_ERROR_PUBLISH;
}

static bool expect_shape(VerifyContext *context, const XrValidatedInstruction *instruction,
                         XrProgramSemanticLocation location, uint32_t operands, uint32_t successors,
                         XrCoreIrImmediateKind immediate_kind, uint16_t result_type,
                         bool has_result) {
    if (instruction->operand_count != operands || instruction->successor_count != successors) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_ARITY, location);
        return false;
    }
    if (instruction->immediate_kind != immediate_kind) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
        return false;
    }
    if ((instruction->result_id != XR_PROGRAM_LOCATION_NONE) != has_result ||
        instruction->result_type_id != result_type) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
        return false;
    }
    return true;
}

static bool operand_type_is(const XrValidatedFunction *function,
                            const XrValidatedInstruction *instruction, uint32_t operand,
                            uint16_t type_id) {
    return operand < instruction->operand_count &&
           function->value_types[instruction->operands[operand]] == type_id;
}

static bool verify_successor_arguments(VerifyContext *context, const XrValidatedFunction *function,
                                       const XrValidatedInstruction *instruction,
                                       uint32_t successor_index, uint32_t operand_start,
                                       XrProgramSemanticLocation location) {
    if (successor_index >= instruction->successor_count ||
        instruction->successors[successor_index] >= function->block_count) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW, location);
        return false;
    }
    const XrValidatedBlock *target = &function->blocks[instruction->successors[successor_index]];
    if (operand_start > instruction->operand_count ||
        target->argument_count > instruction->operand_count - operand_start) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_ARITY, location);
        return false;
    }
    for (uint32_t index = 0; index < target->argument_count; ++index) {
        if (!operand_type_is(function, instruction, operand_start + index,
                             target->argument_types[index])) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
            return false;
        }
    }
    return true;
}

static bool verify_operation(VerifyContext *context, uint32_t function_id, uint32_t block_id,
                             uint32_t instruction_id, uint32_t *local_effects,
                             uint32_t *local_capabilities) {
    XrValidatedFunction *function = &context->program->functions[function_id];
    XrValidatedInstruction *instruction = &function->blocks[block_id].instructions[instruction_id];
    XrProgramSemanticLocation location = {
        .section_id = XR_PROGRAM_SECTION_CODE,
        .function_id = function_id,
        .block_id = block_id,
        .instruction_id = instruction_id,
        .value_id = instruction->result_id,
    };
    const XrCoreOperationSpec *spec = xr_core_spec_operation_by_id(instruction->operation_id);
    if (!spec) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_ARITY, location);
        return false;
    }
    *local_effects |= spec->effect_mask;
    *local_capabilities |= spec->capability_mask;
    for (uint32_t operand = 0; operand < instruction->operand_count; ++operand) {
        location.value_id = instruction->operands[operand];
        if (!value_is_available(function, block_id, instruction_id,
                                instruction->operands[operand])) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_VALUE_USE, location);
            return false;
        }
    }
    location.value_id = instruction->result_id;
    switch (instruction->operation_id) {
        case XR_CORE_OP_CORE_CONSTANT_I64:
        case XR_CORE_OP_CORE_CONSTANT_BOOL: {
            uint16_t expected = instruction->operation_id == XR_CORE_OP_CORE_CONSTANT_I64
                                    ? XR_CORE_TYPE_I64
                                    : XR_CORE_TYPE_BOOL;
            if (!expect_shape(context, instruction, location, 0, 0, XR_CORE_IR_IMMEDIATE_CONSTANT,
                              expected, true) ||
                instruction->immediate.constant_id >= context->program->constant_count ||
                context->program->constants[instruction->immediate.constant_id].type_id !=
                    expected) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
                return false;
            }
            return true;
        }
        case XR_CORE_OP_CORE_ADD_I64:
        case XR_CORE_OP_CORE_SUB_I64:
        case XR_CORE_OP_CORE_MUL_I64:
            if (!expect_shape(context, instruction, location, 2, 0, XR_CORE_IR_IMMEDIATE_U32,
                              XR_CORE_TYPE_I64, true) ||
                instruction->immediate.u32 > 1u ||
                !operand_type_is(function, instruction, 0, XR_CORE_TYPE_I64) ||
                !operand_type_is(function, instruction, 1, XR_CORE_TYPE_I64)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            return true;
        case XR_CORE_OP_CORE_DIV_I64:
            if (!expect_shape(context, instruction, location, 2, 0, XR_CORE_IR_IMMEDIATE_U32,
                              XR_CORE_TYPE_I64, true) ||
                instruction->immediate.u32 != 0u ||
                !operand_type_is(function, instruction, 0, XR_CORE_TYPE_I64) ||
                !operand_type_is(function, instruction, 1, XR_CORE_TYPE_I64)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            return true;
        case XR_CORE_OP_CORE_COMPARE_I64:
            if (!expect_shape(context, instruction, location, 2, 0, XR_CORE_IR_IMMEDIATE_U32,
                              XR_CORE_TYPE_BOOL, true) ||
                instruction->immediate.u32 > 5u ||
                !operand_type_is(function, instruction, 0, XR_CORE_TYPE_I64) ||
                !operand_type_is(function, instruction, 1, XR_CORE_TYPE_I64)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            return true;
        case XR_CORE_OP_CORE_BLOCK_ARGUMENT: {
            const XrValidatedBlock *block = &function->blocks[block_id];
            if (!expect_shape(context, instruction, location, block->argument_count, 0,
                              XR_CORE_IR_IMMEDIATE_NONE, XR_CORE_TYPE_VOID, false))
                return false;
            for (uint32_t index = 0; index < block->argument_count; ++index) {
                if (instruction->operands[index] != block->argument_ids[index]) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_VALUE_USE, location);
                    return false;
                }
            }
            if (instruction_id != 0u) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW, location);
                return false;
            }
            return true;
        }
        case XR_CORE_OP_CORE_BRANCH: {
            if (instruction->successor_count != 1u ||
                instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_NONE ||
                instruction->result_id != XR_PROGRAM_LOCATION_NONE ||
                instruction->result_type_id != XR_CORE_TYPE_VOID ||
                instruction->successors[0] >= function->block_count ||
                instruction->operand_count !=
                    function->blocks[instruction->successors[0]].argument_count ||
                !verify_successor_arguments(context, function, instruction, 0, 0, location)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW, location);
                return false;
            }
            return true;
        }
        case XR_CORE_OP_CORE_CONDITIONAL_BRANCH: {
            if (instruction->successor_count != 2u ||
                instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_NONE ||
                instruction->result_id != XR_PROGRAM_LOCATION_NONE ||
                instruction->result_type_id != XR_CORE_TYPE_VOID ||
                instruction->successors[0] >= function->block_count ||
                instruction->successors[1] >= function->block_count ||
                instruction->operand_count !=
                    1u + function->blocks[instruction->successors[0]].argument_count +
                        function->blocks[instruction->successors[1]].argument_count ||
                !operand_type_is(function, instruction, 0, XR_CORE_TYPE_BOOL) ||
                !verify_successor_arguments(context, function, instruction, 0, 1u, location) ||
                !verify_successor_arguments(
                    context, function, instruction, 1,
                    1u + function->blocks[instruction->successors[0]].argument_count, location)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW, location);
                return false;
            }
            return true;
        }
        case XR_CORE_OP_CORE_RETURN: {
            uint32_t expected = function->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
            if (!expect_shape(context, instruction, location, expected, 0,
                              XR_CORE_IR_IMMEDIATE_NONE, XR_CORE_TYPE_VOID, false) ||
                (expected == 1u &&
                 !operand_type_is(function, instruction, 0, function->result_type_id))) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            return true;
        }
        case XR_CORE_OP_CORE_CALL_SEALED_DIRECT: {
            if (instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_FUNCTION ||
                instruction->successor_count != 0u ||
                instruction->immediate.function_id >= context->program->function_count) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
                return false;
            }
            const XrValidatedFunction *callee =
                &context->program->functions[instruction->immediate.function_id];
            if (instruction->operand_count != callee->parameter_count ||
                (callee->result_type_id == XR_CORE_TYPE_VOID) !=
                    (instruction->result_id == XR_PROGRAM_LOCATION_NONE) ||
                instruction->result_type_id != callee->result_type_id) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            for (uint32_t argument = 0; argument < callee->parameter_count; ++argument) {
                if (!operand_type_is(function, instruction, argument,
                                     callee->parameter_types[argument])) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                    return false;
                }
            }
            if ((function->effect_mask & callee->effect_mask) != callee->effect_mask) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_EFFECT, location);
                return false;
            }
            if ((function->capability_mask & callee->capability_mask) != callee->capability_mask) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_CAPABILITY, location);
                return false;
            }
            return true;
        }
        case XR_CORE_OP_CORE_TRAP:
            if (!expect_shape(context, instruction, location, 0, 0, XR_CORE_IR_IMMEDIATE_U32,
                              XR_CORE_TYPE_VOID, false) ||
                instruction->immediate.u32 != 4u) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
                return false;
            }
            return true;
        case XR_CORE_OP_CORE_ERROR_PUBLISH:
            if (!expect_shape(context, instruction, location, 1, 0, XR_CORE_IR_IMMEDIATE_NONE,
                              XR_CORE_TYPE_VOID, false) ||
                !operand_type_is(function, instruction, 0, XR_CORE_TYPE_ERROR)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            return true;
        case XR_CORE_OP_CORE_TARGET_POINTER_WIDTH:
            return expect_shape(context, instruction, location, 0, 0, XR_CORE_IR_IMMEDIATE_NONE,
                                XR_CORE_TYPE_U32, true);
        default:
            reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_ARITY, location);
            return false;
    }
}

static bool verify_function(VerifyContext *context, uint32_t function_id) {
    XrValidatedFunction *function = &context->program->functions[function_id];
    XrProgramSemanticLocation location = no_location();
    location.section_id = XR_PROGRAM_SECTION_FUNCTIONS;
    location.function_id = function_id;
    if ((function->flags & ~XR_PROGRAM_FUNCTION_ENTRY) != 0u ||
        (function->effect_mask & ~UINT32_C(0x0f)) != 0u ||
        (function->capability_mask & ~UINT32_C(0x01)) != 0u) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_FUNCTION, location);
        return false;
    }
    XrValidatedBlock *entry = &function->blocks[function->entry_block];
    if (entry->argument_count != function->parameter_count) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_FUNCTION, location);
        return false;
    }
    for (uint32_t parameter = 0; parameter < function->parameter_count; ++parameter) {
        if (entry->argument_types[parameter] != function->parameter_types[parameter]) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
            return false;
        }
    }
    uint32_t local_effects = 0;
    uint32_t local_capabilities = 0;
    for (uint32_t block_id = 0; block_id < function->block_count; ++block_id) {
        XrValidatedBlock *block = &function->blocks[block_id];
        location.block_id = block_id;
        if (block->instruction_count == 0 ||
            !instruction_is_terminator(
                block->instructions[block->instruction_count - 1u].operation_id)) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW, location);
            return false;
        }
        for (uint32_t instruction_id = 0; instruction_id < block->instruction_count;
             ++instruction_id) {
            if (instruction_id + 1u != block->instruction_count &&
                instruction_is_terminator(block->instructions[instruction_id].operation_id)) {
                location.instruction_id = instruction_id;
                reject(context, XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW, location);
                return false;
            }
            if (!verify_operation(context, function_id, block_id, instruction_id, &local_effects,
                                  &local_capabilities))
                return false;
        }
    }
    if ((function->effect_mask & local_effects) != local_effects) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_EFFECT, location);
        return false;
    }
    if ((function->capability_mask & local_capabilities) != local_capabilities) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_CAPABILITY, location);
        return false;
    }

    bool *reachable = xr_calloc(function->block_count, sizeof(bool));
    uint32_t *queue = xr_calloc(function->block_count, sizeof(uint32_t));
    if (!reachable || !queue) {
        xr_free(queue);
        xr_free(reachable);
        reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
        return false;
    }
    uint32_t head = 0;
    uint32_t tail = 0;
    reachable[function->entry_block] = true;
    queue[tail++] = function->entry_block;
    while (head != tail) {
        uint32_t block_id = queue[head++];
        const XrValidatedBlock *block = &function->blocks[block_id];
        const XrValidatedInstruction *terminator =
            &block->instructions[block->instruction_count - 1u];
        for (uint32_t successor = 0; successor < terminator->successor_count; ++successor) {
            uint32_t target = terminator->successors[successor];
            if (!reachable[target]) {
                reachable[target] = true;
                queue[tail++] = target;
            }
        }
    }
    for (uint32_t block_id = 0; block_id < function->block_count; ++block_id) {
        if (!reachable[block_id]) {
            location.block_id = block_id;
            reject(context, XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW, location);
            xr_free(queue);
            xr_free(reachable);
            return false;
        }
    }
    xr_free(queue);
    xr_free(reachable);
    return true;
}

XrProgramVerifyBudget xr_program_verify_default_budget(void) {
    XrProgramVerifyBudget budget = {
        .decode =
            {
                .max_bytes = XR_PROGRAM_LIMIT_ARTIFACT_BYTES,
                .max_records = XR_PROGRAM_LIMIT_VALUES_PER_FUNCTION,
                .max_operations = XR_PROGRAM_LIMIT_OPERATIONS,
            },
        .max_work = UINT64_C(100000000),
        .max_functions = XR_PROGRAM_LIMIT_FUNCTIONS,
        .max_blocks_per_function = XR_PROGRAM_LIMIT_BLOCKS_PER_FUNCTION,
        .max_values_per_function = XR_PROGRAM_LIMIT_VALUES_PER_FUNCTION,
        .max_operations = XR_PROGRAM_LIMIT_OPERATIONS,
    };
    return budget;
}

XrProgramVerifyStatus xr_program_validate(const uint8_t *bytes, size_t size,
                                          const XrProgramVerifyBudget *budget,
                                          XrValidatedProgram **program_out,
                                          XrProgramDiagnostic *diagnostic_out) {
    XrProgramVerifyBudget selected = budget ? *budget : xr_program_verify_default_budget();
    VerifyContext context = {.budget = selected, .diagnostic = {.location = no_location()}};
    XrProgramView view;
    uint8_t expected_fingerprint[XR_PROGRAM_DIGEST_SIZE];
    if (program_out)
        *program_out = NULL;
    if (diagnostic_out)
        memset(diagnostic_out, 0, sizeof(*diagnostic_out));
    if (!bytes || !program_out || selected.max_work == 0 || selected.max_functions == 0 ||
        selected.max_blocks_per_function == 0 || selected.max_values_per_function == 0 ||
        selected.max_operations == 0) {
        return XR_PROGRAM_VERIFY_INVALID_INPUT;
    }
    XrProgramDecodeStatus decode =
        xr_program_decode_structure(bytes, size, &selected.decode, &view, NULL, 0);
    if (decode != XR_PROGRAM_DECODE_OK) {
        context.diagnostic.kind = XR_PROGRAM_DIAGNOSTIC_STRUCTURAL;
        context.diagnostic.decode_status = decode;
        if (diagnostic_out)
            *diagnostic_out = context.diagnostic;
        return decode == XR_PROGRAM_DECODE_RESOURCE_LIMIT ? XR_PROGRAM_VERIFY_RESOURCE_LIMIT
                                                          : XR_PROGRAM_VERIFY_STRUCTURAL_REJECTED;
    }
    if (!parse_current_core_spec_fingerprint(expected_fingerprint) ||
        memcmp(view.core_spec_fingerprint, expected_fingerprint, sizeof(expected_fingerprint)) !=
            0) {
        context.diagnostic.kind = XR_PROGRAM_DIAGNOSTIC_CORE_SPEC_IDENTITY;
        if (diagnostic_out)
            *diagnostic_out = context.diagnostic;
        return XR_PROGRAM_VERIFY_SEMANTIC_REJECTED;
    }
    context.program = xr_calloc(1u, sizeof(XrValidatedProgram));
    if (!context.program) {
        context.diagnostic.kind = XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY;
        if (diagnostic_out)
            *diagnostic_out = context.diagnostic;
        return XR_PROGRAM_VERIFY_OUT_OF_MEMORY;
    }
    atomic_init(&context.program->references, 1u);
    context.program->bytes = xr_malloc(size);
    if (!context.program->bytes) {
        reject(&context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, no_location());
        goto rejected;
    }
    memcpy(context.program->bytes, bytes, size);
    context.program->size = size;
    context.program->id = view.id;
    memcpy(context.program->semantic_profile_fingerprint, view.semantic_profile_fingerprint,
           XR_PROGRAM_DIGEST_SIZE);
    if (!parse_constants(&context, &view) || !parse_functions(&context, &view) ||
        !parse_code(&context, &view))
        goto rejected;

    uint32_t entry_count = 0;
    for (uint32_t function = 0; function < context.program->function_count; ++function) {
        if (!verify_function(&context, function))
            goto rejected;
        if ((context.program->functions[function].flags & XR_PROGRAM_FUNCTION_ENTRY) != 0u) {
            context.program->entry_function = function;
            ++entry_count;
        }
    }
    if (entry_count != 1u) {
        reject(&context, XR_PROGRAM_DIAGNOSTIC_ENTRY_POINT, no_location());
        goto rejected;
    }
    context.program->verifier_work = context.work;
    *program_out = context.program;
    if (diagnostic_out)
        *diagnostic_out = context.diagnostic;
    return XR_PROGRAM_VERIFY_OK;

rejected:
    if (diagnostic_out)
        *diagnostic_out = context.diagnostic;
    XrProgramVerifyStatus status = XR_PROGRAM_VERIFY_SEMANTIC_REJECTED;
    if (context.diagnostic.kind == XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT)
        status = XR_PROGRAM_VERIFY_RESOURCE_LIMIT;
    else if (context.diagnostic.kind == XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY)
        status = XR_PROGRAM_VERIFY_OUT_OF_MEMORY;
    xr_validated_program_free(context.program);
    return status;
}

XrProgramId xr_validated_program_id(const XrValidatedProgram *program) {
    XrProgramId id = {{0}};
    return program ? program->id : id;
}

uint32_t xr_validated_program_function_count(const XrValidatedProgram *program) {
    return program ? program->function_count : 0u;
}

uint32_t xr_validated_program_entry_function(const XrValidatedProgram *program) {
    return program ? program->entry_function : XR_PROGRAM_LOCATION_NONE;
}

uint64_t xr_validated_program_verifier_work(const XrValidatedProgram *program) {
    return program ? program->verifier_work : 0u;
}

const uint8_t *xr_validated_program_bytes(const XrValidatedProgram *program, size_t *size_out) {
    if (size_out)
        *size_out = program ? program->size : 0u;
    return program ? program->bytes : NULL;
}

const char *xr_program_verify_status_name(XrProgramVerifyStatus status) {
    switch (status) {
        case XR_PROGRAM_VERIFY_OK:
            return "ok";
        case XR_PROGRAM_VERIFY_INVALID_INPUT:
            return "invalid-input";
        case XR_PROGRAM_VERIFY_STRUCTURAL_REJECTED:
            return "structural-rejected";
        case XR_PROGRAM_VERIFY_SEMANTIC_REJECTED:
            return "semantic-rejected";
        case XR_PROGRAM_VERIFY_RESOURCE_LIMIT:
            return "resource-limit";
        case XR_PROGRAM_VERIFY_OUT_OF_MEMORY:
            return "out-of-memory";
        default:
            return "unknown";
    }
}

const char *xr_program_diagnostic_kind_name(XrProgramDiagnosticKind kind) {
    switch (kind) {
        case XR_PROGRAM_DIAGNOSTIC_NONE:
            return "none";
        case XR_PROGRAM_DIAGNOSTIC_STRUCTURAL:
            return "structural";
        case XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT:
            return "resource-limit";
        case XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY:
            return "out-of-memory";
        case XR_PROGRAM_DIAGNOSTIC_CORE_SPEC_IDENTITY:
            return "core-spec-identity";
        case XR_PROGRAM_DIAGNOSTIC_TYPE:
            return "type";
        case XR_PROGRAM_DIAGNOSTIC_FUNCTION:
            return "function";
        case XR_PROGRAM_DIAGNOSTIC_VALUE_DEFINITION:
            return "value-definition";
        case XR_PROGRAM_DIAGNOSTIC_VALUE_USE:
            return "value-use";
        case XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW:
            return "control-flow";
        case XR_PROGRAM_DIAGNOSTIC_OPERATION_ARITY:
            return "operation-arity";
        case XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE:
            return "operation-type";
        case XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE:
            return "operation-immediate";
        case XR_PROGRAM_DIAGNOSTIC_EFFECT:
            return "effect";
        case XR_PROGRAM_DIAGNOSTIC_CAPABILITY:
            return "capability";
        case XR_PROGRAM_DIAGNOSTIC_ENTRY_POINT:
            return "entry-point";
        default:
            return "unknown";
    }
}
