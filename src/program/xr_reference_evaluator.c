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
} EvalContext;

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

static bool reference_kind_matches_type(XrReferenceValueKind kind, uint16_t type_id) {
    switch (type_id) {
        case XR_CORE_TYPE_VOID:
            return kind == XR_REFERENCE_VALUE_VOID;
        case XR_CORE_TYPE_BOOL:
            return kind == XR_REFERENCE_VALUE_BOOL;
        case XR_CORE_TYPE_I64:
            return kind == XR_REFERENCE_VALUE_I64;
        case XR_CORE_TYPE_U32:
            return kind == XR_REFERENCE_VALUE_U32;
        case XR_CORE_TYPE_ERROR:
            return kind == XR_REFERENCE_VALUE_ERROR;
        default:
            return false;
    }
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
                                            const XrReferenceValue *arguments,
                                            uint32_t argument_count, uint32_t depth) {
    if (depth > context->budget.max_call_depth)
        return outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
    const XrValidatedFunction *function = &context->program->functions[function_id];
    if (argument_count != function->parameter_count)
        return outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
    for (uint32_t index = 0; index < argument_count; ++index) {
        if (!reference_kind_matches_type(arguments[index].kind, function->parameter_types[index]))
            return outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, context);
    }

    XrReferenceValue *values =
        xr_calloc(function->value_count ? function->value_count : 1u, sizeof(XrReferenceValue));
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
    XrReferenceValue *scratch =
        xr_calloc(scratch_count ? scratch_count : 1u, sizeof(XrReferenceValue));
    if (!values || !initialized || !scratch) {
        xr_free(scratch);
        xr_free(initialized);
        xr_free(values);
        return outcome(XR_REFERENCE_OUTCOME_RESOURCE_LIMIT, context);
    }

    uint32_t block_id = function->entry_block;
    uint32_t incoming_count = argument_count;
    if (incoming_count != 0)
        memcpy(scratch, arguments, (size_t) incoming_count * sizeof(XrReferenceValue));
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
            XrReferenceValue produced = void_value();
            bool has_result = instruction->result_id != XR_PROGRAM_LOCATION_NONE;
            switch (instruction->operation_id) {
                case XR_CORE_OP_CORE_CONSTANT_I64: {
                    const XrValidatedConstant *constant =
                        &context->program->constants[instruction->immediate.constant_id];
                    produced.kind = XR_REFERENCE_VALUE_I64;
                    produced.as.i64 = constant->value.i64;
                    break;
                }
                case XR_CORE_OP_CORE_CONSTANT_BOOL: {
                    const XrValidatedConstant *constant =
                        &context->program->constants[instruction->immediate.constant_id];
                    produced.kind = XR_REFERENCE_VALUE_BOOL;
                    produced.as.boolean = constant->value.boolean;
                    break;
                }
                case XR_CORE_OP_CORE_ADD_I64:
                case XR_CORE_OP_CORE_SUB_I64:
                case XR_CORE_OP_CORE_MUL_I64: {
                    int64_t left = values[instruction->operands[0]].as.i64;
                    int64_t right = values[instruction->operands[1]].as.i64;
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
                    produced.kind = XR_REFERENCE_VALUE_I64;
                    produced.as.i64 = exact;
                    break;
                }
                case XR_CORE_OP_CORE_DIV_I64: {
                    int64_t left = values[instruction->operands[0]].as.i64;
                    int64_t right = values[instruction->operands[1]].as.i64;
                    if (right == 0) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_INTEGER_DIVISION_BY_ZERO);
                        goto done;
                    }
                    if (left == INT64_MIN && right == -1) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_INTEGER_DIVISION_OVERFLOW);
                        goto done;
                    }
                    produced.kind = XR_REFERENCE_VALUE_I64;
                    produced.as.i64 = left / right;
                    break;
                }
                case XR_CORE_OP_CORE_COMPARE_I64: {
                    int64_t left = values[instruction->operands[0]].as.i64;
                    int64_t right = values[instruction->operands[1]].as.i64;
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
                    produced.kind = XR_REFERENCE_VALUE_BOOL;
                    produced.as.boolean = comparison;
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
                    bool condition = values[instruction->operands[0]].as.boolean;
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
                                       : values[instruction->operands[0]];
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
                    produced = nested.value;
                    break;
                }
                case XR_CORE_OP_CORE_TRAP:
                    result = trap_outcome(context, XR_REFERENCE_TRAP_EXPLICIT);
                    goto done;
                case XR_CORE_OP_CORE_ERROR_PUBLISH:
                    result = outcome(XR_REFERENCE_OUTCOME_ERROR, context);
                    result.error = values[instruction->operands[0]].as.error;
                    goto done;
                case XR_CORE_OP_CORE_TARGET_POINTER_WIDTH:
                    if (context->profile.pointer_width != 32u &&
                        context->profile.pointer_width != 64u) {
                        result = trap_outcome(context, XR_REFERENCE_TRAP_PROFILE_UNAVAILABLE);
                        goto done;
                    }
                    produced.kind = XR_REFERENCE_VALUE_U32;
                    produced.as.u32 = context->profile.pointer_width;
                    break;
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
    xr_free(values);
    return result;
}

XrReferenceBudget xr_reference_default_budget(void) {
    XrReferenceBudget budget = {.max_steps = UINT64_C(1000000), .max_call_depth = 1024u};
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
        selected.max_steps == 0 || selected.max_call_depth == 0)
        return outcome(XR_REFERENCE_OUTCOME_INVALID_INVOCATION, &context);
    return evaluate_function(&context, function_id, arguments, argument_count, 1u);
}
