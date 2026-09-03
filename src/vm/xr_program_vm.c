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

typedef struct XrVmExistentialValue XrVmExistentialValue;
typedef struct XrVmCallableValue XrVmCallableValue;

typedef struct XrVmFixedInstruction {
    uint16_t operation_id;
    uint16_t result_type_id;
    XrCoreIrValueCategory result_category;
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
        uint16_t type_id;
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
    XrCoreIrValueCategory result_category;
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
        uint16_t type_id;
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
    XrVmExistentialValue **existentials;
    uint32_t existential_count;
    uint32_t existential_capacity;
    XrVmCallableValue **callables;
    uint32_t callable_count;
    uint32_t callable_capacity;
} XrVmContext;

typedef struct XrVmAggregateValue {
    uint16_t type_id;
    uint32_t variant_ordinal;
    XrVmValue *fields;
    uint32_t field_count;
} XrVmAggregateValue;

typedef struct XrVmPlace {
    XrVmValue value;
    bool initialized;
} XrVmPlace;

typedef struct XrVmRuntimeValue {
    XrCoreIrValueCategory category;
    union {
        XrVmValue value;
        XrVmPlace *place;
    } as;
} XrVmRuntimeValue;

struct XrVmExistentialValue {
    uint16_t existential_type_id;
    uint16_t concrete_type_id;
    uint32_t conformance_id;
    XrVmRuntimeValue payload;
    XrVmPlace owned_storage;
};

struct XrVmCallableValue {
    uint16_t callable_type_id;
    uint16_t capture_type_id;
    uint32_t function_id;
    bool has_capture;
    XrVmValue capture;
};

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
        case XR_CORE_TYPE_PANIC_INFO:
            return value.kind == XR_VM_VALUE_PANIC_INFO;
        default: {
            const XrValidatedType *type = xr_validated_program_type(program, type_id);
            if (!type)
                return false;
            if (type->kind == XR_CORE_IR_TYPE_EXISTENTIAL)
                return value.kind == XR_VM_VALUE_EXISTENTIAL && value.as.existential &&
                       ((const XrVmExistentialValue *) value.as.existential)->existential_type_id ==
                           type_id;
            if (type->kind == XR_CORE_IR_TYPE_CALLABLE)
                return value.kind == XR_VM_VALUE_CALLABLE && value.as.callable &&
                       ((const XrVmCallableValue *) value.as.callable)->callable_type_id == type_id;
            return (type->kind == XR_CORE_IR_TYPE_AGGREGATE ||
                    type->kind == XR_CORE_IR_TYPE_VARIANT) &&
                   value.kind == XR_VM_VALUE_AGGREGATE && value.as.aggregate &&
                   ((const XrVmAggregateValue *) value.as.aggregate)->type_id == type_id;
        }
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

static XrVmExistentialValue *allocate_existential(XrVmContext *context) {
    if (context->aggregate_cell_count == context->code->options.max_value_cells)
        return NULL;
    if (context->existential_count == context->existential_capacity) {
        uint32_t capacity = context->existential_capacity ? context->existential_capacity * 2u : 8u;
        if (capacity < context->existential_count)
            return NULL;
#if SIZE_MAX < UINT64_MAX
        if ((size_t) capacity > SIZE_MAX / sizeof(*context->existentials))
            return NULL;
#endif
        XrVmExistentialValue **grown =
            xr_realloc(context->existentials, (size_t) capacity * sizeof(*context->existentials));
        if (!grown)
            return NULL;
        context->existentials = grown;
        context->existential_capacity = capacity;
    }
    XrVmExistentialValue *value = xr_calloc(1u, sizeof(*value));
    if (!value)
        return NULL;
    context->existentials[context->existential_count++] = value;
    ++context->aggregate_cell_count;
    return value;
}

static XrVmCallableValue *allocate_callable(XrVmContext *context) {
    if (context->aggregate_cell_count == context->code->options.max_value_cells)
        return NULL;
    if (context->callable_count == context->callable_capacity) {
        uint32_t capacity = context->callable_capacity ? context->callable_capacity * 2u : 8u;
        if (capacity < context->callable_count)
            return NULL;
#if SIZE_MAX < UINT64_MAX
        if ((size_t) capacity > SIZE_MAX / sizeof(*context->callables))
            return NULL;
#endif
        XrVmCallableValue **grown =
            xr_realloc(context->callables, (size_t) capacity * sizeof(*context->callables));
        if (!grown)
            return NULL;
        context->callables = grown;
        context->callable_capacity = capacity;
    }
    XrVmCallableValue *value = xr_calloc(1u, sizeof(*value));
    if (!value)
        return NULL;
    context->callables[context->callable_count++] = value;
    ++context->aggregate_cell_count;
    return value;
}

static uint32_t callable_function_id(const XrValidatedProgram *program,
                                     const XrVmCallableValue *carrier) {
    if (!program || !carrier || carrier->function_id >= program->function_count)
        return XR_PROGRAM_LOCATION_NONE;
    const XrValidatedType *callable = xr_validated_program_type(program, carrier->callable_type_id);
    const XrValidatedFunction *target = &program->functions[carrier->function_id];
    if (!callable || callable->kind != XR_CORE_IR_TYPE_CALLABLE ||
        callable->signature_id >= program->signature_count ||
        target->has_receiver != carrier->has_capture ||
        (carrier->has_capture &&
         (target->receiver_mode != XR_PARAM_READ || target->parameter_count == 0u ||
          target->parameter_types[0] != carrier->capture_type_id)))
        return XR_PROGRAM_LOCATION_NONE;
    return carrier->function_id;
}

static uint32_t conformance_id(const XrValidatedProgram *program, uint16_t concrete_type_id,
                               uint32_t interface_id) {
    for (uint32_t index = 0; index < program->conformance_count; ++index)
        if (program->conformances[index].implementor_type_id == concrete_type_id &&
            program->conformances[index].interface_id == interface_id)
            return index;
    return XR_PROGRAM_LOCATION_NONE;
}

static uint32_t witness_function_id(const XrValidatedProgram *program,
                                    const XrVmExistentialValue *carrier, uint32_t slot_ordinal) {
    if (!program || !carrier || carrier->conformance_id >= program->conformance_count)
        return XR_PROGRAM_LOCATION_NONE;
    const XrValidatedConformance *conformance = &program->conformances[carrier->conformance_id];
    const XrValidatedType *existential =
        xr_validated_program_type(program, carrier->existential_type_id);
    if (!existential || existential->kind != XR_CORE_IR_TYPE_EXISTENTIAL ||
        conformance->interface_id != existential->interface_id ||
        conformance->implementor_type_id != carrier->concrete_type_id ||
        slot_ordinal >= conformance->slot_count)
        return XR_PROGRAM_LOCATION_NONE;
    uint32_t function_id = conformance->slot_function_ids[slot_ordinal];
    return function_id < program->function_count ? function_id : XR_PROGRAM_LOCATION_NONE;
}

static bool witness_receiver_argument(const XrVmExistentialValue *carrier,
                                      XrParamMode receiver_mode, XrVmRuntimeValue *argument) {
    if (!carrier || !argument)
        return false;
    if (receiver_mode == XR_PARAM_REF) {
        if (carrier->payload.category != XR_CORE_IR_PLACE || !carrier->payload.as.place ||
            !carrier->payload.as.place->initialized)
            return false;
        *argument = carrier->payload;
        return true;
    }
    if (carrier->payload.category == XR_CORE_IR_PLACE) {
        if (!carrier->payload.as.place || !carrier->payload.as.place->initialized)
            return false;
        argument->category = XR_CORE_IR_VALUE;
        argument->as.value = carrier->payload.as.place->value;
        return true;
    }
    *argument = carrier->payload;
    return argument->category == XR_CORE_IR_VALUE;
}

static bool clone_vm_value(XrVmContext *context, XrVmValue source, uint16_t type_id,
                           XrVmValue *output) {
    const XrValidatedType *type = xr_validated_program_type(context->code->program, type_id);
    if (!type) {
        *output = source;
        return true;
    }
    if (type->kind == XR_CORE_IR_TYPE_EXISTENTIAL) {
        if (source.kind != XR_VM_VALUE_EXISTENTIAL || !source.as.existential)
            return false;
        *output = source;
        return true;
    }
    if (type->kind == XR_CORE_IR_TYPE_CALLABLE) {
        if (source.kind != XR_VM_VALUE_CALLABLE || !source.as.callable)
            return false;
        const XrVmCallableValue *source_callable = source.as.callable;
        XrVmCallableValue *copy = allocate_callable(context);
        if (!copy)
            return false;
        *copy = *source_callable;
        if (copy->has_capture && !clone_vm_value(context, source_callable->capture,
                                                 source_callable->capture_type_id, &copy->capture))
            return false;
        output->kind = XR_VM_VALUE_CALLABLE;
        output->as.callable = copy;
        return true;
    }
    if (type->kind != XR_CORE_IR_TYPE_AGGREGATE && type->kind != XR_CORE_IR_TYPE_VARIANT)
        return false;
    const XrVmAggregateValue *source_aggregate = source.as.aggregate;
    if (!source_aggregate || source_aggregate->type_id != type_id)
        return false;
    XrVmAggregateValue *copy = allocate_aggregate(
        context, type_id, source_aggregate->variant_ordinal, source_aggregate->field_count);
    if (!copy)
        return false;
    for (uint32_t field = 0; field < source_aggregate->field_count; ++field) {
        uint16_t field_type = XR_CORE_TYPE_VOID;
        if (type->kind == XR_CORE_IR_TYPE_AGGREGATE) {
            if (field >= type->field_count)
                return false;
            field_type = type->field_types[field];
        } else {
            if (source_aggregate->variant_ordinal >= type->variant_count ||
                field >= type->variants[source_aggregate->variant_ordinal].payload_count)
                return false;
            field_type = type->variants[source_aggregate->variant_ordinal].payload_types[field];
        }
        if (!clone_vm_value(context, source_aggregate->fields[field], field_type,
                            &copy->fields[field]))
            return false;
    }
    output->kind = XR_VM_VALUE_AGGREGATE;
    output->as.aggregate = copy;
    return true;
}

static void free_aggregates(XrVmContext *context) {
    for (uint32_t index = 0; index < context->aggregate_count; ++index) {
        xr_free(context->aggregates[index]->fields);
        xr_free(context->aggregates[index]);
    }
    xr_free(context->aggregates);
    for (uint32_t index = 0; index < context->existential_count; ++index)
        xr_free(context->existentials[index]);
    xr_free(context->existentials);
    for (uint32_t index = 0; index < context->callable_count; ++index)
        xr_free(context->callables[index]);
    xr_free(context->callables);
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
        view.result_category = source->result_category;
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
        view.result_category = source->result_category;
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
                                    const XrVmRuntimeValue *arguments, uint32_t argument_count,
                                    uint32_t depth) {
    if (depth > context->code->options.max_call_depth)
        return vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
    const XrValidatedFunction *function = &context->code->program->functions[function_id];
    if (argument_count != function->parameter_count)
        return vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
    for (uint32_t index = 0; index < argument_count; ++index) {
        XrCoreIrValueCategory expected =
            function->parameter_modes[index] == XR_PARAM_REF ? XR_CORE_IR_PLACE : XR_CORE_IR_VALUE;
        if (arguments[index].category != expected ||
            (expected == XR_CORE_IR_PLACE &&
             (!arguments[index].as.place || !arguments[index].as.place->initialized)))
            return vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
        XrVmValue value = expected == XR_CORE_IR_PLACE ? arguments[index].as.place->value
                                                       : arguments[index].as.value;
        if (!value_matches_type(context->code->program, value, function->parameter_types[index]))
            return vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
    }

    size_t value_count = function->value_count ? function->value_count : 1u;
    XrVmRuntimeValue *values = xr_calloc(value_count, sizeof(XrVmRuntimeValue));
    XrVmPlace *places = xr_calloc(value_count, sizeof(XrVmPlace));
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
    XrVmRuntimeValue *scratch =
        xr_calloc(scratch_count ? scratch_count : 1u, sizeof(XrVmRuntimeValue));
    if (!values || !places || !initialized || !scratch) {
        xr_free(scratch);
        xr_free(initialized);
        xr_free(places);
        xr_free(values);
        return vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
    }

    uint32_t block_id = function->entry_block;
    uint32_t incoming_count = argument_count;
    if (incoming_count != 0u)
        memcpy(scratch, arguments, (size_t) incoming_count * sizeof(XrVmRuntimeValue));
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
            XrVmRuntimeValue produced = {
                .category = XR_CORE_IR_VALUE,
                .as.value = void_value(),
            };
            bool has_result = instruction.result_id != XR_PROGRAM_LOCATION_NONE;
            switch (instruction.operation_id) {
                case XR_CORE_OP_CORE_CONSTANT_I64: {
                    const XrValidatedConstant *constant =
                        &context->code->program->constants[instruction.immediate.constant_id];
                    produced.as.value.kind = XR_VM_VALUE_I64;
                    produced.as.value.as.i64 = constant->value.i64;
                    break;
                }
                case XR_CORE_OP_CORE_CONSTANT_BOOL: {
                    const XrValidatedConstant *constant =
                        &context->code->program->constants[instruction.immediate.constant_id];
                    produced.as.value.kind = XR_VM_VALUE_BOOL;
                    produced.as.value.as.boolean = constant->value.boolean;
                    break;
                }
                case XR_CORE_OP_CORE_ADD_I64:
                case XR_CORE_OP_CORE_SUB_I64:
                case XR_CORE_OP_CORE_MUL_I64: {
                    int64_t left = values[instruction.operands[0]].as.value.as.i64;
                    int64_t right = values[instruction.operands[1]].as.value.as.i64;
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
                    produced.as.value.kind = XR_VM_VALUE_I64;
                    produced.as.value.as.i64 = exact;
                    break;
                }
                case XR_CORE_OP_CORE_DIV_I64: {
                    int64_t left = values[instruction.operands[0]].as.value.as.i64;
                    int64_t right = values[instruction.operands[1]].as.value.as.i64;
                    if (right == 0) {
                        result = vm_trap(XR_VM_TRAP_INTEGER_DIVISION_BY_ZERO, context);
                        goto done;
                    }
                    if (left == INT64_MIN && right == -1) {
                        result = vm_trap(XR_VM_TRAP_INTEGER_DIVISION_OVERFLOW, context);
                        goto done;
                    }
                    produced.as.value.kind = XR_VM_VALUE_I64;
                    produced.as.value.as.i64 = left / right;
                    break;
                }
                case XR_CORE_OP_CORE_COMPARE_I64: {
                    int64_t left = values[instruction.operands[0]].as.value.as.i64;
                    int64_t right = values[instruction.operands[1]].as.value.as.i64;
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
                    produced.as.value.kind = XR_VM_VALUE_BOOL;
                    produced.as.value.as.boolean = comparison;
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
                    bool condition = values[instruction.operands[0]].as.value.as.boolean;
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
                                       : values[instruction.operands[0]].as.value;
                    goto done;
                case XR_CORE_OP_CORE_CALL_SEALED_DIRECT:
                case XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT:
                case XR_CORE_OP_CORE_CALL_WITNESS_DIRECT: {
                    uint32_t target_function =
                        instruction.operation_id == XR_CORE_OP_CORE_CALL_SEALED_DIRECT
                            ? instruction.immediate.function_id
                            : XR_PROGRAM_LOCATION_NONE;
                    uint32_t source_argument = 0u;
                    uint32_t target_argument = 0u;
                    if (instruction.operation_id == XR_CORE_OP_CORE_CALL_WITNESS_DIRECT) {
                        const XrVmExistentialValue *carrier =
                            values[instruction.operands[0]].as.value.as.existential;
                        target_function = witness_function_id(context->code->program, carrier,
                                                              instruction.immediate.u32);
                        if (target_function == XR_PROGRAM_LOCATION_NONE)
                            goto done;
                        if (!witness_receiver_argument(
                                carrier,
                                context->code->program->functions[target_function].receiver_mode,
                                &scratch[0]))
                            goto done;
                        source_argument = 1u;
                        target_argument = 1u;
                    } else if (instruction.operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT) {
                        const XrVmCallableValue *carrier =
                            values[instruction.operands[0]].as.value.as.callable;
                        target_function = callable_function_id(context->code->program, carrier);
                        if (target_function == XR_PROGRAM_LOCATION_NONE)
                            goto done;
                        source_argument = 1u;
                        if (carrier->has_capture) {
                            scratch[0] = (XrVmRuntimeValue) {
                                .category = XR_CORE_IR_VALUE,
                                .as.value = carrier->capture,
                            };
                            target_argument = 1u;
                        }
                    }
                    for (; source_argument < instruction.operand_count;
                         ++source_argument, ++target_argument)
                        scratch[target_argument] = values[instruction.operands[source_argument]];
                    const XrValidatedFunction *callee =
                        &context->code->program->functions[target_function];
                    XrVmOutcome nested = execute_function(context, target_function, scratch,
                                                          callee->parameter_count, depth + 1u);
                    if (nested.kind != XR_VM_OUTCOME_RETURN) {
                        result = nested;
                        goto done;
                    }
                    produced.as.value = nested.value;
                    break;
                }
                case XR_CORE_OP_CORE_CALL_SEALED_INVOKE:
                case XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE:
                case XR_CORE_OP_CORE_CALL_WITNESS_INVOKE: {
                    uint32_t target_function =
                        instruction.operation_id == XR_CORE_OP_CORE_CALL_SEALED_INVOKE
                            ? instruction.immediate.function_id
                            : XR_PROGRAM_LOCATION_NONE;
                    uint32_t source_argument = 0u;
                    uint32_t target_argument = 0u;
                    if (instruction.operation_id == XR_CORE_OP_CORE_CALL_WITNESS_INVOKE) {
                        const XrVmExistentialValue *carrier =
                            values[instruction.operands[0]].as.value.as.existential;
                        target_function = witness_function_id(context->code->program, carrier,
                                                              instruction.immediate.u32);
                        if (target_function == XR_PROGRAM_LOCATION_NONE)
                            goto done;
                        if (!witness_receiver_argument(
                                carrier,
                                context->code->program->functions[target_function].receiver_mode,
                                &scratch[0]))
                            goto done;
                        source_argument = 1u;
                        target_argument = 1u;
                    } else if (instruction.operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE) {
                        const XrVmCallableValue *carrier =
                            values[instruction.operands[0]].as.value.as.callable;
                        target_function = callable_function_id(context->code->program, carrier);
                        if (target_function == XR_PROGRAM_LOCATION_NONE)
                            goto done;
                        source_argument = 1u;
                        if (carrier->has_capture) {
                            scratch[0] = (XrVmRuntimeValue) {
                                .category = XR_CORE_IR_VALUE,
                                .as.value = carrier->capture,
                            };
                            target_argument = 1u;
                        }
                    }
                    const XrValidatedFunction *callee =
                        &context->code->program->functions[target_function];
                    for (; target_argument < callee->parameter_count;
                         ++source_argument, ++target_argument)
                        scratch[target_argument] = values[instruction.operands[source_argument]];
                    XrVmOutcome nested = execute_function(context, target_function, scratch,
                                                          callee->parameter_count, depth + 1u);
                    uint32_t successor = 0u;
                    uint32_t implicit = 0u;
                    uint32_t operand = callee->parameter_count;
                    if (instruction.operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE &&
                        !callee->has_receiver)
                        ++operand;
                    if (nested.kind == XR_VM_OUTCOME_RETURN) {
                        if (callee->result_type_id != XR_CORE_TYPE_VOID) {
                            scratch[0] = (XrVmRuntimeValue) {
                                .category = XR_CORE_IR_VALUE,
                                .as.value = nested.value,
                            };
                            implicit = 1u;
                        }
                    } else if (nested.kind == XR_VM_OUTCOME_ERROR) {
                        successor = 1u;
                        operand += function->blocks[instruction.successors[0]].argument_count -
                                   (callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u);
                        scratch[0] = (XrVmRuntimeValue) {
                            .category = XR_CORE_IR_VALUE,
                            .as.value = nested.error_value,
                        };
                        implicit = 1u;
                    } else if (nested.kind == XR_VM_OUTCOME_PANIC) {
                        successor = 1u + (callee->error_type_id == XR_CORE_TYPE_VOID ? 0u : 1u);
                        operand += function->blocks[instruction.successors[0]].argument_count -
                                   (callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u);
                        if (callee->error_type_id != XR_CORE_TYPE_VOID)
                            operand +=
                                function->blocks[instruction.successors[1]].argument_count - 1u;
                        scratch[0] = (XrVmRuntimeValue) {
                            .category = XR_CORE_IR_VALUE,
                            .as.value = nested.panic_value,
                        };
                        implicit = 1u;
                    } else {
                        result = nested;
                        goto done;
                    }
                    const XrValidatedBlock *target =
                        &function->blocks[instruction.successors[successor]];
                    for (uint32_t index = implicit; index < target->argument_count; ++index)
                        scratch[index] = values[instruction.operands[operand + index - implicit]];
                    incoming_count = target->argument_count;
                    block_id = instruction.successors[successor];
                    transferred = true;
                    break;
                }
                case XR_CORE_OP_CORE_TRAP:
                    result = vm_trap(XR_VM_TRAP_EXPLICIT, context);
                    goto done;
                case XR_CORE_OP_CORE_ERROR_PUBLISH:
                    result = vm_outcome(XR_VM_OUTCOME_ERROR, context);
                    result.error_value = values[instruction.operands[0]].as.value;
                    goto done;
                case XR_CORE_OP_CORE_PANIC_PUBLISH:
                    result = vm_outcome(XR_VM_OUTCOME_PANIC, context);
                    result.panic_value = values[instruction.operands[0]].as.value;
                    goto done;
                case XR_CORE_OP_CORE_TARGET_POINTER_WIDTH:
                    if (context->code->pointer_width != 32u &&
                        context->code->pointer_width != 64u) {
                        result = vm_trap(XR_VM_TRAP_PROFILE_UNAVAILABLE, context);
                        goto done;
                    }
                    produced.as.value.kind = XR_VM_VALUE_U32;
                    produced.as.value.as.u32 = context->code->pointer_width;
                    break;
                case XR_CORE_OP_CORE_CALLABLE_PACK: {
                    XrVmCallableValue *carrier = allocate_callable(context);
                    if (!carrier) {
                        result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    carrier->callable_type_id = instruction.result_type_id;
                    carrier->function_id = instruction.immediate.function_id;
                    carrier->has_capture = instruction.operand_count != 0u;
                    if (carrier->has_capture) {
                        uint32_t capture_value = instruction.operands[0];
                        carrier->capture_type_id = function->value_types[capture_value];
                        carrier->capture = values[capture_value].as.value;
                    }
                    produced.as.value.kind = XR_VM_VALUE_CALLABLE;
                    produced.as.value.as.callable = carrier;
                    break;
                }
                case XR_CORE_OP_CORE_OWNER_COPY:
                    if (!clone_vm_value(context, values[instruction.operands[0]].as.value,
                                        instruction.result_type_id, &produced.as.value)) {
                        result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    break;
                case XR_CORE_OP_CORE_OWNER_MOVE:
                    produced.as.value = values[instruction.operands[0]].as.value;
                    break;
                case XR_CORE_OP_CORE_OWNER_DROP:
                    break;
                case XR_CORE_OP_CORE_PLACE_LOCAL:
                    places[instruction.result_id].value = values[instruction.operands[0]].as.value;
                    places[instruction.result_id].initialized = true;
                    produced.category = XR_CORE_IR_PLACE;
                    produced.as.place = &places[instruction.result_id];
                    break;
                case XR_CORE_OP_CORE_PLACE_LOAD:
                    produced.as.value = values[instruction.operands[0]].as.place->value;
                    break;
                case XR_CORE_OP_CORE_PLACE_STORE:
                    values[instruction.operands[0]].as.place->value =
                        values[instruction.operands[1]].as.value;
                    break;
                case XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT: {
                    XrVmAggregateValue *aggregate = allocate_aggregate(
                        context, instruction.result_type_id, UINT32_MAX, instruction.operand_count);
                    if (!aggregate) {
                        result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    for (uint32_t field = 0; field < instruction.operand_count; ++field)
                        aggregate->fields[field] = values[instruction.operands[field]].as.value;
                    produced.as.value.kind = XR_VM_VALUE_AGGREGATE;
                    produced.as.value.as.aggregate = aggregate;
                    break;
                }
                case XR_CORE_OP_CORE_AGGREGATE_PROJECT: {
                    const XrVmAggregateValue *aggregate =
                        values[instruction.operands[0]].as.value.as.aggregate;
                    produced.as.value = aggregate->fields[instruction.immediate.field_ordinal];
                    break;
                }
                case XR_CORE_OP_CORE_AGGREGATE_UPDATE: {
                    const XrVmAggregateValue *source =
                        values[instruction.operands[0]].as.value.as.aggregate;
                    XrVmAggregateValue *aggregate = allocate_aggregate(
                        context, instruction.result_type_id, UINT32_MAX, source->field_count);
                    if (!aggregate) {
                        result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    memcpy(aggregate->fields, source->fields,
                           (size_t) source->field_count * sizeof(XrVmValue));
                    aggregate->fields[instruction.immediate.field_ordinal] =
                        values[instruction.operands[1]].as.value;
                    produced.as.value.kind = XR_VM_VALUE_AGGREGATE;
                    produced.as.value.as.aggregate = aggregate;
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
                        aggregate->fields[field] = values[instruction.operands[field]].as.value;
                    produced.as.value.kind = XR_VM_VALUE_AGGREGATE;
                    produced.as.value.as.aggregate = aggregate;
                    break;
                }
                case XR_CORE_OP_CORE_VARIANT_TEST: {
                    const XrVmAggregateValue *aggregate =
                        values[instruction.operands[0]].as.value.as.aggregate;
                    produced.as.value.kind = XR_VM_VALUE_BOOL;
                    produced.as.value.as.boolean =
                        aggregate->variant_ordinal == instruction.immediate.variant_ordinal;
                    break;
                }
                case XR_CORE_OP_CORE_VARIANT_PROJECT: {
                    const XrVmAggregateValue *aggregate =
                        values[instruction.operands[0]].as.value.as.aggregate;
                    if (aggregate->variant_ordinal !=
                        instruction.immediate.variant_field.variant_ordinal) {
                        result = vm_trap(XR_VM_TRAP_VARIANT_TAG_MISMATCH, context);
                        goto done;
                    }
                    produced.as.value =
                        aggregate->fields[instruction.immediate.variant_field.field_ordinal];
                    break;
                }
                case XR_CORE_OP_CORE_EXISTENTIAL_PACK: {
                    const XrValidatedType *existential = xr_validated_program_type(
                        context->code->program, instruction.result_type_id);
                    uint16_t concrete_type = function->value_types[instruction.operands[0]];
                    XrVmExistentialValue *carrier = allocate_existential(context);
                    uint32_t conformance =
                        existential ? conformance_id(context->code->program, concrete_type,
                                                     existential->interface_id)
                                    : XR_PROGRAM_LOCATION_NONE;
                    if (!carrier || conformance == XR_PROGRAM_LOCATION_NONE) {
                        result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    carrier->existential_type_id = instruction.result_type_id;
                    carrier->concrete_type_id = concrete_type;
                    carrier->conformance_id = conformance;
                    if (existential->interface_use_kind ==
                        XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE) {
                        carrier->owned_storage.value = values[instruction.operands[0]].as.value;
                        carrier->owned_storage.initialized = true;
                        carrier->payload.category = XR_CORE_IR_PLACE;
                        carrier->payload.as.place = &carrier->owned_storage;
                    } else {
                        carrier->payload = values[instruction.operands[0]];
                    }
                    produced.as.value.kind = XR_VM_VALUE_EXISTENTIAL;
                    produced.as.value.as.existential = carrier;
                    break;
                }
                case XR_CORE_OP_CORE_EXISTENTIAL_TEST: {
                    const XrVmExistentialValue *carrier =
                        values[instruction.operands[0]].as.value.as.existential;
                    produced.as.value.kind = XR_VM_VALUE_BOOL;
                    produced.as.value.as.boolean =
                        carrier->concrete_type_id == instruction.immediate.type_id;
                    break;
                }
                case XR_CORE_OP_CORE_EXISTENTIAL_PROJECT: {
                    const XrVmExistentialValue *carrier =
                        values[instruction.operands[0]].as.value.as.existential;
                    const XrValidatedType *existential = xr_validated_program_type(
                        context->code->program, carrier->existential_type_id);
                    if (existential && existential->interface_use_kind ==
                                           XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE) {
                        produced.category = XR_CORE_IR_VALUE;
                        produced.as.value = carrier->owned_storage.value;
                    } else {
                        produced = carrier->payload;
                    }
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
    xr_free(places);
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
                destination->result_category = source->result_category;
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
    const XrValidatedFunction *function = &code->program->functions[function_id];
    XrVmRuntimeValue *runtime_arguments =
        xr_calloc(argument_count ? argument_count : 1u, sizeof(XrVmRuntimeValue));
    if (!runtime_arguments) {
        free_aggregates(&context);
        xr_execution_instance_unpin(instance);
        return vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, &context);
    }
    for (uint32_t index = 0; index < argument_count; ++index) {
        if (index >= function->parameter_count ||
            function->parameter_modes[index] == XR_PARAM_REF) {
            xr_free(runtime_arguments);
            free_aggregates(&context);
            xr_execution_instance_unpin(instance);
            return vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, &context);
        }
        runtime_arguments[index].category = XR_CORE_IR_VALUE;
        runtime_arguments[index].as.value = arguments[index];
    }
    XrVmOutcome outcome =
        execute_function(&context, function_id, runtime_arguments, argument_count, 1u);
    xr_free(runtime_arguments);
    if (outcome.kind == XR_VM_OUTCOME_RETURN && (outcome.value.kind == XR_VM_VALUE_AGGREGATE ||
                                                 outcome.value.kind == XR_VM_VALUE_EXISTENTIAL ||
                                                 outcome.value.kind == XR_VM_VALUE_CALLABLE))
        outcome = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, &context);
    if (outcome.kind == XR_VM_OUTCOME_ERROR &&
        (outcome.error_value.kind == XR_VM_VALUE_AGGREGATE ||
         outcome.error_value.kind == XR_VM_VALUE_EXISTENTIAL ||
         outcome.error_value.kind == XR_VM_VALUE_CALLABLE))
        outcome = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, &context);
    if (outcome.kind == XR_VM_OUTCOME_PANIC && outcome.panic_value.kind != XR_VM_VALUE_PANIC_INFO)
        outcome = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, &context);
    hash_u32(&context.trace, (uint32_t) outcome.kind);
    hash_u32(&context.trace, (uint32_t) outcome.trap);
    hash_u32(&context.trace, (uint32_t) outcome.error_value.kind);
    if (outcome.error_value.kind == XR_VM_VALUE_ERROR)
        hash_u32(&context.trace, outcome.error_value.as.error);
    hash_u32(&context.trace, (uint32_t) outcome.panic_value.kind);
    if (outcome.panic_value.kind == XR_VM_VALUE_PANIC_INFO)
        hash_u32(&context.trace, outcome.panic_value.as.panic_info);
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
