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
#include "xr_validated_program_internal.h"

#include <limits.h>
#include <string.h>

typedef struct EvalContext {
    const XrValidatedProgram *program;
    XrReferenceProfile profile;
    XrReferenceBudget budget;
    uint64_t steps;
    uint64_t aggregate_cell_count;
    struct XrReferenceAggregateValue **aggregates;
    uint32_t aggregate_count;
    uint32_t aggregate_capacity;
} EvalContext;

typedef struct XrReferenceAggregateValue {
    uint16_t type_id;
    uint32_t variant_ordinal;
    XrReferenceValue *fields;
    uint32_t field_count;
} XrReferenceAggregateValue;

typedef struct EvalPlace {
    XrReferenceValue value;
    bool initialized;
} EvalPlace;

typedef struct EvalRuntimeValue {
    XrCoreIrValueCategory category;
    union {
        XrReferenceValue value;
        EvalPlace *place;
    } as;
} EvalRuntimeValue;

static XrReferenceOutcome outcome(XrReferenceOutcomeKind kind, EvalContext *context) {
    XrReferenceOutcome result = {.kind = kind, .steps = context->steps};
    return result;
}

static XrReferenceOutcome trap_outcome(EvalContext *context, XrReferenceTrap trap) {
    XrReferenceOutcome result = outcome(XR_REFERENCE_OUTCOME_TRAP, context);
    result.trap = trap;
    return result;
}

static XrReferenceValue void_value(void) {
    XrReferenceValue value = {.kind = XR_REFERENCE_VALUE_VOID};
    return value;
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
        case XR_CORE_TYPE_ERROR:
            return value.kind == XR_REFERENCE_VALUE_ERROR;
        case XR_CORE_TYPE_PANIC_INFO:
            return value.kind == XR_REFERENCE_VALUE_PANIC_INFO;
        default:
            return xr_validated_program_type(program, type_id) &&
                   value.kind == XR_REFERENCE_VALUE_AGGREGATE && value.as.aggregate &&
                   ((const XrReferenceAggregateValue *) value.as.aggregate)->type_id == type_id;
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

static bool clone_reference_value(EvalContext *context, XrReferenceValue source, uint16_t type_id,
                                  XrReferenceValue *output) {
    const XrValidatedType *type = xr_validated_program_type(context->program, type_id);
    if (!type) {
        *output = source;
        return true;
    }
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

static void free_aggregates(EvalContext *context) {
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
                                     ? arguments[index].as.place->value
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
                case XR_CORE_OP_CORE_CALL_SEALED_DIRECT: {
                    for (uint32_t index = 0; index < instruction->operand_count; ++index)
                        scratch[index] = values[instruction->operands[index]];
                    XrReferenceOutcome nested =
                        evaluate_function(context, instruction->immediate.function_id, scratch,
                                          instruction->operand_count, depth + 1u);
                    if (nested.kind != XR_REFERENCE_OUTCOME_RETURN) {
                        result = nested;
                        goto done;
                    }
                    produced.as.value = nested.value;
                    break;
                }
                case XR_CORE_OP_CORE_CALL_SEALED_INVOKE: {
                    const XrValidatedFunction *callee =
                        &context->program->functions[instruction->immediate.function_id];
                    for (uint32_t index = 0; index < callee->parameter_count; ++index)
                        scratch[index] = values[instruction->operands[index]];
                    XrReferenceOutcome nested =
                        evaluate_function(context, instruction->immediate.function_id, scratch,
                                          callee->parameter_count, depth + 1u);
                    uint32_t successor = 0u;
                    uint32_t implicit = 0u;
                    uint32_t operand = callee->parameter_count;
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
                    result = trap_outcome(context, XR_REFERENCE_TRAP_EXPLICIT);
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
                    produced.as.value.kind = XR_REFERENCE_VALUE_U32;
                    produced.as.value.as.u32 = context->profile.pointer_width;
                    break;
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
                    produced.as.value = values[instruction->operands[0]].as.place->value;
                    break;
                case XR_CORE_OP_CORE_PLACE_STORE:
                    values[instruction->operands[0]].as.place->value =
                        values[instruction->operands[1]].as.value;
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
                default:
                    result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
                    goto done;
            }
            if (has_result) {
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

XrReferenceBudget xr_reference_default_budget(void) {
    XrReferenceBudget budget = {
        .max_steps = UINT64_C(1000000),
        .max_value_cells = UINT64_C(1048576),
        .max_call_depth = 1024u,
    };
    return budget;
}

XrReferenceOutcome xr_reference_evaluate(const XrValidatedProgram *program, uint32_t function_id,
                                         const XrReferenceValue *arguments, uint32_t argument_count,
                                         const XrReferenceProfile *profile,
                                         const XrReferenceBudget *budget) {
    XrReferenceBudget selected = budget ? *budget : xr_reference_default_budget();
    EvalContext context = {
        .program = program,
        .profile = profile ? *profile : (XrReferenceProfile) {0},
        .budget = selected,
    };
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
        result.value.kind == XR_REFERENCE_VALUE_AGGREGATE)
        result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, &context);
    if (result.kind == XR_REFERENCE_OUTCOME_ERROR &&
        result.error_value.kind == XR_REFERENCE_VALUE_AGGREGATE)
        result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, &context);
    if (result.kind == XR_REFERENCE_OUTCOME_PANIC &&
        result.panic_value.kind != XR_REFERENCE_VALUE_PANIC_INFO)
        result = outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, &context);
    free_aggregates(&context);
    return result;
}
