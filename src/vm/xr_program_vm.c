/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_vm.c - Generic CoreSpec dispatch over validated XrProgram
 */

#include "xr_program_vm.h"

#include "../base/xmalloc.h"
#include "../base/xsha256.h"
#include "../core/xr_core_spec_gen.h"
#include "../program/xr_validated_program_internal.h"

#include <limits.h>
#include <string.h>

typedef struct XrVmFixedInstruction {
    uint16_t operation_id;
    uint16_t result_type_id;
    uint32_t result_id;
    const uint32_t *operands;
    uint32_t operand_count;
    XrCoreIrImmediateKind immediate_kind;
    union {
        int64_t i64;
        uint32_t u32;
        bool boolean;
        uint32_t constant_id;
        uint32_t function_id;
        uint32_t field_ordinal;
        uint32_t variant_ordinal;
        struct {
            uint32_t variant_ordinal;
            uint32_t field_ordinal;
        } variant_field;
    } immediate;
    const uint32_t *successors;
    uint32_t successor_count;
} XrVmFixedInstruction;

typedef struct XrVmFixedBlock {
    XrVmFixedInstruction *instructions;
    uint32_t instruction_count;
} XrVmFixedBlock;

typedef struct XrVmFixedFunction {
    XrVmFixedBlock *blocks;
    uint32_t block_count;
} XrVmFixedFunction;

struct XrVmCode {
    XrValidatedProgram *program;
    XrExecutionCacheKey cache_key;
    XrFingerprint private_digest;
    XrVmCodeOptions options;
    uint32_t pointer_width;
    XrVmFixedFunction *fixed_functions;
    size_t private_size;
};

typedef struct XrVmInstructionView {
    uint16_t operation_id;
    uint16_t result_type_id;
    uint32_t result_id;
    const uint32_t *operands;
    uint32_t operand_count;
    XrCoreIrImmediateKind immediate_kind;
    union {
        int64_t i64;
        uint32_t u32;
        bool boolean;
        uint32_t constant_id;
        uint32_t function_id;
        uint32_t field_ordinal;
        uint32_t variant_ordinal;
        struct {
            uint32_t variant_ordinal;
            uint32_t field_ordinal;
        } variant_field;
    } immediate;
    const uint32_t *successors;
    uint32_t successor_count;
} XrVmInstructionView;

typedef struct XrVmContext {
    const XrVmCode *code;
    uint64_t steps;
    uint64_t aggregate_cell_count;
    XrSHA256Context trace;
    struct XrVmAggregateValue **aggregates;
    uint32_t aggregate_count;
    uint32_t aggregate_capacity;
} XrVmContext;

typedef struct XrVmAggregateValue {
    uint16_t type_id;
    uint32_t variant_ordinal;
    XrVmValue *fields;
    uint32_t field_count;
} XrVmAggregateValue;

static XrVmOutcome vm_outcome(XrVmOutcomeKind kind, const XrVmContext *context) {
    XrVmOutcome outcome = {.kind = kind, .steps = context ? context->steps : 0u};
    return outcome;
}

static XrVmOutcome vm_trap(XrVmTrap trap, const XrVmContext *context) {
    XrVmOutcome outcome = vm_outcome(XR_VM_OUTCOME_TRAP, context);
    outcome.trap = trap;
    return outcome;
}

static XrVmValue void_value(void) {
    XrVmValue value = {.kind = XR_VM_VALUE_VOID};
    return value;
}

static bool value_matches_type(const XrValidatedProgram *program, XrVmValue value,
                               uint16_t type_id) {
    switch (type_id) {
        case XR_CORE_TYPE_VOID:
            return value.kind == XR_VM_VALUE_VOID;
        case XR_CORE_TYPE_BOOL:
            return value.kind == XR_VM_VALUE_BOOL;
        case XR_CORE_TYPE_I64:
            return value.kind == XR_VM_VALUE_I64;
        case XR_CORE_TYPE_U32:
            return value.kind == XR_VM_VALUE_U32;
        case XR_CORE_TYPE_ERROR:
            return value.kind == XR_VM_VALUE_ERROR;
        default:
            return xr_validated_program_type(program, type_id) &&
                   value.kind == XR_VM_VALUE_AGGREGATE && value.as.aggregate &&
                   ((const XrVmAggregateValue *) value.as.aggregate)->type_id == type_id;
    }
}

static XrVmAggregateValue *allocate_aggregate(XrVmContext *context, uint16_t type_id,
                                              uint32_t variant_ordinal, uint32_t field_count) {
    if ((uint64_t) field_count >
        (uint64_t) context->code->options.max_value_cells - context->aggregate_cell_count)
        return NULL;
    if (context->aggregate_count == context->aggregate_capacity) {
        uint32_t capacity = context->aggregate_capacity ? context->aggregate_capacity * 2u : 8u;
        if (capacity < context->aggregate_count)
            return NULL;
#if SIZE_MAX < UINT64_MAX
        if ((size_t) capacity > SIZE_MAX / sizeof(*context->aggregates))
            return NULL;
#endif
        XrVmAggregateValue **grown =
            xr_realloc(context->aggregates, (size_t) capacity * sizeof(*context->aggregates));
        if (!grown)
            return NULL;
        context->aggregates = grown;
        context->aggregate_capacity = capacity;
    }
    XrVmAggregateValue *aggregate = xr_calloc(1u, sizeof(*aggregate));
    if (!aggregate)
        return NULL;
    if (field_count != 0u) {
        aggregate->fields = xr_calloc(field_count, sizeof(XrVmValue));
        if (!aggregate->fields) {
            xr_free(aggregate);
            return NULL;
        }
    }
    aggregate->type_id = type_id;
    aggregate->variant_ordinal = variant_ordinal;
    aggregate->field_count = field_count;
    context->aggregates[context->aggregate_count++] = aggregate;
    context->aggregate_cell_count += field_count;
    return aggregate;
}

static void free_aggregates(XrVmContext *context) {
    for (uint32_t index = 0; index < context->aggregate_count; ++index) {
        xr_free(context->aggregates[index]->fields);
        xr_free(context->aggregates[index]);
    }
    xr_free(context->aggregates);
}

static int64_t i64_from_bits(uint64_t bits) {
    if (bits <= (uint64_t) INT64_MAX)
        return (int64_t) bits;
    return -(int64_t) (~bits) - 1;
}

static bool checked_add(int64_t left, int64_t right, int64_t *result) {
    if ((right > 0 && left > INT64_MAX - right) || (right < 0 && left < INT64_MIN - right))
        return false;
    *result = left + right;
    return true;
}

static bool checked_sub(int64_t left, int64_t right, int64_t *result) {
    if ((right < 0 && left > INT64_MAX + right) || (right > 0 && left < INT64_MIN + right))
        return false;
    *result = left - right;
    return true;
}

static bool checked_mul(int64_t left, int64_t right, int64_t *result) {
    if (left == 0 || right == 0) {
        *result = 0;
        return true;
    }
    if ((left == -1 && right == INT64_MIN) || (right == -1 && left == INT64_MIN))
        return false;
    if (left > 0) {
        if ((right > 0 && left > INT64_MAX / right) || (right < 0 && right < INT64_MIN / left))
            return false;
    } else if ((right > 0 && left < INT64_MIN / right) || (right < 0 && left < INT64_MAX / right)) {
        return false;
    }
    *result = left * right;
    return true;
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

static void trace_instruction(XrVmContext *context, uint32_t function_id, uint32_t block_id,
                              uint32_t instruction_id, uint16_t operation_id) {
    hash_u32(&context->trace, function_id);
    hash_u32(&context->trace, block_id);
    hash_u32(&context->trace, instruction_id);
    hash_u32(&context->trace, operation_id);
}

static XrVmInstructionView instruction_view(const XrVmCode *code, uint32_t function_id,
                                            uint32_t block_id, uint32_t instruction_id) {
    XrVmInstructionView view = {0};
    if (code->options.decode_policy == XR_VM_DECODE_FIXED_ROWS) {
        const XrVmFixedInstruction *source =
            &code->fixed_functions[function_id].blocks[block_id].instructions[instruction_id];
        view.operation_id = source->operation_id;
        view.result_type_id = source->result_type_id;
        view.result_id = source->result_id;
        view.operands = source->operands;
        view.operand_count = source->operand_count;
        view.immediate_kind = source->immediate_kind;
        memcpy(&view.immediate, &source->immediate, sizeof(view.immediate));
        view.successors = source->successors;
        view.successor_count = source->successor_count;
    } else {
        const XrValidatedInstruction *source =
            &code->program->functions[function_id].blocks[block_id].instructions[instruction_id];
        view.operation_id = source->operation_id;
        view.result_type_id = source->result_type_id;
        view.result_id = source->result_id;
        view.operands = source->operands;
        view.operand_count = source->operand_count;
        view.immediate_kind = source->immediate_kind;
        memcpy(&view.immediate, &source->immediate, sizeof(view.immediate));
        view.successors = source->successors;
        view.successor_count = source->successor_count;
    }
    return view;
}

static XrVmOutcome execute_function(XrVmContext *context, uint32_t function_id,
                                    const XrVmValue *arguments, uint32_t argument_count,
                                    uint32_t depth) {
    if (depth > context->code->options.max_call_depth)
        return vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
    const XrValidatedFunction *function = &context->code->program->functions[function_id];
    if (argument_count != function->parameter_count)
        return vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
    for (uint32_t index = 0; index < argument_count; ++index) {
        if (!value_matches_type(context->code->program, arguments[index],
                                function->parameter_types[index]))
            return vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
    }

    size_t value_count = function->value_count ? function->value_count : 1u;
    XrVmValue *values = xr_calloc(value_count, sizeof(XrVmValue));
    bool *initialized = xr_calloc(value_count, sizeof(bool));
    uint32_t scratch_count = function->parameter_count;
    for (uint32_t block = 0; block < function->block_count; ++block) {
        const XrValidatedBlock *row = &function->blocks[block];
        if (row->argument_count > scratch_count)
            scratch_count = row->argument_count;
        for (uint32_t instruction = 0; instruction < row->instruction_count; ++instruction) {
            if (row->instructions[instruction].operand_count > scratch_count)
                scratch_count = row->instructions[instruction].operand_count;
        }
    }
    XrVmValue *scratch = xr_calloc(scratch_count ? scratch_count : 1u, sizeof(XrVmValue));
    if (!values || !initialized || !scratch) {
        xr_free(scratch);
        xr_free(initialized);
        xr_free(values);
        return vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
    }

    uint32_t block_id = function->entry_block;
    uint32_t incoming_count = argument_count;
    if (incoming_count != 0u)
        memcpy(scratch, arguments, (size_t) incoming_count * sizeof(XrVmValue));
    XrVmOutcome result = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
    for (;;) {
        const XrValidatedBlock *block = &function->blocks[block_id];
        if (incoming_count != block->argument_count)
            break;
        for (uint32_t argument = 0; argument < block->argument_count; ++argument) {
            uint32_t value_id = block->argument_ids[argument];
            values[value_id] = scratch[argument];
            initialized[value_id] = true;
        }
        bool transferred = false;
        for (uint32_t instruction_id = 0; instruction_id < block->instruction_count;
             ++instruction_id) {
            XrVmInstructionView instruction =
                instruction_view(context->code, function_id, block_id, instruction_id);
            if (context->steps == context->code->options.max_steps) {
                result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                goto done;
            }
            ++context->steps;
            trace_instruction(context, function_id, block_id, instruction_id,
                              instruction.operation_id);
            for (uint32_t operand = 0; operand < instruction.operand_count; ++operand) {
                if (!initialized[instruction.operands[operand]])
                    goto done;
            }
            XrVmValue produced = void_value();
            bool has_result = instruction.result_id != XR_PROGRAM_LOCATION_NONE;
            switch (instruction.operation_id) {
                case XR_CORE_OP_CORE_CONSTANT_I64: {
                    const XrValidatedConstant *constant =
                        &context->code->program->constants[instruction.immediate.constant_id];
                    produced.kind = XR_VM_VALUE_I64;
                    produced.as.i64 = constant->value.i64;
                    break;
                }
                case XR_CORE_OP_CORE_CONSTANT_BOOL: {
                    const XrValidatedConstant *constant =
                        &context->code->program->constants[instruction.immediate.constant_id];
                    produced.kind = XR_VM_VALUE_BOOL;
                    produced.as.boolean = constant->value.boolean;
                    break;
                }
                case XR_CORE_OP_CORE_ADD_I64:
                case XR_CORE_OP_CORE_SUB_I64:
                case XR_CORE_OP_CORE_MUL_I64: {
                    int64_t left = values[instruction.operands[0]].as.i64;
                    int64_t right = values[instruction.operands[1]].as.i64;
                    int64_t exact = 0;
                    bool valid = instruction.operation_id == XR_CORE_OP_CORE_ADD_I64
                                     ? checked_add(left, right, &exact)
                                 : instruction.operation_id == XR_CORE_OP_CORE_SUB_I64
                                     ? checked_sub(left, right, &exact)
                                     : checked_mul(left, right, &exact);
                    if (instruction.immediate.u32 == 0u && !valid) {
                        result = vm_trap(XR_VM_TRAP_INTEGER_OVERFLOW, context);
                        goto done;
                    }
                    if (instruction.immediate.u32 != 0u) {
                        uint64_t bits = instruction.operation_id == XR_CORE_OP_CORE_ADD_I64
                                            ? (uint64_t) left + (uint64_t) right
                                        : instruction.operation_id == XR_CORE_OP_CORE_SUB_I64
                                            ? (uint64_t) left - (uint64_t) right
                                            : (uint64_t) left * (uint64_t) right;
                        exact = i64_from_bits(bits);
                    }
                    produced.kind = XR_VM_VALUE_I64;
                    produced.as.i64 = exact;
                    break;
                }
                case XR_CORE_OP_CORE_DIV_I64: {
                    int64_t left = values[instruction.operands[0]].as.i64;
                    int64_t right = values[instruction.operands[1]].as.i64;
                    if (right == 0) {
                        result = vm_trap(XR_VM_TRAP_INTEGER_DIVISION_BY_ZERO, context);
                        goto done;
                    }
                    if (left == INT64_MIN && right == -1) {
                        result = vm_trap(XR_VM_TRAP_INTEGER_DIVISION_OVERFLOW, context);
                        goto done;
                    }
                    produced.kind = XR_VM_VALUE_I64;
                    produced.as.i64 = left / right;
                    break;
                }
                case XR_CORE_OP_CORE_COMPARE_I64: {
                    int64_t left = values[instruction.operands[0]].as.i64;
                    int64_t right = values[instruction.operands[1]].as.i64;
                    bool comparison = false;
                    switch (instruction.immediate.u32) {
                        case 0:
                            comparison = left == right;
                            break;
                        case 1:
                            comparison = left != right;
                            break;
                        case 2:
                            comparison = left < right;
                            break;
                        case 3:
                            comparison = left <= right;
                            break;
                        case 4:
                            comparison = left > right;
                            break;
                        case 5:
                            comparison = left >= right;
                            break;
                        default:
                            goto done;
                    }
                    produced.kind = XR_VM_VALUE_BOOL;
                    produced.as.boolean = comparison;
                    break;
                }
                case XR_CORE_OP_CORE_BLOCK_ARGUMENT:
                    break;
                case XR_CORE_OP_CORE_BRANCH: {
                    const XrValidatedBlock *target = &function->blocks[instruction.successors[0]];
                    for (uint32_t index = 0; index < target->argument_count; ++index)
                        scratch[index] = values[instruction.operands[index]];
                    incoming_count = target->argument_count;
                    block_id = instruction.successors[0];
                    transferred = true;
                    break;
                }
                case XR_CORE_OP_CORE_CONDITIONAL_BRANCH: {
                    bool condition = values[instruction.operands[0]].as.boolean;
                    uint32_t successor = condition ? 0u : 1u;
                    uint32_t operand =
                        condition ? 1u
                                  : 1u + function->blocks[instruction.successors[0]].argument_count;
                    const XrValidatedBlock *target =
                        &function->blocks[instruction.successors[successor]];
                    for (uint32_t index = 0; index < target->argument_count; ++index)
                        scratch[index] = values[instruction.operands[operand + index]];
                    incoming_count = target->argument_count;
                    block_id = instruction.successors[successor];
                    transferred = true;
                    break;
                }
                case XR_CORE_OP_CORE_RETURN:
                    result = vm_outcome(XR_VM_OUTCOME_RETURN, context);
                    result.value = instruction.operand_count == 0u
                                       ? void_value()
                                       : values[instruction.operands[0]];
                    goto done;
                case XR_CORE_OP_CORE_CALL_SEALED_DIRECT: {
                    for (uint32_t index = 0; index < instruction.operand_count; ++index)
                        scratch[index] = values[instruction.operands[index]];
                    XrVmOutcome nested =
                        execute_function(context, instruction.immediate.function_id, scratch,
                                         instruction.operand_count, depth + 1u);
                    if (nested.kind != XR_VM_OUTCOME_RETURN) {
                        result = nested;
                        goto done;
                    }
                    produced = nested.value;
                    break;
                }
                case XR_CORE_OP_CORE_TRAP:
                    result = vm_trap(XR_VM_TRAP_EXPLICIT, context);
                    goto done;
                case XR_CORE_OP_CORE_ERROR_PUBLISH:
                    result = vm_outcome(XR_VM_OUTCOME_ERROR, context);
                    result.error = values[instruction.operands[0]].as.error;
                    goto done;
                case XR_CORE_OP_CORE_TARGET_POINTER_WIDTH:
                    if (context->code->pointer_width != 32u &&
                        context->code->pointer_width != 64u) {
                        result = vm_trap(XR_VM_TRAP_PROFILE_UNAVAILABLE, context);
                        goto done;
                    }
                    produced.kind = XR_VM_VALUE_U32;
                    produced.as.u32 = context->code->pointer_width;
                    break;
                case XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT: {
                    XrVmAggregateValue *aggregate = allocate_aggregate(
                        context, instruction.result_type_id, UINT32_MAX, instruction.operand_count);
                    if (!aggregate) {
                        result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    for (uint32_t field = 0; field < instruction.operand_count; ++field)
                        aggregate->fields[field] = values[instruction.operands[field]];
                    produced.kind = XR_VM_VALUE_AGGREGATE;
                    produced.as.aggregate = aggregate;
                    break;
                }
                case XR_CORE_OP_CORE_AGGREGATE_PROJECT: {
                    const XrVmAggregateValue *aggregate =
                        values[instruction.operands[0]].as.aggregate;
                    produced = aggregate->fields[instruction.immediate.field_ordinal];
                    break;
                }
                case XR_CORE_OP_CORE_AGGREGATE_UPDATE: {
                    const XrVmAggregateValue *source = values[instruction.operands[0]].as.aggregate;
                    XrVmAggregateValue *aggregate = allocate_aggregate(
                        context, instruction.result_type_id, UINT32_MAX, source->field_count);
                    if (!aggregate) {
                        result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    memcpy(aggregate->fields, source->fields,
                           (size_t) source->field_count * sizeof(XrVmValue));
                    aggregate->fields[instruction.immediate.field_ordinal] =
                        values[instruction.operands[1]];
                    produced.kind = XR_VM_VALUE_AGGREGATE;
                    produced.as.aggregate = aggregate;
                    break;
                }
                case XR_CORE_OP_CORE_VARIANT_CONSTRUCT: {
                    XrVmAggregateValue *aggregate = allocate_aggregate(
                        context, instruction.result_type_id, instruction.immediate.variant_ordinal,
                        instruction.operand_count);
                    if (!aggregate) {
                        result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    for (uint32_t field = 0; field < instruction.operand_count; ++field)
                        aggregate->fields[field] = values[instruction.operands[field]];
                    produced.kind = XR_VM_VALUE_AGGREGATE;
                    produced.as.aggregate = aggregate;
                    break;
                }
                case XR_CORE_OP_CORE_VARIANT_TEST: {
                    const XrVmAggregateValue *aggregate =
                        values[instruction.operands[0]].as.aggregate;
                    produced.kind = XR_VM_VALUE_BOOL;
                    produced.as.boolean =
                        aggregate->variant_ordinal == instruction.immediate.variant_ordinal;
                    break;
                }
                case XR_CORE_OP_CORE_VARIANT_PROJECT: {
                    const XrVmAggregateValue *aggregate =
                        values[instruction.operands[0]].as.aggregate;
                    if (aggregate->variant_ordinal !=
                        instruction.immediate.variant_field.variant_ordinal) {
                        result = vm_trap(XR_VM_TRAP_VARIANT_TAG_MISMATCH, context);
                        goto done;
                    }
                    produced = aggregate->fields[instruction.immediate.variant_field.field_ordinal];
                    break;
                }
                default:
                    goto done;
            }
            if (has_result) {
                values[instruction.result_id] = produced;
                initialized[instruction.result_id] = true;
            }
            if (transferred)
                break;
        }
        if (!transferred)
            break;
    }

done:
    result.steps = context->steps;
    xr_free(scratch);
    xr_free(initialized);
    xr_free(values);
    return result;
}

static void fixed_view_free(XrVmCode *code) {
    if (!code || !code->fixed_functions)
        return;
    for (uint32_t function = 0; function < code->program->function_count; ++function) {
        XrVmFixedFunction *fixed = &code->fixed_functions[function];
        for (uint32_t block = 0; block < fixed->block_count; ++block)
            xr_free(fixed->blocks[block].instructions);
        xr_free(fixed->blocks);
    }
    xr_free(code->fixed_functions);
    code->fixed_functions = NULL;
}

static bool fixed_view_build(XrVmCode *code) {
    code->fixed_functions = xr_calloc(code->program->function_count, sizeof(XrVmFixedFunction));
    if (!code->fixed_functions)
        return false;
    code->private_size = (size_t) code->program->function_count * sizeof(XrVmFixedFunction);
    for (uint32_t function = 0; function < code->program->function_count; ++function) {
        const XrValidatedFunction *source_function = &code->program->functions[function];
        XrVmFixedFunction *destination_function = &code->fixed_functions[function];
        destination_function->block_count = source_function->block_count;
        destination_function->blocks =
            xr_calloc(source_function->block_count, sizeof(XrVmFixedBlock));
        if (!destination_function->blocks)
            return false;
        code->private_size += (size_t) source_function->block_count * sizeof(XrVmFixedBlock);
        for (uint32_t block = 0; block < source_function->block_count; ++block) {
            const XrValidatedBlock *source_block = &source_function->blocks[block];
            XrVmFixedBlock *destination_block = &destination_function->blocks[block];
            destination_block->instruction_count = source_block->instruction_count;
            destination_block->instructions =
                xr_calloc(source_block->instruction_count, sizeof(XrVmFixedInstruction));
            if (!destination_block->instructions)
                return false;
            code->private_size +=
                (size_t) source_block->instruction_count * sizeof(XrVmFixedInstruction);
            for (uint32_t instruction = 0; instruction < source_block->instruction_count;
                 ++instruction) {
                const XrValidatedInstruction *source = &source_block->instructions[instruction];
                XrVmFixedInstruction *destination = &destination_block->instructions[instruction];
                destination->operation_id = source->operation_id;
                destination->result_type_id = source->result_type_id;
                destination->result_id = source->result_id;
                destination->operands = source->operands;
                destination->operand_count = source->operand_count;
                destination->immediate_kind = source->immediate_kind;
                memcpy(&destination->immediate, &source->immediate, sizeof(destination->immediate));
                destination->successors = source->successors;
                destination->successor_count = source->successor_count;
            }
        }
    }
    return true;
}

static void compute_private_digest(XrVmCode *code) {
    static const uint8_t domain[] = "xray-private-vm-code-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    xr_sha256_update(&context, code->cache_key.execution_id.bytes,
                     sizeof(code->cache_key.execution_id.bytes));
    hash_u64(&context, code->cache_key.generation);
    xr_sha256_update(&context, (const uint8_t *) XR_VM_BUILD_ID, sizeof(XR_VM_BUILD_ID));
    xr_sha256_update(&context, &code->options.decode_policy, sizeof(code->options.decode_policy));
    xr_sha256_update(&context, &code->options.quickening_policy,
                     sizeof(code->options.quickening_policy));
    hash_u64(&context, (uint64_t) code->private_size);
    xr_sha256_final(&context, code->private_digest.bytes);
}

XrVmCodeOptions xr_vm_code_default_options(void) {
    XrVmCodeOptions options = {
        .schema_version = XR_VM_CODE_OPTIONS_SCHEMA_VERSION,
        .decode_policy = XR_VM_DECODE_BASELINE_VIEW,
        .quickening_policy = XR_VM_QUICKENING_NONE,
        .max_steps = UINT64_C(1000000),
        .max_value_cells = UINT32_C(1048576),
        .max_call_depth = 1024u,
    };
    return options;
}

XrVmCodeStatus xr_vm_code_build(XrInstance *instance, const XrVmCodeOptions *options,
                                XrVmCode **code_out, XrVmCodeDiagnostic *diagnostic_out) {
    if (code_out)
        *code_out = NULL;
    if (diagnostic_out)
        memset(diagnostic_out, 0, sizeof(*diagnostic_out));
    XrVmCodeOptions selected = options ? *options : xr_vm_code_default_options();
    if (!instance || !code_out || selected.schema_version != XR_VM_CODE_OPTIONS_SCHEMA_VERSION ||
        selected.reserved16 != 0u || selected.max_steps == 0u || selected.max_value_cells == 0u ||
        selected.max_call_depth == 0u) {
        if (diagnostic_out)
            diagnostic_out->status = XR_VM_CODE_INVALID_INPUT;
        return XR_VM_CODE_INVALID_INPUT;
    }
    if ((selected.decode_policy != XR_VM_DECODE_BASELINE_VIEW &&
         selected.decode_policy != XR_VM_DECODE_FIXED_ROWS) ||
        selected.quickening_policy != XR_VM_QUICKENING_NONE) {
        if (diagnostic_out)
            diagnostic_out->status = XR_VM_CODE_POLICY_REJECTED;
        return XR_VM_CODE_POLICY_REJECTED;
    }
    if (!xr_execution_instance_pin(instance)) {
        if (diagnostic_out)
            diagnostic_out->status = XR_VM_CODE_INSTANCE_UNAVAILABLE;
        return XR_VM_CODE_INSTANCE_UNAVAILABLE;
    }
    const XrValidatedProgram *program = xr_execution_instance_program(instance);
    const XrTargetProfile *profile = xr_execution_instance_profile(instance);
    const XrBoundaryAbi *boundary = profile ? xr_target_profile_boundary_abi(profile) : NULL;
    if (!program || !boundary) {
        xr_execution_instance_unpin(instance);
        if (diagnostic_out)
            diagnostic_out->status = XR_VM_CODE_INSTANCE_UNAVAILABLE;
        return XR_VM_CODE_INSTANCE_UNAVAILABLE;
    }
    XrVmCode *code = xr_calloc(1u, sizeof(XrVmCode));
    if (!code) {
        xr_execution_instance_unpin(instance);
        if (diagnostic_out)
            diagnostic_out->status = XR_VM_CODE_OUT_OF_MEMORY;
        return XR_VM_CODE_OUT_OF_MEMORY;
    }
    code->program = xr_validated_program_retain(program);
    code->cache_key = xr_execution_instance_cache_key(instance);
    code->options = selected;
    code->pointer_width = (uint32_t) boundary->pointer_size * 8u;
    if (selected.decode_policy == XR_VM_DECODE_FIXED_ROWS && !fixed_view_build(code)) {
        xr_vm_code_free(code);
        xr_execution_instance_unpin(instance);
        if (diagnostic_out)
            diagnostic_out->status = XR_VM_CODE_OUT_OF_MEMORY;
        return XR_VM_CODE_OUT_OF_MEMORY;
    }
    compute_private_digest(code);
    xr_execution_instance_unpin(instance);
    *code_out = code;
    return XR_VM_CODE_OK;
}

void xr_vm_code_free(XrVmCode *code) {
    if (!code)
        return;
    fixed_view_free(code);
    xr_validated_program_free(code->program);
    xr_free(code);
}

bool xr_vm_code_matches_instance(const XrVmCode *code, const XrInstance *instance) {
    if (!code || !instance)
        return false;
    XrExecutionCacheKey key = xr_execution_instance_cache_key(instance);
    return key.generation == code->cache_key.generation &&
           xr_fingerprint_equal(key.execution_id, code->cache_key.execution_id);
}

XrExecutionCacheKey xr_vm_code_cache_key(const XrVmCode *code) {
    XrExecutionCacheKey key = {0};
    return code ? code->cache_key : key;
}

XrFingerprint xr_vm_code_private_digest(const XrVmCode *code) {
    XrFingerprint digest = {{0}};
    return code ? code->private_digest : digest;
}

size_t xr_vm_code_private_size(const XrVmCode *code) {
    return code ? code->private_size : 0u;
}

XrVmDecodePolicy xr_vm_code_decode_policy(const XrVmCode *code) {
    return code ? (XrVmDecodePolicy) code->options.decode_policy : XR_VM_DECODE_INVALID;
}

XrVmOutcome xr_vm_code_execute(const XrVmCode *code, XrInstance *instance, uint32_t function_id,
                               const XrVmValue *arguments, uint32_t argument_count) {
    XrVmContext context = {.code = code};
    if (!code || !instance || function_id >= code->program->function_count ||
        (argument_count != 0u && !arguments))
        return vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, &context);
    if (!xr_vm_code_matches_instance(code, instance) || !xr_execution_instance_pin(instance))
        return vm_outcome(XR_VM_OUTCOME_STALE_CODE, &context);
    static const uint8_t trace_domain[] = "xray-vm-logical-trace-v1\0";
    xr_sha256_init(&context.trace);
    xr_sha256_update(&context.trace, trace_domain, sizeof(trace_domain) - 1u);
    xr_sha256_update(&context.trace, code->cache_key.execution_id.bytes,
                     sizeof(code->cache_key.execution_id.bytes));
    hash_u32(&context.trace, function_id);
    XrVmOutcome outcome = execute_function(&context, function_id, arguments, argument_count, 1u);
    if (outcome.kind == XR_VM_OUTCOME_RETURN && outcome.value.kind == XR_VM_VALUE_AGGREGATE)
        outcome = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, &context);
    hash_u32(&context.trace, (uint32_t) outcome.kind);
    hash_u32(&context.trace, (uint32_t) outcome.trap);
    hash_u32(&context.trace, outcome.error);
    xr_sha256_final(&context.trace, outcome.logical_trace.bytes);
    outcome.steps = context.steps;
    free_aggregates(&context);
    xr_execution_instance_unpin(instance);
    return outcome;
}

const char *xr_vm_code_status_name(XrVmCodeStatus status) {
    switch (status) {
        case XR_VM_CODE_OK:
            return "ok";
        case XR_VM_CODE_INVALID_INPUT:
            return "invalid-input";
        case XR_VM_CODE_INSTANCE_UNAVAILABLE:
            return "instance-unavailable";
        case XR_VM_CODE_POLICY_REJECTED:
            return "policy-rejected";
        case XR_VM_CODE_OUT_OF_MEMORY:
            return "out-of-memory";
        default:
            return "unknown";
    }
}
