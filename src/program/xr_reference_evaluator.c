/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_reference_evaluator.c - Simple host-independent CoreSpec evaluator
 */

#include "xr_reference_evaluator.h"

#include "../base/xmalloc.h"
#include "../core/xr_core_spec_gen.h"
#include "../runtime/abi/xr_target_machine_facts.h"
#include "xr_validated_program_internal.h"

#include <limits.h>
#include <string.h>

typedef struct XrReferenceExistentialValue XrReferenceExistentialValue;
typedef struct XrReferenceCallableValue XrReferenceCallableValue;

typedef struct EvalContext {
    const XrValidatedProgram *program;
    const XrReferenceProviderBinding *providers;
    XrReferenceProfile profile;
    XrReferenceBudget budget;
    uint64_t steps;
    uint64_t aggregate_cell_count;
    struct XrReferenceAggregateValue **aggregates;
    uint32_t aggregate_count;
    uint32_t aggregate_capacity;
    XrReferenceExistentialValue **existentials;
    uint32_t existential_count;
    uint32_t existential_capacity;
    XrReferenceCallableValue **callables;
    uint32_t callable_count;
    uint32_t callable_capacity;
} EvalContext;

typedef struct XrReferenceAggregateValue {
    uint16_t type_id;
    uint32_t variant_ordinal;
    XrReferenceValue *fields;
    uint32_t field_count;
} XrReferenceAggregateValue;

typedef struct EvalPlace {
    XrReferenceValue value;
    XrReferenceValue *alias;
    bool initialized;
} EvalPlace;

typedef struct EvalRuntimeValue {
    XrCoreIrValueCategory category;
    union {
        XrReferenceValue value;
        EvalPlace *place;
    } as;
} EvalRuntimeValue;

struct XrReferenceExistentialValue {
    uint16_t existential_type_id;
    uint16_t concrete_type_id;
    uint32_t conformance_id;
    EvalRuntimeValue payload;
    EvalPlace owned_storage;
};

struct XrReferenceCallableValue {
    uint16_t callable_type_id;
    uint16_t capture_type_id;
    uint32_t function_id;
    bool has_capture;
    XrReferenceValue capture;
};

struct XrReferenceExecution {
    XrExecutionLease lease;
    XrValidatedProgram *program;
    XrReferenceBudget budget;
    XrReferenceValue *values;
    bool *initialized;
    uint32_t function_id;
    uint32_t block_id;
    uint32_t instruction_id;
    uint32_t state_id;
    uint64_t steps;
    struct XrReferenceExecution *child;
    bool owns_lease;
    bool finished;
};

static XrReferenceOutcome outcome(XrReferenceOutcomeKind kind, EvalContext *context) {
    XrReferenceOutcome result = {.kind = kind, .steps = context->steps};
    return result;
}

static XrReferenceOutcome trap_outcome(EvalContext *context, XrReferenceTrap trap) {
    XrReferenceOutcome result = outcome(XR_REFERENCE_OUTCOME_TRAP, context);
    result.trap = trap;
    return result;
}

static size_t format_i64_line(int64_t value, uint8_t output[22]) {
    uint8_t reverse[20];
    size_t count = 0u;
    uint64_t magnitude = value < 0 ? UINT64_C(0) - (uint64_t) value : (uint64_t) value;
    do {
        reverse[count++] = (uint8_t) ('0' + magnitude % UINT64_C(10));
        magnitude /= UINT64_C(10);
    } while (magnitude != 0u);
    size_t cursor = 0u;
    if (value < 0)
        output[cursor++] = (uint8_t) '-';
    while (count != 0u)
        output[cursor++] = reverse[--count];
    output[cursor++] = (uint8_t) '\n';
    return cursor;
}

static XrReferenceValue void_value(void) {
    XrReferenceValue value = {.kind = XR_REFERENCE_VALUE_VOID};
    return value;
}

static XrReferenceValue *eval_place_value(EvalPlace *place) {
    return place ? (place->alias ? place->alias : &place->value) : NULL;
}

static const XrReferenceValue *eval_place_value_const(const EvalPlace *place) {
    return place ? (place->alias ? place->alias : &place->value) : NULL;
}

static bool reference_value_matches_type(const XrValidatedProgram *program, XrReferenceValue value,
                                         uint16_t type_id) {
    switch (type_id) {
        case XR_CORE_TYPE_VOID:
            return value.kind == XR_REFERENCE_VALUE_VOID;
        case XR_CORE_TYPE_BOOL:
            return value.kind == XR_REFERENCE_VALUE_BOOL;
        case XR_CORE_TYPE_I64:
            return value.kind == XR_REFERENCE_VALUE_I64;
        case XR_CORE_TYPE_U32:
            return value.kind == XR_REFERENCE_VALUE_U32;
        case XR_CORE_TYPE_U16:
            return value.kind == XR_REFERENCE_VALUE_U16;
        case XR_CORE_TYPE_TARGET_OS:
            return value.kind == XR_REFERENCE_VALUE_TARGET_OS;
        case XR_CORE_TYPE_TARGET_ARCH:
            return value.kind == XR_REFERENCE_VALUE_TARGET_ARCH;
        case XR_CORE_TYPE_TARGET_ABI:
            return value.kind == XR_REFERENCE_VALUE_TARGET_ABI;
        case XR_CORE_TYPE_TARGET_ENDIAN:
            return value.kind == XR_REFERENCE_VALUE_TARGET_ENDIAN;
        case XR_CORE_TYPE_ERROR:
            return value.kind == XR_REFERENCE_VALUE_ERROR;
        case XR_CORE_TYPE_PANIC_INFO:
            return value.kind == XR_REFERENCE_VALUE_PANIC_INFO;
        default: {
            const XrValidatedType *type = xr_validated_program_type(program, type_id);
            if (!type)
                return false;
            if (type->kind == XR_CORE_IR_TYPE_EXISTENTIAL)
                return value.kind == XR_REFERENCE_VALUE_EXISTENTIAL && value.as.existential &&
                       ((const XrReferenceExistentialValue *) value.as.existential)
                               ->existential_type_id == type_id;
            if (type->kind == XR_CORE_IR_TYPE_CALLABLE)
                return value.kind == XR_REFERENCE_VALUE_CALLABLE && value.as.callable &&
                       ((const XrReferenceCallableValue *) value.as.callable)->callable_type_id ==
                           type_id;
            return (type->kind == XR_CORE_IR_TYPE_AGGREGATE ||
                    type->kind == XR_CORE_IR_TYPE_VARIANT) &&
                   value.kind == XR_REFERENCE_VALUE_AGGREGATE && value.as.aggregate &&
                   ((const XrReferenceAggregateValue *) value.as.aggregate)->type_id == type_id;
        }
    }
}

static XrReferenceAggregateValue *allocate_aggregate(EvalContext *context, uint16_t type_id,
                                                     uint32_t variant_ordinal,
                                                     uint32_t field_count) {
    if ((uint64_t) field_count > context->budget.max_value_cells - context->aggregate_cell_count)
        return NULL;
    if (context->aggregate_count == context->aggregate_capacity) {
        uint32_t capacity = context->aggregate_capacity ? context->aggregate_capacity * 2u : 8u;
        if (capacity < context->aggregate_count)
            return NULL;
#if SIZE_MAX < UINT64_MAX
        if ((size_t) capacity > SIZE_MAX / sizeof(*context->aggregates))
            return NULL;
#endif
        XrReferenceAggregateValue **grown =
            xr_realloc(context->aggregates, (size_t) capacity * sizeof(*context->aggregates));
        if (!grown)
            return NULL;
        context->aggregates = grown;
        context->aggregate_capacity = capacity;
    }
    XrReferenceAggregateValue *aggregate = xr_calloc(1u, sizeof(*aggregate));
    if (!aggregate)
        return NULL;
    if (field_count != 0u) {
        aggregate->fields = xr_calloc(field_count, sizeof(XrReferenceValue));
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

static XrReferenceExistentialValue *allocate_existential(EvalContext *context) {
    if (context->aggregate_cell_count == context->budget.max_value_cells)
        return NULL;
    if (context->existential_count == context->existential_capacity) {
        uint32_t capacity = context->existential_capacity ? context->existential_capacity * 2u : 8u;
        if (capacity < context->existential_count)
            return NULL;
#if SIZE_MAX < UINT64_MAX
        if ((size_t) capacity > SIZE_MAX / sizeof(*context->existentials))
            return NULL;
#endif
        XrReferenceExistentialValue **grown =
            xr_realloc(context->existentials, (size_t) capacity * sizeof(*context->existentials));
        if (!grown)
            return NULL;
        context->existentials = grown;
        context->existential_capacity = capacity;
    }
    XrReferenceExistentialValue *value = xr_calloc(1u, sizeof(*value));
    if (!value)
        return NULL;
    context->existentials[context->existential_count++] = value;
    ++context->aggregate_cell_count;
    return value;
}

static XrReferenceCallableValue *allocate_callable(EvalContext *context) {
    if (context->aggregate_cell_count == context->budget.max_value_cells)
        return NULL;
    if (context->callable_count == context->callable_capacity) {
        uint32_t capacity = context->callable_capacity ? context->callable_capacity * 2u : 8u;
        if (capacity < context->callable_count)
            return NULL;
#if SIZE_MAX < UINT64_MAX
        if ((size_t) capacity > SIZE_MAX / sizeof(*context->callables))
            return NULL;
#endif
        XrReferenceCallableValue **grown =
            xr_realloc(context->callables, (size_t) capacity * sizeof(*context->callables));
        if (!grown)
            return NULL;
        context->callables = grown;
        context->callable_capacity = capacity;
    }
    XrReferenceCallableValue *value = xr_calloc(1u, sizeof(*value));
    if (!value)
        return NULL;
    context->callables[context->callable_count++] = value;
    ++context->aggregate_cell_count;
    return value;
}

static uint32_t callable_function_id(const XrValidatedProgram *program,
                                     const XrReferenceCallableValue *carrier) {
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
                                    const XrReferenceExistentialValue *carrier,
                                    uint32_t slot_ordinal) {
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

static bool witness_receiver_argument(const XrReferenceExistentialValue *carrier,
                                      XrParamMode receiver_mode, EvalRuntimeValue *argument) {
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
        argument->as.value = *eval_place_value_const(carrier->payload.as.place);
        return true;
    }
    *argument = carrier->payload;
    return argument->category == XR_CORE_IR_VALUE;
}

static bool clone_reference_value(EvalContext *context, XrReferenceValue source, uint16_t type_id,
                                  XrReferenceValue *output) {
    const XrValidatedType *type = xr_validated_program_type(context->program, type_id);
    if (!type) {
        *output = source;
        return true;
    }
    if (type->kind == XR_CORE_IR_TYPE_EXISTENTIAL) {
        if (source.kind != XR_REFERENCE_VALUE_EXISTENTIAL || !source.as.existential)
            return false;
        *output = source;
        return true;
    }
    if (type->kind == XR_CORE_IR_TYPE_CALLABLE) {
        if (source.kind != XR_REFERENCE_VALUE_CALLABLE || !source.as.callable)
            return false;
        const XrReferenceCallableValue *source_callable = source.as.callable;
        XrReferenceCallableValue *copy = allocate_callable(context);
        if (!copy)
            return false;
        *copy = *source_callable;
        if (copy->has_capture &&
            !clone_reference_value(context, source_callable->capture,
                                   source_callable->capture_type_id, &copy->capture))
            return false;
        output->kind = XR_REFERENCE_VALUE_CALLABLE;
        output->as.callable = copy;
        return true;
    }
    if (type->kind != XR_CORE_IR_TYPE_AGGREGATE && type->kind != XR_CORE_IR_TYPE_VARIANT)
        return false;
    const XrReferenceAggregateValue *source_aggregate = source.as.aggregate;
    if (!source_aggregate || source_aggregate->type_id != type_id)
        return false;
    XrReferenceAggregateValue *copy = allocate_aggregate(
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
        if (!clone_reference_value(context, source_aggregate->fields[field], field_type,
                                   &copy->fields[field]))
            return false;
    }
    output->kind = XR_REFERENCE_VALUE_AGGREGATE;
    output->as.aggregate = copy;
    return true;
}

static void dispose_detached_reference_value(XrReferenceValue *value) {
    if (!value || value->kind != XR_REFERENCE_VALUE_AGGREGATE || !value->as.aggregate)
        return;
    XrReferenceAggregateValue *aggregate =
        (XrReferenceAggregateValue *) (void *) value->as.aggregate;
    for (uint32_t field = 0u; field < aggregate->field_count; ++field)
        dispose_detached_reference_value(&aggregate->fields[field]);
    xr_free(aggregate->fields);
    xr_free(aggregate);
    *value = void_value();
}

static bool detach_reference_value(XrReferenceValue source, XrReferenceValue *output) {
    if (!output)
        return false;
    *output = void_value();
    if (source.kind == XR_REFERENCE_VALUE_EXISTENTIAL ||
        source.kind == XR_REFERENCE_VALUE_CALLABLE)
        return false;
    if (source.kind != XR_REFERENCE_VALUE_AGGREGATE) {
        *output = source;
        return true;
    }
    const XrReferenceAggregateValue *source_aggregate = source.as.aggregate;
    if (!source_aggregate ||
        (source_aggregate->field_count != 0u && !source_aggregate->fields))
        return false;
#if SIZE_MAX < UINT64_MAX
    if ((size_t) source_aggregate->field_count > SIZE_MAX / sizeof(*source_aggregate->fields))
        return false;
#endif
    XrReferenceAggregateValue *aggregate = xr_calloc(1u, sizeof(*aggregate));
    if (!aggregate)
        return false;
    if (source_aggregate->field_count != 0u) {
        aggregate->fields =
            xr_calloc(source_aggregate->field_count, sizeof(*aggregate->fields));
        if (!aggregate->fields) {
            xr_free(aggregate);
            return false;
        }
    }
    aggregate->type_id = source_aggregate->type_id;
    aggregate->variant_ordinal = source_aggregate->variant_ordinal;
    aggregate->field_count = source_aggregate->field_count;
    XrReferenceValue detached = {
        .kind = XR_REFERENCE_VALUE_AGGREGATE,
        .as.aggregate = aggregate,
    };
    for (uint32_t field = 0u; field < aggregate->field_count; ++field) {
        if (!detach_reference_value(source_aggregate->fields[field],
                                    &aggregate->fields[field])) {
            dispose_detached_reference_value(&detached);
            return false;
        }
    }
    *output = detached;
    return true;
}

bool xr_reference_value_aggregate_view(const XrReferenceValue *value,
                                       XrReferenceAggregateView *view_out) {
    if (view_out)
        memset(view_out, 0, sizeof(*view_out));
    if (!value || !view_out || value->kind != XR_REFERENCE_VALUE_AGGREGATE ||
        !value->as.aggregate)
        return false;
    const XrReferenceAggregateValue *aggregate = value->as.aggregate;
    *view_out = (XrReferenceAggregateView) {
        .type_id = aggregate->type_id,
        .variant_ordinal = aggregate->variant_ordinal,
        .fields = aggregate->fields,
        .field_count = aggregate->field_count,
    };
    return true;
}

void xr_reference_outcome_dispose(XrReferenceOutcome *outcome) {
    if (!outcome)
        return;
    if (outcome->owns_dynamic_values) {
        dispose_detached_reference_value(&outcome->value);
        dispose_detached_reference_value(&outcome->error_value);
        dispose_detached_reference_value(&outcome->panic_value);
    }
    memset(outcome, 0, sizeof(*outcome));
}

static void free_aggregates(EvalContext *context) {
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
    } else {
        if ((right > 0 && left < INT64_MIN / right) || (right < 0 && left < INT64_MAX / right))
            return false;
    }
    *result = left * right;
    return true;
}

static XrReferenceOutcome evaluate_function(EvalContext *context, uint32_t function_id,
                                            const EvalRuntimeValue *arguments,
                                            uint32_t argument_count, uint32_t depth) {
    if (depth > context->budget.max_call_depth)
        return outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
    const XrValidatedFunction *function = &context->program->functions[function_id];
    if (argument_count != function->parameter_count)
        return outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
    for (uint32_t index = 0; index < argument_count; ++index) {
        XrCoreIrValueCategory expected =
            function->parameter_modes[index] == XR_PARAM_REF ? XR_CORE_IR_PLACE : XR_CORE_IR_VALUE;
        if (arguments[index].category != expected)
            return outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
        if (expected == XR_CORE_IR_PLACE &&
            (!arguments[index].as.place || !arguments[index].as.place->initialized))
            return outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
        XrReferenceValue value = expected == XR_CORE_IR_PLACE && arguments[index].as.place
                                     ? *eval_place_value_const(arguments[index].as.place)
                                     : arguments[index].as.value;
        if (!reference_value_matches_type(context->program, value,
                                          function->parameter_types[index]))
            return outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
    }

    EvalRuntimeValue *values =
        xr_calloc(function->value_count ? function->value_count : 1u, sizeof(EvalRuntimeValue));
    EvalPlace *places =
        xr_calloc(function->value_count ? function->value_count : 1u, sizeof(EvalPlace));
    bool *initialized = xr_calloc(function->value_count ? function->value_count : 1u, sizeof(bool));
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
    EvalRuntimeValue *scratch =
        xr_calloc(scratch_count ? scratch_count : 1u, sizeof(EvalRuntimeValue));
    if (!values || !places || !initialized || !scratch) {
        xr_free(scratch);
        xr_free(initialized);
        xr_free(places);
        xr_free(values);
        return outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
    }

    uint32_t block_id = function->entry_block;
    uint32_t incoming_count = argument_count;
    if (incoming_count != 0)
        memcpy(scratch, arguments, (size_t) incoming_count * sizeof(EvalRuntimeValue));
    XrReferenceOutcome result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
    for (;;) {
        const XrValidatedBlock *block = &function->blocks[block_id];
        if (incoming_count != block->argument_count) {
            result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
            break;
        }
        for (uint32_t argument = 0; argument < block->argument_count; ++argument) {
            uint32_t value_id = block->argument_ids[argument];
            values[value_id] = scratch[argument];
            initialized[value_id] = true;
        }
        bool transferred = false;
        for (uint32_t instruction_id = 0; instruction_id < block->instruction_count;
             ++instruction_id) {
            const XrValidatedInstruction *instruction = &block->instructions[instruction_id];
            if (context->steps == context->budget.max_steps) {
                result = outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
                goto done;
            }
            ++context->steps;
            for (uint32_t operand = 0; operand < instruction->operand_count; ++operand) {
                if (!initialized[instruction->operands[operand]]) {
                    result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                    goto done;
                }
            }
            EvalRuntimeValue produced = {
                .category = XR_CORE_IR_VALUE,
                .as.value = void_value(),
            };
            bool has_result = instruction->result_id != XR_PROGRAM_LOCATION_NONE;
            switch (instruction->operation_id) {
                case XR_CORE_OP_CORE_CONSTANT_I64: {
                    const XrValidatedConstant *constant =
                        &context->program->constants[instruction->immediate.constant_id];
                    produced.as.value.kind = XR_REFERENCE_VALUE_I64;
                    produced.as.value.as.i64 = constant->value.i64;
                    break;
                }
                case XR_CORE_OP_CORE_CONSTANT_BOOL: {
                    const XrValidatedConstant *constant =
                        &context->program->constants[instruction->immediate.constant_id];
                    produced.as.value.kind = XR_REFERENCE_VALUE_BOOL;
                    produced.as.value.as.boolean = constant->value.boolean;
                    break;
                }
                case XR_CORE_OP_CORE_CONSTANT_TARGET_ENUM:
                    produced.as.value.kind =
                        instruction->result_type_id == XR_CORE_TYPE_TARGET_OS
                            ? XR_REFERENCE_VALUE_TARGET_OS
                        : instruction->result_type_id == XR_CORE_TYPE_TARGET_ARCH
                            ? XR_REFERENCE_VALUE_TARGET_ARCH
                        : instruction->result_type_id == XR_CORE_TYPE_TARGET_ABI
                            ? XR_REFERENCE_VALUE_TARGET_ABI
                            : XR_REFERENCE_VALUE_TARGET_ENDIAN;
                    produced.as.value.as.target_enum = (uint16_t) instruction->immediate.u32;
                    break;
                case XR_CORE_OP_CORE_ADD_I64:
                case XR_CORE_OP_CORE_SUB_I64:
                case XR_CORE_OP_CORE_MUL_I64: {
                    int64_t left = values[instruction->operands[0]].as.value.as.i64;
                    int64_t right = values[instruction->operands[1]].as.value.as.i64;
                    int64_t exact = 0;
                    bool valid = instruction->operation_id == XR_CORE_OP_CORE_ADD_I64
                                     ? checked_add(left, right, &exact)
                                 : instruction->operation_id == XR_CORE_OP_CORE_SUB_I64
                                     ? checked_sub(left, right, &exact)
                                     : checked_mul(left, right, &exact);
                    if (instruction->immediate.u32 == 0u && !valid) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_INTEGER_OVERFLOW);
                        goto done;
                    }
                    if (instruction->immediate.u32 != 0u) {
                        uint64_t bits = instruction->operation_id == XR_CORE_OP_CORE_ADD_I64
                                            ? (uint64_t) left + (uint64_t) right
                                        : instruction->operation_id == XR_CORE_OP_CORE_SUB_I64
                                            ? (uint64_t) left - (uint64_t) right
                                            : (uint64_t) left * (uint64_t) right;
                        exact = i64_from_bits(bits);
                    }
                    produced.as.value.kind = XR_REFERENCE_VALUE_I64;
                    produced.as.value.as.i64 = exact;
                    break;
                }
                case XR_CORE_OP_CORE_DIV_I64: {
                    int64_t left = values[instruction->operands[0]].as.value.as.i64;
                    int64_t right = values[instruction->operands[1]].as.value.as.i64;
                    if (right == 0) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_INTEGER_DIVISION_BY_ZERO);
                        goto done;
                    }
                    if (left == INT64_MIN && right == -1) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_INTEGER_DIVISION_OVERFLOW);
                        goto done;
                    }
                    produced.as.value.kind = XR_REFERENCE_VALUE_I64;
                    produced.as.value.as.i64 = left / right;
                    break;
                }
                case XR_CORE_OP_CORE_LOGICAL_NOT:
                    produced.as.value.kind = XR_REFERENCE_VALUE_BOOL;
                    produced.as.value.as.boolean =
                        !values[instruction->operands[0]].as.value.as.boolean;
                    break;
                case XR_CORE_OP_CORE_LOGICAL_AND:
                case XR_CORE_OP_CORE_LOGICAL_OR: {
                    bool left = values[instruction->operands[0]].as.value.as.boolean;
                    bool right = values[instruction->operands[1]].as.value.as.boolean;
                    produced.as.value.kind = XR_REFERENCE_VALUE_BOOL;
                    produced.as.value.as.boolean =
                        instruction->operation_id == XR_CORE_OP_CORE_LOGICAL_AND ? left && right
                                                                                : left || right;
                    break;
                }
                case XR_CORE_OP_CORE_COMPARE_I64: {
                    int64_t left = values[instruction->operands[0]].as.value.as.i64;
                    int64_t right = values[instruction->operands[1]].as.value.as.i64;
                    bool comparison = false;
                    switch (instruction->immediate.u32) {
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
                            result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                            goto done;
                    }
                    produced.as.value.kind = XR_REFERENCE_VALUE_BOOL;
                    produced.as.value.as.boolean = comparison;
                    break;
                }
                case XR_CORE_OP_CORE_COMPARE_TARGET_ENUM: {
                    uint16_t left =
                        values[instruction->operands[0]].as.value.as.target_enum;
                    uint16_t right =
                        values[instruction->operands[1]].as.value.as.target_enum;
                    produced.as.value.kind = XR_REFERENCE_VALUE_BOOL;
                    produced.as.value.as.boolean = instruction->immediate.u32 == 0u
                                                        ? left == right
                                                        : left != right;
                    break;
                }
                case XR_CORE_OP_CORE_BLOCK_ARGUMENT:
                    break;
                case XR_CORE_OP_CORE_BRANCH: {
                    const XrValidatedBlock *target = &function->blocks[instruction->successors[0]];
                    for (uint32_t index = 0; index < target->argument_count; ++index)
                        scratch[index] = values[instruction->operands[index]];
                    incoming_count = target->argument_count;
                    block_id = instruction->successors[0];
                    transferred = true;
                    break;
                }
                case XR_CORE_OP_CORE_CONDITIONAL_BRANCH: {
                    bool condition = values[instruction->operands[0]].as.value.as.boolean;
                    uint32_t successor = condition ? 0u : 1u;
                    uint32_t operand =
                        condition
                            ? 1u
                            : 1u + function->blocks[instruction->successors[0]].argument_count;
                    const XrValidatedBlock *target =
                        &function->blocks[instruction->successors[successor]];
                    for (uint32_t index = 0; index < target->argument_count; ++index)
                        scratch[index] = values[instruction->operands[operand + index]];
                    incoming_count = target->argument_count;
                    block_id = instruction->successors[successor];
                    transferred = true;
                    break;
                }
                case XR_CORE_OP_CORE_RETURN:
                    result = outcome(XR_REFERENCE_OUTCOME_RETURN, context);
                    result.value = instruction->operand_count == 0
                                       ? void_value()
                                       : values[instruction->operands[0]].as.value;
                    goto done;
                case XR_CORE_OP_CORE_CALL_SEALED_DIRECT:
                case XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT:
                case XR_CORE_OP_CORE_CALL_WITNESS_DIRECT: {
                    uint32_t target_function =
                        instruction->operation_id == XR_CORE_OP_CORE_CALL_SEALED_DIRECT
                            ? instruction->immediate.function_id
                            : XR_PROGRAM_LOCATION_NONE;
                    uint32_t source_argument = 0u;
                    uint32_t target_argument = 0u;
                    if (instruction->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_DIRECT) {
                        const XrReferenceExistentialValue *carrier =
                            values[instruction->operands[0]].as.value.as.existential;
                        target_function = witness_function_id(context->program, carrier,
                                                              instruction->immediate.u32);
                        if (target_function == XR_PROGRAM_LOCATION_NONE) {
                            result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                            goto done;
                        }
                        if (!witness_receiver_argument(
                                carrier, context->program->functions[target_function].receiver_mode,
                                &scratch[0])) {
                            result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                            goto done;
                        }
                        source_argument = 1u;
                        target_argument = 1u;
                    } else if (instruction->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT) {
                        const XrReferenceCallableValue *carrier =
                            values[instruction->operands[0]].as.value.as.callable;
                        target_function = callable_function_id(context->program, carrier);
                        if (target_function == XR_PROGRAM_LOCATION_NONE) {
                            result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                            goto done;
                        }
                        source_argument = 1u;
                        if (carrier->has_capture) {
                            scratch[0] = (EvalRuntimeValue) {
                                .category = XR_CORE_IR_VALUE,
                                .as.value = carrier->capture,
                            };
                            target_argument = 1u;
                        }
                    }
                    uint32_t call_operand_count = instruction->operand_count;
                    if (instruction->successor_count == 1u)
                        call_operand_count -=
                            function->blocks[instruction->successors[0]].argument_count;
                    for (; source_argument < call_operand_count;
                         ++source_argument, ++target_argument)
                        scratch[target_argument] = values[instruction->operands[source_argument]];
                    const XrValidatedFunction *callee =
                        &context->program->functions[target_function];
                    XrReferenceOutcome nested = evaluate_function(
                        context, target_function, scratch, callee->parameter_count, depth + 1u);
                    if (nested.kind != XR_REFERENCE_OUTCOME_RETURN) {
                        if (nested.kind == XR_REFERENCE_OUTCOME_TRAP &&
                            nested.trap == XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED &&
                            instruction->successor_count == 1u) {
                            const XrValidatedBlock *target =
                                &function->blocks[instruction->successors[0]];
                            for (uint32_t index = 0u; index < target->argument_count; ++index)
                                scratch[index] =
                                    values[instruction->operands[call_operand_count + index]];
                            incoming_count = target->argument_count;
                            block_id = instruction->successors[0];
                            transferred = true;
                            break;
                        }
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
                        instruction->operation_id == XR_CORE_OP_CORE_CALL_SEALED_INVOKE
                            ? instruction->immediate.function_id
                            : XR_PROGRAM_LOCATION_NONE;
                    uint32_t source_argument = 0u;
                    uint32_t target_argument = 0u;
                    if (instruction->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_INVOKE) {
                        const XrReferenceExistentialValue *carrier =
                            values[instruction->operands[0]].as.value.as.existential;
                        target_function = witness_function_id(context->program, carrier,
                                                              instruction->immediate.u32);
                        if (target_function == XR_PROGRAM_LOCATION_NONE) {
                            result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                            goto done;
                        }
                        if (!witness_receiver_argument(
                                carrier, context->program->functions[target_function].receiver_mode,
                                &scratch[0])) {
                            result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                            goto done;
                        }
                        source_argument = 1u;
                        target_argument = 1u;
                    } else if (instruction->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE) {
                        const XrReferenceCallableValue *carrier =
                            values[instruction->operands[0]].as.value.as.callable;
                        target_function = callable_function_id(context->program, carrier);
                        if (target_function == XR_PROGRAM_LOCATION_NONE) {
                            result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                            goto done;
                        }
                        source_argument = 1u;
                        if (carrier->has_capture) {
                            scratch[0] = (EvalRuntimeValue) {
                                .category = XR_CORE_IR_VALUE,
                                .as.value = carrier->capture,
                            };
                            target_argument = 1u;
                        }
                    }
                    const XrValidatedFunction *callee =
                        &context->program->functions[target_function];
                    for (; target_argument < callee->parameter_count;
                         ++source_argument, ++target_argument)
                        scratch[target_argument] = values[instruction->operands[source_argument]];
                    XrReferenceOutcome nested = evaluate_function(
                        context, target_function, scratch, callee->parameter_count, depth + 1u);
                    uint32_t successor = 0u;
                    uint32_t implicit = 0u;
                    uint32_t operand = callee->parameter_count;
                    if (instruction->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE &&
                        !callee->has_receiver)
                        ++operand;
                    if (nested.kind == XR_REFERENCE_OUTCOME_RETURN) {
                        if (callee->result_type_id != XR_CORE_TYPE_VOID) {
                            scratch[0] = (EvalRuntimeValue) {
                                .category = XR_CORE_IR_VALUE,
                                .as.value = nested.value,
                            };
                            implicit = 1u;
                        }
                    } else if (nested.kind == XR_REFERENCE_OUTCOME_ERROR) {
                        successor = 1u;
                        operand += function->blocks[instruction->successors[0]].argument_count -
                                   (callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u);
                        scratch[0] = (EvalRuntimeValue) {
                            .category = XR_CORE_IR_VALUE,
                            .as.value = nested.error_value,
                        };
                        implicit = 1u;
                    } else if (nested.kind == XR_REFERENCE_OUTCOME_PANIC) {
                        successor = 1u + (callee->error_type_id == XR_CORE_TYPE_VOID ? 0u : 1u);
                        operand += function->blocks[instruction->successors[0]].argument_count -
                                   (callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u);
                        if (callee->error_type_id != XR_CORE_TYPE_VOID)
                            operand +=
                                function->blocks[instruction->successors[1]].argument_count - 1u;
                        scratch[0] = (EvalRuntimeValue) {
                            .category = XR_CORE_IR_VALUE,
                            .as.value = nested.panic_value,
                        };
                        implicit = 1u;
                    } else {
                        result = nested;
                        goto done;
                    }
                    const XrValidatedBlock *target =
                        &function->blocks[instruction->successors[successor]];
                    for (uint32_t index = implicit; index < target->argument_count; ++index)
                        scratch[index] = values[instruction->operands[operand + index - implicit]];
                    incoming_count = target->argument_count;
                    block_id = instruction->successors[successor];
                    transferred = true;
                    break;
                }
                case XR_CORE_OP_CORE_TRAP:
                    result = trap_outcome(
                        context, instruction->immediate.u32 == 7u
                                     ? XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED
                                     : XR_REFERENCE_TRAP_EXPLICIT);
                    goto done;
                case XR_CORE_OP_CORE_ERROR_PUBLISH:
                    result = outcome(XR_REFERENCE_OUTCOME_ERROR, context);
                    result.error_value = values[instruction->operands[0]].as.value;
                    goto done;
                case XR_CORE_OP_CORE_PANIC_PUBLISH:
                    result = outcome(XR_REFERENCE_OUTCOME_PANIC, context);
                    result.panic_value = values[instruction->operands[0]].as.value;
                    goto done;
                case XR_CORE_OP_CORE_TARGET_POINTER_WIDTH:
                    if (context->profile.pointer_width != 32u &&
                        context->profile.pointer_width != 64u) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_PROFILE_UNAVAILABLE);
                        goto done;
                    }
                    produced.as.value.kind = XR_REFERENCE_VALUE_U16;
                    produced.as.value.as.u16 = context->profile.pointer_width;
                    break;
                case XR_CORE_OP_CORE_TARGET_OPERATING_SYSTEM:
                    if (context->profile.operating_system <= XR_TARGET_OS_NONE ||
                        context->profile.operating_system >= XR_TARGET_OS_COUNT) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_PROFILE_UNAVAILABLE);
                        goto done;
                    }
                    produced.as.value.kind = XR_REFERENCE_VALUE_TARGET_OS;
                    produced.as.value.as.target_enum = context->profile.operating_system;
                    break;
                case XR_CORE_OP_CORE_TARGET_ARCHITECTURE:
                    if (context->profile.architecture <= XR_TARGET_ARCH_NONE ||
                        context->profile.architecture >= XR_TARGET_ARCH_COUNT) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_PROFILE_UNAVAILABLE);
                        goto done;
                    }
                    produced.as.value.kind = XR_REFERENCE_VALUE_TARGET_ARCH;
                    produced.as.value.as.target_enum = context->profile.architecture;
                    break;
                case XR_CORE_OP_CORE_TARGET_NATIVE_ABI:
                    if (context->profile.native_abi <= XR_TARGET_ABI_NONE ||
                        context->profile.native_abi >= XR_TARGET_ABI_COUNT) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_PROFILE_UNAVAILABLE);
                        goto done;
                    }
                    produced.as.value.kind = XR_REFERENCE_VALUE_TARGET_ABI;
                    produced.as.value.as.target_enum = context->profile.native_abi;
                    break;
                case XR_CORE_OP_CORE_TARGET_ENDIANNESS:
                    if (context->profile.endianness != XR_TARGET_ENDIAN_LITTLE &&
                        context->profile.endianness != XR_TARGET_ENDIAN_BIG) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_PROFILE_UNAVAILABLE);
                        goto done;
                    }
                    produced.as.value.kind = XR_REFERENCE_VALUE_TARGET_ENDIAN;
                    produced.as.value.as.target_enum = context->profile.endianness;
                    break;
                case XR_CORE_OP_CORE_PROVIDER_CALL: {
                    bool has_trap_edge = instruction->successor_count != 0u;
                    const XrValidatedBlock *trap_target =
                        has_trap_edge ? &function->blocks[instruction->successors[0]] : NULL;
                    uint32_t provider_operand_count =
                        instruction->operand_count - (trap_target ? trap_target->argument_count : 0u);
                    uint16_t operand_type = provider_operand_count == 1u
                                                ? function->value_types[instruction->operands[0]]
                                                : XR_CORE_TYPE_VOID;
                    XrProviderLogicalCallKind call_kind = xr_validated_program_provider_call_kind(
                        context->program, instruction->result_type_id,
                        provider_operand_count == 1u ? &operand_type : NULL,
                        provider_operand_count);
                    bool call_ok = false;
                    if (call_kind == XR_PROVIDER_LOGICAL_CALL_I64_UNARY ||
                        call_kind == XR_PROVIDER_LOGICAL_CALL_I64_NULLARY) {
                        int64_t provider_result = 0;
                        call_ok = context->providers &&
                                  (call_kind == XR_PROVIDER_LOGICAL_CALL_I64_UNARY
                                       ? context->providers->call_i64_unary &&
                                             context->providers->call_i64_unary(
                                                 context->providers->context,
                                                 instruction->immediate.provider_operation
                                                     .requirement_index,
                                                 instruction->immediate.provider_operation
                                                     .operation_index,
                                                 values[instruction->operands[0]].as.value.as.i64,
                                                 &provider_result)
                                       : context->providers->call_i64_nullary &&
                                             context->providers->call_i64_nullary(
                                                 context->providers->context,
                                                 instruction->immediate.provider_operation
                                                     .requirement_index,
                                                 instruction->immediate.provider_operation
                                                     .operation_index,
                                                 &provider_result));
                        if (call_ok) {
                            produced.as.value.kind = XR_REFERENCE_VALUE_I64;
                            produced.as.value.as.i64 = provider_result;
                        }
                    } else if (call_kind == XR_PROVIDER_LOGICAL_CALL_BOOL_I64_UNARY) {
                        bool provider_result = false;
                        call_ok = context->providers && context->providers->call_bool_i64_unary &&
                                  context->providers->call_bool_i64_unary(
                                      context->providers->context,
                                      instruction->immediate.provider_operation.requirement_index,
                                      instruction->immediate.provider_operation.operation_index,
                                      values[instruction->operands[0]].as.value.as.i64,
                                      &provider_result);
                        if (call_ok) {
                            produced.as.value.kind = XR_REFERENCE_VALUE_BOOL;
                            produced.as.value.as.boolean = provider_result;
                        }
                    } else if (call_kind == XR_PROVIDER_LOGICAL_CALL_OPTIONAL_I64_PAIR_NULLARY) {
                        uint16_t pair_type_id = XR_CORE_TYPE_VOID;
                        (void) xr_validated_program_type_is_optional_i64_pair(
                            context->program, instruction->result_type_id, &pair_type_id);
                        XrReferenceAggregateValue *pair =
                            allocate_aggregate(context, pair_type_id, UINT32_MAX, 2u);
                        XrReferenceAggregateValue *optional =
                            allocate_aggregate(context, instruction->result_type_id, 1u, 1u);
                        if (!pair || !optional) {
                            result = outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
                            goto done;
                        }
                        bool present = false;
                        int64_t first = 0;
                        int64_t second = 0;
                        call_ok = context->providers &&
                                  context->providers->call_optional_i64_pair_nullary &&
                                  context->providers->call_optional_i64_pair_nullary(
                                      context->providers->context,
                                      instruction->immediate.provider_operation.requirement_index,
                                      instruction->immediate.provider_operation.operation_index,
                                      &present, &first, &second);
                        if (call_ok) {
                            if (present) {
                                pair->fields[0] = (XrReferenceValue) {
                                    .kind = XR_REFERENCE_VALUE_I64, .as.i64 = first};
                                pair->fields[1] = (XrReferenceValue) {
                                    .kind = XR_REFERENCE_VALUE_I64, .as.i64 = second};
                                optional->fields[0] = (XrReferenceValue) {
                                    .kind = XR_REFERENCE_VALUE_AGGREGATE, .as.aggregate = pair};
                            } else {
                                optional->variant_ordinal = 0u;
                                optional->field_count = 0u;
                            }
                            produced.as.value.kind = XR_REFERENCE_VALUE_AGGREGATE;
                            produced.as.value.as.aggregate = optional;
                        }
                    }
                    if (!call_ok) {
                        if (!has_trap_edge) {
                            result =
                                trap_outcome(context, XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED);
                            goto done;
                        }
                        for (uint32_t index = 0u; index < trap_target->argument_count; ++index)
                            scratch[index] =
                                values[instruction->operands[provider_operand_count + index]];
                        incoming_count = trap_target->argument_count;
                        block_id = instruction->successors[0];
                        transferred = true;
                    }
                    break;
                }
                case XR_CORE_OP_CORE_OUTPUT_GROUP_I64: {
                    uint8_t bytes[22];
                    size_t size = format_i64_line(
                        values[instruction->operands[0]].as.value.as.i64, bytes);
                    if (!context->providers || !context->providers->output_write ||
                        !context->providers->output_write(
                            context->providers->context,
                            instruction->immediate.provider_operation.requirement_index,
                            instruction->immediate.provider_operation.operation_index, bytes,
                            size)) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED);
                        goto done;
                    }
                    break;
                }
                case XR_CORE_OP_CORE_CALLABLE_PACK: {
                    XrReferenceCallableValue *carrier = allocate_callable(context);
                    if (!carrier) {
                        result = outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    carrier->callable_type_id = instruction->result_type_id;
                    carrier->function_id = instruction->immediate.function_id;
                    carrier->has_capture = instruction->operand_count != 0u;
                    if (carrier->has_capture) {
                        uint32_t capture_value = instruction->operands[0];
                        carrier->capture_type_id = function->value_types[capture_value];
                        carrier->capture = values[capture_value].as.value;
                    }
                    produced.as.value.kind = XR_REFERENCE_VALUE_CALLABLE;
                    produced.as.value.as.callable = carrier;
                    break;
                }
                case XR_CORE_OP_CORE_OWNER_COPY:
                    if (!clone_reference_value(context, values[instruction->operands[0]].as.value,
                                               instruction->result_type_id, &produced.as.value)) {
                        result = outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    break;
                case XR_CORE_OP_CORE_OWNER_MOVE:
                    produced.as.value = values[instruction->operands[0]].as.value;
                    break;
                case XR_CORE_OP_CORE_OWNER_DROP:
                    break;
                case XR_CORE_OP_CORE_PLACE_LOCAL:
                    places[instruction->result_id].value =
                        values[instruction->operands[0]].as.value;
                    places[instruction->result_id].initialized = true;
                    produced.category = XR_CORE_IR_PLACE;
                    produced.as.place = &places[instruction->result_id];
                    break;
                case XR_CORE_OP_CORE_PLACE_LOAD:
                    produced.as.value =
                        *eval_place_value(values[instruction->operands[0]].as.place);
                    break;
                case XR_CORE_OP_CORE_PLACE_STORE:
                    *eval_place_value(values[instruction->operands[0]].as.place) =
                        values[instruction->operands[1]].as.value;
                    break;
                case XR_CORE_OP_CORE_PLACE_PROJECT: {
                    XrReferenceValue *source =
                        eval_place_value(values[instruction->operands[0]].as.place);
                    XrReferenceAggregateValue *aggregate =
                        (XrReferenceAggregateValue *) (void *) source->as.aggregate;
                    places[instruction->result_id].alias =
                        &aggregate->fields[instruction->immediate.field_ordinal];
                    places[instruction->result_id].initialized = true;
                    produced.category = XR_CORE_IR_PLACE;
                    produced.as.place = &places[instruction->result_id];
                    break;
                }
                case XR_CORE_OP_CORE_PLACE_TAKE:
                    produced.as.value =
                        *eval_place_value(values[instruction->operands[0]].as.place);
                    values[instruction->operands[0]].as.place->initialized = false;
                    break;
                case XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT: {
                    XrReferenceAggregateValue *aggregate =
                        allocate_aggregate(context, instruction->result_type_id, UINT32_MAX,
                                           instruction->operand_count);
                    if (!aggregate) {
                        result = outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    for (uint32_t field = 0; field < instruction->operand_count; ++field)
                        aggregate->fields[field] = values[instruction->operands[field]].as.value;
                    produced.as.value.kind = XR_REFERENCE_VALUE_AGGREGATE;
                    produced.as.value.as.aggregate = aggregate;
                    break;
                }
                case XR_CORE_OP_CORE_AGGREGATE_PROJECT: {
                    const XrReferenceAggregateValue *aggregate =
                        values[instruction->operands[0]].as.value.as.aggregate;
                    produced.as.value = aggregate->fields[instruction->immediate.field_ordinal];
                    break;
                }
                case XR_CORE_OP_CORE_AGGREGATE_UPDATE: {
                    const XrReferenceAggregateValue *source =
                        values[instruction->operands[0]].as.value.as.aggregate;
                    XrReferenceAggregateValue *aggregate = allocate_aggregate(
                        context, instruction->result_type_id, UINT32_MAX, source->field_count);
                    if (!aggregate) {
                        result = outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    memcpy(aggregate->fields, source->fields,
                           (size_t) source->field_count * sizeof(XrReferenceValue));
                    aggregate->fields[instruction->immediate.field_ordinal] =
                        values[instruction->operands[1]].as.value;
                    produced.as.value.kind = XR_REFERENCE_VALUE_AGGREGATE;
                    produced.as.value.as.aggregate = aggregate;
                    break;
                }
                case XR_CORE_OP_CORE_VARIANT_CONSTRUCT: {
                    XrReferenceAggregateValue *aggregate = allocate_aggregate(
                        context, instruction->result_type_id,
                        instruction->immediate.variant_ordinal, instruction->operand_count);
                    if (!aggregate) {
                        result = outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    for (uint32_t field = 0; field < instruction->operand_count; ++field)
                        aggregate->fields[field] = values[instruction->operands[field]].as.value;
                    produced.as.value.kind = XR_REFERENCE_VALUE_AGGREGATE;
                    produced.as.value.as.aggregate = aggregate;
                    break;
                }
                case XR_CORE_OP_CORE_VARIANT_TEST: {
                    const XrReferenceAggregateValue *aggregate =
                        values[instruction->operands[0]].as.value.as.aggregate;
                    produced.as.value.kind = XR_REFERENCE_VALUE_BOOL;
                    produced.as.value.as.boolean =
                        aggregate->variant_ordinal == instruction->immediate.variant_ordinal;
                    break;
                }
                case XR_CORE_OP_CORE_VARIANT_PROJECT: {
                    const XrReferenceAggregateValue *aggregate =
                        values[instruction->operands[0]].as.value.as.aggregate;
                    if (aggregate->variant_ordinal !=
                        instruction->immediate.variant_field.variant_ordinal) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_VARIANT_TAG_MISMATCH);
                        goto done;
                    }
                    produced.as.value =
                        aggregate->fields[instruction->immediate.variant_field.field_ordinal];
                    break;
                }
                case XR_CORE_OP_CORE_EXISTENTIAL_PACK: {
                    const XrValidatedType *existential =
                        xr_validated_program_type(context->program, instruction->result_type_id);
                    uint16_t concrete_type = function->value_types[instruction->operands[0]];
                    XrReferenceExistentialValue *carrier = allocate_existential(context);
                    uint32_t conformance = existential
                                               ? conformance_id(context->program, concrete_type,
                                                                existential->interface_id)
                                               : XR_PROGRAM_LOCATION_NONE;
                    if (!carrier || conformance == XR_PROGRAM_LOCATION_NONE) {
                        result = outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
                        goto done;
                    }
                    carrier->existential_type_id = instruction->result_type_id;
                    carrier->concrete_type_id = concrete_type;
                    carrier->conformance_id = conformance;
                    if (existential->interface_use_kind ==
                        XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE) {
                        carrier->owned_storage.value = values[instruction->operands[0]].as.value;
                        carrier->owned_storage.initialized = true;
                        carrier->payload.category = XR_CORE_IR_PLACE;
                        carrier->payload.as.place = &carrier->owned_storage;
                    } else {
                        carrier->payload = values[instruction->operands[0]];
                    }
                    produced.as.value.kind = XR_REFERENCE_VALUE_EXISTENTIAL;
                    produced.as.value.as.existential = carrier;
                    break;
                }
                case XR_CORE_OP_CORE_EXISTENTIAL_TEST: {
                    const XrReferenceExistentialValue *carrier =
                        values[instruction->operands[0]].as.value.as.existential;
                    produced.as.value.kind = XR_REFERENCE_VALUE_BOOL;
                    produced.as.value.as.boolean =
                        carrier->concrete_type_id == instruction->immediate.type_id;
                    break;
                }
                case XR_CORE_OP_CORE_EXISTENTIAL_PROJECT: {
                    const XrReferenceExistentialValue *carrier =
                        values[instruction->operands[0]].as.value.as.existential;
                    const XrValidatedType *existential =
                        xr_validated_program_type(context->program, carrier->existential_type_id);
                    if (existential && existential->interface_use_kind ==
                                           XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE) {
                        produced.category = XR_CORE_IR_VALUE;
                        produced.as.value = *eval_place_value_const(&carrier->owned_storage);
                    } else {
                        produced = carrier->payload;
                    }
                    break;
                }
                default:
                    result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                    goto done;
            }
            if (has_result && !transferred) {
                values[instruction->result_id] = produced;
                initialized[instruction->result_id] = true;
            }
            if (transferred)
                break;
        }
        if (!transferred) {
            result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
            break;
        }
    }

done:
    result.steps = context->steps;
    xr_free(scratch);
    xr_free(initialized);
    xr_free(places);
    xr_free(values);
    return result;
}

static XrReferenceOutcome execution_outcome(XrReferenceExecution *execution,
                                            XrReferenceOutcomeKind kind) {
    XrReferenceOutcome result = {.kind = kind};
    if (execution) {
        result.steps = execution->steps;
        result.state_id = execution->state_id;
    }
    return result;
}

static bool reference_coroutine_operation_supported(uint16_t operation_id) {
    return operation_id == XR_CORE_OP_CORE_BLOCK_ARGUMENT ||
           operation_id == XR_CORE_OP_CORE_CONSTANT_I64 ||
           operation_id == XR_CORE_OP_CORE_ADD_I64 ||
           operation_id == XR_CORE_OP_CORE_BRANCH ||
           operation_id == XR_CORE_OP_CORE_COROUTINE_YIELD ||
           operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED ||
           operation_id == XR_CORE_OP_CORE_RETURN;
}

static void reference_execution_release_lease(XrReferenceExecution *execution) {
    if (execution && execution->owns_lease && xr_execution_lease_is_valid(&execution->lease))
        (void) xr_execution_lease_release(&execution->lease);
}

static bool reference_child_execution_create(
    XrReferenceExecution *parent, const XrValidatedInstruction *instruction,
    XrReferenceExecution **child_out) {
    if (child_out)
        *child_out = NULL;
    if (!parent || !instruction || !child_out ||
        instruction->immediate.coroutine_call.function_id >= parent->program->function_count)
        return false;
    uint32_t function_id = instruction->immediate.coroutine_call.function_id;
    const XrValidatedFunction *function = &parent->program->functions[function_id];
    if (instruction->operand_count < function->parameter_count ||
        function->value_count > parent->budget.max_value_cells)
        return false;
    XrReferenceExecution *child = xr_calloc(1u, sizeof(*child));
    if (!child)
        return false;
    child->lease = parent->lease;
    child->program = xr_validated_program_retain(parent->program);
    child->budget = parent->budget;
    child->function_id = function_id;
    child->block_id = function->entry_block;
    child->values =
        xr_calloc(function->value_count ? function->value_count : 1u, sizeof(*child->values));
    child->initialized = xr_calloc(function->value_count ? function->value_count : 1u,
                                   sizeof(*child->initialized));
    if (!child->program || !child->values || !child->initialized) {
        xr_reference_execution_free(child);
        return false;
    }
    for (uint32_t parameter = 0u; parameter < function->parameter_count; ++parameter) {
        uint32_t source = instruction->operands[parameter];
        uint32_t target = function->blocks[function->entry_block].argument_ids[parameter];
        if (!parent->initialized[source] || function->parameter_modes[parameter] != XR_PARAM_READ ||
            !reference_value_matches_type(parent->program, parent->values[source],
                                          function->parameter_types[parameter])) {
            xr_reference_execution_free(child);
            return false;
        }
        child->values[target] = parent->values[source];
        child->initialized[target] = true;
    }
    *child_out = child;
    return true;
}

bool xr_reference_execution_create(XrInstance *instance, uint32_t function_id,
                                   const XrReferenceValue *arguments, uint32_t argument_count,
                                   const XrReferenceBudget *budget,
                                   XrReferenceExecution **execution_out) {
    if (execution_out)
        *execution_out = NULL;
    XrReferenceBudget selected = budget ? *budget : xr_reference_default_budget();
    if (!instance || !execution_out || (argument_count != 0u && !arguments) ||
        selected.max_steps == 0u || selected.max_value_cells == 0u || selected.max_call_depth == 0u)
        return false;
    XrExecutionLease lease = {0};
    if (!xr_execution_instance_acquire(instance, &lease))
        return false;
    XrValidatedProgram *program = xr_execution_lease_retain_program(&lease);
    if (!program || function_id >= program->function_count) {
        xr_validated_program_free(program);
        (void) xr_execution_lease_release(&lease);
        return false;
    }
    const XrValidatedFunction *function = &program->functions[function_id];
    if (argument_count != function->parameter_count || function->coroutine_state_count != 2u ||
        function->coroutine_safepoint_count != 1u ||
        function->value_count > selected.max_value_cells)
        goto reject;
    for (uint32_t block = 0; block < function->block_count; ++block)
        for (uint32_t instruction = 0; instruction < function->blocks[block].instruction_count;
             ++instruction)
            if (!reference_coroutine_operation_supported(
                    function->blocks[block].instructions[instruction].operation_id))
                goto reject;
    XrReferenceExecution *execution = xr_calloc(1u, sizeof(*execution));
    if (!execution)
        goto reject;
    execution->lease = lease;
    execution->owns_lease = true;
    execution->program = program;
    execution->values =
        xr_calloc(function->value_count ? function->value_count : 1u, sizeof(*execution->values));
    execution->initialized = xr_calloc(function->value_count ? function->value_count : 1u,
                                       sizeof(*execution->initialized));
    if (!execution->values || !execution->initialized) {
        xr_reference_execution_free(execution);
        return false;
    }
    execution->budget = selected;
    execution->function_id = function_id;
    execution->block_id = function->entry_block;
    for (uint32_t argument = 0; argument < argument_count; ++argument) {
        uint32_t value_id = function->blocks[function->entry_block].argument_ids[argument];
        if (function->parameter_modes[argument] == XR_PARAM_REF ||
            !reference_value_matches_type(program, arguments[argument],
                                          function->parameter_types[argument])) {
            xr_reference_execution_free(execution);
            return false;
        }
        execution->values[value_id] = arguments[argument];
        execution->initialized[value_id] = true;
    }
    *execution_out = execution;
    return true;

reject:
    xr_validated_program_free(program);
    (void) xr_execution_lease_release(&lease);
    return false;
}

XrReferenceOutcome xr_reference_execution_step(XrReferenceExecution *execution) {
    if (!execution || execution->finished || !xr_execution_lease_is_valid(&execution->lease))
        return execution_outcome(execution, XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
    const XrValidatedFunction *function = &execution->program->functions[execution->function_id];
    for (;;) {
        const XrValidatedBlock *block = &function->blocks[execution->block_id];
        if (execution->instruction_id >= block->instruction_count) {
            execution->finished = true;
            reference_execution_release_lease(execution);
            return execution_outcome(execution, XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
        }
        const XrValidatedInstruction *instruction =
            &block->instructions[execution->instruction_id++];
        if (++execution->steps > execution->budget.max_steps) {
            execution->finished = true;
            reference_execution_release_lease(execution);
            return execution_outcome(execution, XR_REFERENCE_OUTCOME_RESOURCE_LIMIT);
        }
        for (uint32_t operand = 0; operand < instruction->operand_count; ++operand) {
            if (!execution->initialized[instruction->operands[operand]]) {
                execution->finished = true;
                reference_execution_release_lease(execution);
                return execution_outcome(execution, XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
            }
        }
        switch (instruction->operation_id) {
            case XR_CORE_OP_CORE_BLOCK_ARGUMENT:
                break;
            case XR_CORE_OP_CORE_CONSTANT_I64: {
                const XrValidatedConstant *constant =
                    &execution->program->constants[instruction->immediate.constant_id];
                execution->values[instruction->result_id] = (XrReferenceValue) {
                    .kind = XR_REFERENCE_VALUE_I64,
                    .as.i64 = constant->value.i64,
                };
                execution->initialized[instruction->result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_ADD_I64: {
                int64_t left = execution->values[instruction->operands[0]].as.i64;
                int64_t right = execution->values[instruction->operands[1]].as.i64;
                int64_t value = 0;
                if (instruction->immediate.u32 == 0u) {
                    if (!checked_add(left, right, &value)) {
                        execution->finished = true;
                        reference_execution_release_lease(execution);
                        XrReferenceOutcome result =
                            execution_outcome(execution, XR_REFERENCE_OUTCOME_TRAP);
                        result.trap = XR_REFERENCE_TRAP_INTEGER_OVERFLOW;
                        return result;
                    }
                } else {
                    value = i64_from_bits((uint64_t) left + (uint64_t) right);
                }
                execution->values[instruction->result_id] =
                    (XrReferenceValue) {.kind = XR_REFERENCE_VALUE_I64, .as.i64 = value};
                execution->initialized[instruction->result_id] = true;
                break;
            }
            case XR_CORE_OP_CORE_BRANCH: {
                const XrValidatedBlock *target =
                    &function->blocks[instruction->successors[0]];
                for (uint32_t argument = 0u; argument < instruction->operand_count; ++argument) {
                    uint32_t target_value = target->argument_ids[argument];
                    execution->values[target_value] =
                        execution->values[instruction->operands[argument]];
                    execution->initialized[target_value] = true;
                }
                execution->block_id = instruction->successors[0];
                execution->instruction_id = 0u;
                break;
            }
            case XR_CORE_OP_CORE_COROUTINE_YIELD: {
                const XrValidatedCoroutineSafepoint *safepoint =
                    &function->coroutine_safepoints[instruction->immediate.u32];
                const XrValidatedBlock *resume = &function->blocks[instruction->successors[0]];
                for (uint32_t live = 0; live < instruction->operand_count; ++live) {
                    uint32_t target = resume->argument_ids[live];
                    execution->values[target] = execution->values[instruction->operands[live]];
                    execution->initialized[target] = true;
                }
                execution->state_id = safepoint->resume_state_id;
                execution->block_id = instruction->successors[0];
                execution->instruction_id = 0u;
                XrReferenceOutcome result =
                    execution_outcome(execution, XR_REFERENCE_OUTCOME_SUSPENDED);
                result.safepoint_id = instruction->immediate.u32;
                return result;
            }
            case XR_CORE_OP_CORE_COROUTINE_CALL_SEALED: {
                uint32_t callee_id = instruction->immediate.coroutine_call.function_id;
                uint32_t safepoint_id = instruction->immediate.coroutine_call.safepoint_id;
                const XrValidatedFunction *callee = &execution->program->functions[callee_id];
                const XrValidatedCoroutineSafepoint *safepoint =
                    &function->coroutine_safepoints[safepoint_id];
                if (!execution->child &&
                    !reference_child_execution_create(execution, instruction, &execution->child)) {
                    execution->finished = true;
                    reference_execution_release_lease(execution);
                    return execution_outcome(execution, XR_REFERENCE_OUTCOME_RESOURCE_LIMIT);
                }
                uint64_t child_steps = execution->child->steps;
                uint64_t remaining_steps = execution->budget.max_steps - execution->steps;
                execution->child->budget.max_steps =
                    child_steps > UINT64_MAX - remaining_steps
                        ? UINT64_MAX
                        : child_steps + remaining_steps;
                XrReferenceOutcome child = xr_reference_execution_step(execution->child);
                uint64_t child_delta = child.steps - child_steps;
                if (child_delta > execution->budget.max_steps - execution->steps) {
                    execution->finished = true;
                    xr_reference_execution_free(execution->child);
                    execution->child = NULL;
                    reference_execution_release_lease(execution);
                    return execution_outcome(execution, XR_REFERENCE_OUTCOME_RESOURCE_LIMIT);
                }
                execution->steps += child_delta;
                if (child.kind == XR_REFERENCE_OUTCOME_SUSPENDED) {
                    --execution->instruction_id;
                    execution->state_id = safepoint->resume_state_id;
                    XrReferenceOutcome suspended =
                        execution_outcome(execution, XR_REFERENCE_OUTCOME_SUSPENDED);
                    suspended.safepoint_id = safepoint_id;
                    return suspended;
                }
                if (child.kind != XR_REFERENCE_OUTCOME_RETURN) {
                    execution->finished = true;
                    xr_reference_execution_free(execution->child);
                    execution->child = NULL;
                    reference_execution_release_lease(execution);
                    child.steps = execution->steps;
                    child.state_id = execution->state_id;
                    return child;
                }
                const XrValidatedBlock *normal = &function->blocks[instruction->successors[0]];
                uint32_t implicit_result = callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
                if (implicit_result != 0u) {
                    uint32_t target = normal->argument_ids[0];
                    execution->values[target] = child.value;
                    execution->initialized[target] = true;
                }
                for (uint32_t live = 0u; live < safepoint->live_value_count; ++live) {
                    uint32_t source = instruction->operands[callee->parameter_count + live];
                    uint32_t target = normal->argument_ids[implicit_result + live];
                    execution->values[target] = execution->values[source];
                    execution->initialized[target] = true;
                }
                xr_reference_execution_free(execution->child);
                execution->child = NULL;
                execution->block_id = instruction->successors[0];
                execution->instruction_id = 0u;
                break;
            }
            case XR_CORE_OP_CORE_RETURN: {
                execution->finished = true;
                XrReferenceOutcome result =
                    execution_outcome(execution, XR_REFERENCE_OUTCOME_RETURN);
                result.value = instruction->operand_count == 0u
                                   ? void_value()
                                   : execution->values[instruction->operands[0]];
                reference_execution_release_lease(execution);
                return result;
            }
            default:
                execution->finished = true;
                reference_execution_release_lease(execution);
                return execution_outcome(execution, XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
        }
    }
}

void xr_reference_execution_free(XrReferenceExecution *execution) {
    if (!execution)
        return;
    xr_reference_execution_free(execution->child);
    xr_free(execution->initialized);
    xr_free(execution->values);
    reference_execution_release_lease(execution);
    xr_validated_program_free(execution->program);
    xr_free(execution);
}

XrReferenceBudget xr_reference_default_budget(void) {
    XrReferenceBudget budget = {
        .max_steps = UINT64_C(1000000),
        .max_value_cells = UINT64_C(1048576),
        .max_call_depth = 1024u,
    };
    return budget;
}

XrReferenceOutcome xr_reference_evaluate_bound(
    const XrValidatedProgram *program, uint32_t function_id, const XrReferenceValue *arguments,
    uint32_t argument_count, const XrReferenceProfile *profile, const XrReferenceBudget *budget,
    const XrReferenceProviderBinding *providers) {
    XrReferenceBudget selected = budget ? *budget : xr_reference_default_budget();
    EvalContext context = {
        .program = program,
        .providers = providers,
        .profile = profile ? *profile : (XrReferenceProfile) {0},
        .budget = selected,
    };
    if (program && function_id < program->function_count &&
        program->functions[function_id].coroutine_safepoint_count != 0u)
        return outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, &context);
    if (!program || function_id >= program->function_count || (argument_count != 0 && !arguments) ||
        selected.max_steps == 0 || selected.max_value_cells == 0 || selected.max_call_depth == 0)
        return outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, &context);
    const XrValidatedFunction *function = &program->functions[function_id];
    EvalRuntimeValue *runtime_arguments =
        xr_calloc(argument_count ? argument_count : 1u, sizeof(EvalRuntimeValue));
    if (!runtime_arguments)
        return outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, &context);
    for (uint32_t index = 0; index < argument_count; ++index) {
        if (index >= function->parameter_count ||
            function->parameter_modes[index] == XR_PARAM_REF) {
            xr_free(runtime_arguments);
            return outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, &context);
        }
        runtime_arguments[index].category = XR_CORE_IR_VALUE;
        runtime_arguments[index].as.value = arguments[index];
    }
    XrReferenceOutcome result =
        evaluate_function(&context, function_id, runtime_arguments, argument_count, 1u);
    xr_free(runtime_arguments);
    if (result.kind == XR_REFERENCE_OUTCOME_RETURN &&
        result.value.kind == XR_REFERENCE_VALUE_AGGREGATE) {
        XrReferenceValue detached = void_value();
        if (detach_reference_value(result.value, &detached)) {
            result.value = detached;
            result.owns_dynamic_values = true;
        } else {
            result = outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, &context);
        }
    }
    if (result.kind == XR_REFERENCE_OUTCOME_RETURN &&
        (result.value.kind == XR_REFERENCE_VALUE_EXISTENTIAL ||
         result.value.kind == XR_REFERENCE_VALUE_CALLABLE))
        result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, &context);
    if (result.kind == XR_REFERENCE_OUTCOME_ERROR &&
        result.error_value.kind == XR_REFERENCE_VALUE_AGGREGATE) {
        XrReferenceValue detached = void_value();
        if (detach_reference_value(result.error_value, &detached)) {
            result.error_value = detached;
            result.owns_dynamic_values = true;
        } else {
            result = outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, &context);
        }
    }
    if (result.kind == XR_REFERENCE_OUTCOME_ERROR &&
        (result.error_value.kind == XR_REFERENCE_VALUE_EXISTENTIAL ||
         result.error_value.kind == XR_REFERENCE_VALUE_CALLABLE))
        result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, &context);
    if (result.kind == XR_REFERENCE_OUTCOME_PANIC &&
        result.panic_value.kind != XR_REFERENCE_VALUE_PANIC_INFO)
        result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, &context);
    free_aggregates(&context);
    return result;
}

XrReferenceOutcome xr_reference_evaluate(const XrValidatedProgram *program, uint32_t function_id,
                                         const XrReferenceValue *arguments, uint32_t argument_count,
                                         const XrReferenceProfile *profile,
                                         const XrReferenceBudget *budget) {
    return xr_reference_evaluate_bound(program, function_id, arguments, argument_count, profile,
                                       budget, NULL);
}
