/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_vm_dispatch.inc.c - Shared typed instruction handlers
 */

/* One transient instruction result, never an executable graph or an owner of
 * Program facts. Control edges retain the frame's parallel argument scratch. */
typedef struct XrVmDispatch {
    XrVmExecution *execution;
    XrVmInstructionView instruction;
    XrVmRuntimeValue produced;
    XrVmOutcome outcome;
    uint32_t instruction_id;
    uint32_t block_id;
    uint32_t incoming_count;
    bool transferred;
    bool edge_materialized;
    bool terminal;
    bool returned;
} XrVmDispatch;

static void vm_dispatch_publish_outcome(XrVmDispatch *dispatch, XrVmOutcome outcome) {
    dispatch->outcome = outcome;
    dispatch->returned = true;
}

static void vm_dispatch_constants(XrVmDispatch *dispatch) {
    XrVmExecution *execution = dispatch->execution;
    XrVmInstructionView instruction = dispatch->instruction;
    XrVmContext *context = &execution->context;
    XrVmRuntimeValue produced = dispatch->produced;
    XrVmOutcome result = dispatch->outcome;
    dispatch->terminal = true;
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
        case XR_CORE_OP_CORE_CONSTANT_TARGET_ENUM:
            produced.as.value.kind =
                instruction.result_type_id == XR_CORE_TYPE_TARGET_OS ? XR_VM_VALUE_TARGET_OS
                : instruction.result_type_id == XR_CORE_TYPE_TARGET_ARCH
                    ? XR_VM_VALUE_TARGET_ARCH
                : instruction.result_type_id == XR_CORE_TYPE_TARGET_ABI
                    ? XR_VM_VALUE_TARGET_ABI
                    : XR_VM_VALUE_TARGET_ENDIAN;
            produced.as.value.as.target_enum = (uint16_t) instruction.immediate.u32;
            break;
        case XR_CORE_OP_CORE_TARGET_POINTER_WIDTH:
            if (context->code->pointer_width != 32u &&
                context->code->pointer_width != 64u) {
                result = vm_trap(XR_VM_TRAP_PROFILE_UNAVAILABLE, context);
                goto done;
            }
            produced.as.value.kind = XR_VM_VALUE_U16;
            produced.as.value.as.u16 = context->code->pointer_width;
            break;
        case XR_CORE_OP_CORE_TARGET_OPERATING_SYSTEM:
            if (context->code->operating_system <= XR_TARGET_OS_NONE ||
                context->code->operating_system >= XR_TARGET_OS_COUNT) {
                result = vm_trap(XR_VM_TRAP_PROFILE_UNAVAILABLE, context);
                goto done;
            }
            produced.as.value.kind = XR_VM_VALUE_TARGET_OS;
            produced.as.value.as.target_enum = context->code->operating_system;
            break;
        case XR_CORE_OP_CORE_TARGET_ARCHITECTURE:
            if (context->code->architecture <= XR_TARGET_ARCH_NONE ||
                context->code->architecture >= XR_TARGET_ARCH_COUNT) {
                result = vm_trap(XR_VM_TRAP_PROFILE_UNAVAILABLE, context);
                goto done;
            }
            produced.as.value.kind = XR_VM_VALUE_TARGET_ARCH;
            produced.as.value.as.target_enum = context->code->architecture;
            break;
        case XR_CORE_OP_CORE_TARGET_NATIVE_ABI:
            if (context->code->native_abi <= XR_TARGET_ABI_NONE ||
                context->code->native_abi >= XR_TARGET_ABI_COUNT) {
                result = vm_trap(XR_VM_TRAP_PROFILE_UNAVAILABLE, context);
                goto done;
            }
            produced.as.value.kind = XR_VM_VALUE_TARGET_ABI;
            produced.as.value.as.target_enum = context->code->native_abi;
            break;
        case XR_CORE_OP_CORE_TARGET_ENDIANNESS:
            if (context->code->endianness != XR_TARGET_ENDIAN_LITTLE &&
                context->code->endianness != XR_TARGET_ENDIAN_BIG) {
                result = vm_trap(XR_VM_TRAP_PROFILE_UNAVAILABLE, context);
                goto done;
            }
            produced.as.value.kind = XR_VM_VALUE_TARGET_ENDIAN;
            produced.as.value.as.target_enum = context->code->endianness;
            break;
        default:
            goto done;
    }
    dispatch->terminal = false;
done:
    dispatch->produced = produced;
    dispatch->outcome = result;
}

static void vm_dispatch_arithmetic(XrVmDispatch *dispatch) {
    XrVmExecution *execution = dispatch->execution;
    XrVmInstructionView instruction = dispatch->instruction;
    XrVmContext *context = &execution->context;
    XrVmRuntimeValue *values = execution->values;
    XrVmRuntimeValue produced = dispatch->produced;
    XrVmOutcome result = dispatch->outcome;
    dispatch->terminal = true;
    switch (instruction.operation_id) {
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
        default:
            goto done;
    }
    dispatch->terminal = false;
done:
    dispatch->produced = produced;
    dispatch->outcome = result;
}

static void vm_dispatch_logic(XrVmDispatch *dispatch) {
    XrVmExecution *execution = dispatch->execution;
    XrVmInstructionView instruction = dispatch->instruction;
    XrVmRuntimeValue *values = execution->values;
    XrVmRuntimeValue produced = dispatch->produced;
    dispatch->terminal = true;
    switch (instruction.operation_id) {
        case XR_CORE_OP_CORE_LOGICAL_NOT:
            produced.as.value.kind = XR_VM_VALUE_BOOL;
            produced.as.value.as.boolean =
                !values[instruction.operands[0]].as.value.as.boolean;
            break;
        case XR_CORE_OP_CORE_LOGICAL_AND:
        case XR_CORE_OP_CORE_LOGICAL_OR: {
            bool left = values[instruction.operands[0]].as.value.as.boolean;
            bool right = values[instruction.operands[1]].as.value.as.boolean;
            produced.as.value.kind = XR_VM_VALUE_BOOL;
            produced.as.value.as.boolean =
                instruction.operation_id == XR_CORE_OP_CORE_LOGICAL_AND ? left && right
                                                                        : left || right;
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
        case XR_CORE_OP_CORE_COMPARE_TARGET_ENUM: {
            uint16_t left = values[instruction.operands[0]].as.value.as.target_enum;
            uint16_t right = values[instruction.operands[1]].as.value.as.target_enum;
            produced.as.value.kind = XR_VM_VALUE_BOOL;
            produced.as.value.as.boolean =
                instruction.immediate.u32 == 0u ? left == right : left != right;
            break;
        }
        default:
            goto done;
    }
    dispatch->terminal = false;
done:
    dispatch->produced = produced;
}

static void vm_dispatch_control(XrVmDispatch *dispatch) {
    XrVmExecution *execution = dispatch->execution;
    XrVmInstructionView instruction = dispatch->instruction;
    XrVmContext *context = &execution->context;
    const XrValidatedFunction *function =
        &execution->context.code->program->functions[execution->function_id];
    XrVmRuntimeValue *values = execution->values;
    XrVmRuntimeValue *scratch = execution->edge_values;
    XrVmOutcome result = dispatch->outcome;
    uint32_t block_id = dispatch->block_id;
    uint32_t incoming_count = dispatch->incoming_count;
    bool transferred = dispatch->transferred;
    dispatch->terminal = true;
    switch (instruction.operation_id) {
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
        case XR_CORE_OP_CORE_ASSERT_CONDITION:
            if (!values[instruction.operands[0]].as.value.as.boolean) {
                XrVmValue panic = {
                    .kind = XR_VM_VALUE_PANIC_INFO,
                    .as.panic_info = instruction.immediate.u32,
                };
                if (instruction.successor_count == 0u) {
                    result = vm_outcome(XR_VM_OUTCOME_PANIC, context);
                    result.panic_value = panic;
                    goto done;
                }
                const XrValidatedBlock *target =
                    &function->blocks[instruction.successors[0]];
                scratch[0] = (XrVmRuntimeValue) {
                    .category = XR_CORE_IR_VALUE,
                    .as.value = panic,
                };
                for (uint32_t index = 1u; index < target->argument_count; ++index)
                    scratch[index] = values[instruction.operands[index]];
                incoming_count = target->argument_count;
                block_id = instruction.successors[0];
                transferred = true;
            }
            break;
        case XR_CORE_OP_CORE_RETURN:
            result = vm_outcome(XR_VM_OUTCOME_RETURN, context);
            result.value = instruction.operand_count == 0u
                               ? void_value()
                               : values[instruction.operands[0]].as.value;
            goto done;
        case XR_CORE_OP_CORE_TRAP:
            result =
                vm_trap(instruction.immediate.u32 == 7u ? XR_VM_TRAP_PROVIDER_CALL_FAILED
                                                        : XR_VM_TRAP_EXPLICIT,
                        context);
            goto done;
        case XR_CORE_OP_CORE_ERROR_PUBLISH:
            result = vm_outcome(XR_VM_OUTCOME_ERROR, context);
            result.error_value = values[instruction.operands[0]].as.value;
            goto done;
        case XR_CORE_OP_CORE_PANIC_PUBLISH:
            result = vm_outcome(XR_VM_OUTCOME_PANIC, context);
            result.panic_value = values[instruction.operands[0]].as.value;
            goto done;
        case XR_CORE_OP_CORE_CANCEL_PUBLISH:
            result = vm_outcome(XR_VM_OUTCOME_CANCELLED, context);
            goto done;
        default:
            goto done;
    }
    dispatch->terminal = false;
done:
    dispatch->outcome = result;
    dispatch->block_id = block_id;
    dispatch->incoming_count = incoming_count;
    dispatch->transferred = transferred;
}

static void vm_dispatch_direct_call(XrVmDispatch *dispatch) {
    XrVmExecution *execution = dispatch->execution;
    XrVmInstructionView instruction = dispatch->instruction;
    XrVmContext *context = &execution->context;
    const XrValidatedFunction *function =
        &execution->context.code->program->functions[execution->function_id];
    uint32_t depth = execution->depth;
    XrVmRuntimeValue *values = execution->values;
    XrVmRuntimeValue *scratch = execution->edge_values;
    XrVmRuntimeValue produced = dispatch->produced;
    XrVmOutcome result = dispatch->outcome;
    uint32_t block_id = dispatch->block_id;
    uint32_t incoming_count = dispatch->incoming_count;
    bool transferred = dispatch->transferred;
    dispatch->terminal = true;
    switch (instruction.operation_id) {
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
            uint32_t call_operand_count = instruction.operand_count;
            if (instruction.successor_count == 1u)
                call_operand_count -=
                    function->blocks[instruction.successors[0]].argument_count;
            for (; source_argument < call_operand_count;
                 ++source_argument, ++target_argument)
                scratch[target_argument] = values[instruction.operands[source_argument]];
            const XrValidatedFunction *callee =
                &context->code->program->functions[target_function];
            XrVmOutcome nested = execute_function(context, target_function, scratch,
                                                  callee->parameter_count, depth + 1u);
            if (nested.kind != XR_VM_OUTCOME_RETURN) {
                if (nested.kind == XR_VM_OUTCOME_TRAP &&
                    nested.trap == XR_VM_TRAP_PROVIDER_CALL_FAILED &&
                    instruction.successor_count == 1u) {
                    const XrValidatedBlock *target =
                        &function->blocks[instruction.successors[0]];
                    for (uint32_t index = 0u; index < target->argument_count; ++index)
                        scratch[index] =
                            values[instruction.operands[call_operand_count + index]];
                    incoming_count = target->argument_count;
                    block_id = instruction.successors[0];
                    transferred = true;
                    break;
                }
                result = nested;
                goto done;
            }
            produced.as.value = nested.value;
            break;
        }
        default:
            goto done;
    }
    dispatch->terminal = false;
done:
    dispatch->produced = produced;
    dispatch->outcome = result;
    dispatch->block_id = block_id;
    dispatch->incoming_count = incoming_count;
    dispatch->transferred = transferred;
}

static void vm_dispatch_invoke(XrVmDispatch *dispatch) {
    XrVmExecution *execution = dispatch->execution;
    XrVmInstructionView instruction = dispatch->instruction;
    XrVmContext *context = &execution->context;
    const XrValidatedFunction *function =
        &execution->context.code->program->functions[execution->function_id];
    uint32_t depth = execution->depth;
    XrVmRuntimeValue *values = execution->values;
    XrVmRuntimeValue *scratch = execution->edge_values;
    XrVmOutcome result = dispatch->outcome;
    uint32_t block_id = dispatch->block_id;
    uint32_t incoming_count = dispatch->incoming_count;
    bool transferred = dispatch->transferred;
    dispatch->terminal = true;
    switch (instruction.operation_id) {
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
            uint32_t typed_successors =
                1u + (callee->error_type_id == XR_CORE_TYPE_VOID ? 0u : 1u) +
                (callee->panic_type_id == XR_CORE_TYPE_VOID ? 0u : 1u);
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
            } else if ((instruction.operation_id == XR_CORE_OP_CORE_CALL_WITNESS_INVOKE ||
                        instruction.operation_id == XR_CORE_OP_CORE_CALL_SEALED_INVOKE ||
                        instruction.operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE) &&
                       nested.kind == XR_VM_OUTCOME_TRAP &&
                       nested.trap == XR_VM_TRAP_PROVIDER_CALL_FAILED &&
                       instruction.successor_count == typed_successors + 1u) {
                successor = typed_successors;
                for (uint32_t prior = 0u; prior < typed_successors; ++prior) {
                    uint32_t prior_implicit =
                        prior == 0u
                            ? (callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u)
                            : 1u;
                    const XrValidatedBlock *prior_target =
                        &function->blocks[instruction.successors[prior]];
                    operand += prior_target->argument_count - prior_implicit;
                }
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
        default:
            goto done;
    }
    dispatch->terminal = false;
done:
    dispatch->outcome = result;
    dispatch->block_id = block_id;
    dispatch->incoming_count = incoming_count;
    dispatch->transferred = transferred;
}

static void vm_dispatch_provider_text(XrVmDispatch *dispatch) {
    XrVmExecution *execution = dispatch->execution;
    XrVmInstructionView instruction = dispatch->instruction;
    XrVmContext *context = &execution->context;
    const XrValidatedFunction *function =
        &execution->context.code->program->functions[execution->function_id];
    XrVmRuntimeValue *values = execution->values;
    XrVmRuntimeValue *scratch = execution->edge_values;
    XrVmRuntimeValue produced = dispatch->produced;
    XrVmOutcome result = dispatch->outcome;
    uint32_t block_id = dispatch->block_id;
    uint32_t incoming_count = dispatch->incoming_count;
    bool transferred = dispatch->transferred;
    dispatch->terminal = true;
    switch (instruction.operation_id) {
        case XR_CORE_OP_CORE_PROVIDER_CALL: {
            bool has_trap_edge = instruction.successor_count != 0u;
            const XrValidatedBlock *trap_target =
                has_trap_edge ? &function->blocks[instruction.successors[0]] : NULL;
            uint32_t provider_operand_count =
                instruction.operand_count -
                (trap_target ? trap_target->argument_count : 0u);
            XrVmOutcome provider = vm_provider_call(context, function, &instruction, values,
                                                    provider_operand_count);
            if (provider.kind != XR_VM_OUTCOME_RETURN) {
                if (provider.kind != XR_VM_OUTCOME_TRAP ||
                    provider.trap != XR_VM_TRAP_PROVIDER_CALL_FAILED || !has_trap_edge) {
                    result = provider;
                    goto done;
                }
                for (uint32_t index = 0u; index < trap_target->argument_count; ++index)
                    scratch[index] =
                        values[instruction.operands[provider_operand_count + index]];
                incoming_count = trap_target->argument_count;
                block_id = instruction.successors[0];
                transferred = true;
            } else {
                produced.as.value = provider.value;
            }
            break;
        }
        case XR_CORE_OP_CORE_CONSTANT_STRING:
        case XR_CORE_OP_CORE_CONSTANT_RUNE:
        case XR_CORE_OP_CORE_STRING_FROM_I64:
        case XR_CORE_OP_CORE_STRING_CONCAT:
        case XR_CORE_OP_CORE_COMPARE_STRING:
        case XR_CORE_OP_CORE_COMPARE_RUNE:
        case XR_CORE_OP_CORE_OUTPUT_GROUP: {
            VmTextStatus text_status = vm_execute_text_operation(
                context, instruction.operation_id, instruction.operands,
                instruction.operand_count, instruction.immediate.constant_id,
                instruction.immediate.u32,
                instruction.immediate.provider_operation.requirement_index,
                instruction.immediate.provider_operation.operation_index, values,
                &produced.as.value);
            if (text_status != VM_TEXT_OK) {
                result = vm_text_outcome(context, text_status);
                goto done;
            }
            break;
        }
        default:
            goto done;
    }
    dispatch->terminal = false;
done:
    dispatch->produced = produced;
    dispatch->outcome = result;
    dispatch->block_id = block_id;
    dispatch->incoming_count = incoming_count;
    dispatch->transferred = transferred;
}

static void vm_dispatch_owner_callable(XrVmDispatch *dispatch) {
    XrVmExecution *execution = dispatch->execution;
    XrVmInstructionView instruction = dispatch->instruction;
    XrVmContext *context = &execution->context;
    const XrValidatedFunction *function =
        &execution->context.code->program->functions[execution->function_id];
    XrVmRuntimeValue *values = execution->values;
    XrVmRuntimeValue produced = dispatch->produced;
    XrVmOutcome result = dispatch->outcome;
    dispatch->terminal = true;
    switch (instruction.operation_id) {
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
            drop_vm_value(context, &values[instruction.operands[0]].as.value,
                          XR_VM_EVENT_ORIGIN_PROGRAM_OPERATION);
            break;
        default:
            goto done;
    }
    dispatch->terminal = false;
done:
    dispatch->produced = produced;
    dispatch->outcome = result;
}

static void vm_dispatch_class_owner(XrVmDispatch *dispatch) {
    XrVmExecution *execution = dispatch->execution;
    XrVmInstructionView instruction = dispatch->instruction;
    XrVmContext *context = &execution->context;
    XrVmRuntimeValue *values = execution->values;
    XrVmRuntimeValue produced = dispatch->produced;
    XrVmOutcome result = dispatch->outcome;
    dispatch->terminal = true;
    switch (instruction.operation_id) {
        case XR_CORE_OP_CORE_CLASS_CONSTRUCT: {
            XrVmClassValue *instance = allocate_class(
                context, instruction.result_type_id, instruction.operand_count);
            if (!instance) {
                result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                goto done;
            }
            for (uint32_t field = 0u; field < instruction.operand_count; ++field)
                instance->fields[field] = values[instruction.operands[field]].as.value;
            produced.as.value.kind = XR_VM_VALUE_CLASS_REFERENCE;
            produced.as.value.as.class_reference = instance;
            emit_lifecycle(context, XR_VM_EVENT_CLASS_CONSTRUCT,
                           XR_VM_EVENT_ORIGIN_PROGRAM_OPERATION, instance, UINT64_MAX,
                           UINT32_MAX);
            break;
        }
        case XR_CORE_OP_CORE_CLASS_SHARE: {
            XrVmValue source = values[instruction.operands[0]].as.value;
            XrVmClassValue *instance =
                source.kind == XR_VM_VALUE_CLASS_REFERENCE
                    ? (XrVmClassValue *) (void *) source.as.class_reference
                    : NULL;
            if (!class_value_is_live(instance, instruction.result_type_id) ||
                instance->owner_count == UINT32_MAX) {
                result = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
                goto done;
            }
            ++instance->owner_count;
            produced.as.value.kind = XR_VM_VALUE_CLASS_REFERENCE;
            produced.as.value.as.class_reference = instance;
            emit_lifecycle(context, XR_VM_EVENT_CLASS_SHARE,
                           XR_VM_EVENT_ORIGIN_PROGRAM_OPERATION, instance,
                           instance->identity, UINT32_MAX);
            break;
        }
        default:
            goto done;
    }
    dispatch->terminal = false;
done:
    dispatch->produced = produced;
    dispatch->outcome = result;
}

static void vm_dispatch_class_field(XrVmDispatch *dispatch) {
    XrVmExecution *execution = dispatch->execution;
    XrVmInstructionView instruction = dispatch->instruction;
    XrVmContext *context = &execution->context;
    XrVmRuntimeValue *values = execution->values;
    XrVmPlace *places = execution->places;
    XrVmRuntimeValue produced = dispatch->produced;
    XrVmOutcome result = dispatch->outcome;
    dispatch->terminal = true;
    switch (instruction.operation_id) {
        case XR_CORE_OP_CORE_CLASS_FIELD_LOAD: {
            XrVmValue source = values[instruction.operands[0]].as.value;
            XrVmClassValue *instance =
                source.kind == XR_VM_VALUE_CLASS_REFERENCE
                    ? (XrVmClassValue *) (void *) source.as.class_reference
                    : NULL;
            if (!class_field_load_value(context, instance,
                                        instruction.immediate.field_ordinal,
                                        instruction.result_type_id, &produced.as.value)) {
                result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                goto done;
            }
            emit_lifecycle(context, XR_VM_EVENT_CLASS_FIELD_LOAD,
                           XR_VM_EVENT_ORIGIN_PROGRAM_OPERATION, instance, UINT64_MAX,
                           instruction.immediate.field_ordinal);
            break;
        }
        case XR_CORE_OP_CORE_CLASS_FIELD_PLACE: {
            XrVmValue source = values[instruction.operands[0]].as.value;
            XrVmClassValue *instance =
                source.kind == XR_VM_VALUE_CLASS_REFERENCE
                    ? (XrVmClassValue *) (void *) source.as.class_reference
                    : NULL;
            const XrValidatedType *type =
                instance ? xr_validated_program_type(context->code->program,
                                                     instance->type_id)
                         : NULL;
            uint32_t field = instruction.immediate.field_ordinal;
            if (!type || type->kind != XR_CORE_IR_TYPE_CLASS_REFERENCE ||
                !class_value_is_live(instance, instance->type_id) ||
                field >= type->field_count || field >= instance->field_count ||
                type->field_types[field] != instruction.result_type_id) {
                result = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
                goto done;
            }
            places[instruction.result_id].alias = &instance->fields[field];
            places[instruction.result_id].initialized = true;
            produced.category = XR_CORE_IR_PLACE;
            produced.as.place = &places[instruction.result_id];
            emit_lifecycle(context, XR_VM_EVENT_CLASS_FIELD_PLACE,
                           XR_VM_EVENT_ORIGIN_PROGRAM_OPERATION, instance, UINT64_MAX,
                           field);
            break;
        }
        default:
            goto done;
    }
    dispatch->terminal = false;
done:
    dispatch->produced = produced;
    dispatch->outcome = result;
}

static void vm_dispatch_place(XrVmDispatch *dispatch) {
    XrVmExecution *execution = dispatch->execution;
    XrVmInstructionView instruction = dispatch->instruction;
    XrVmContext *context = &execution->context;
    XrVmRuntimeValue *values = execution->values;
    XrVmPlace *places = execution->places;
    XrVmRuntimeValue produced = dispatch->produced;
    XrVmOutcome result = dispatch->outcome;
    dispatch->terminal = true;
    switch (instruction.operation_id) {
        case XR_CORE_OP_CORE_PLACE_LOCAL:
            places[instruction.result_id].alias = &values[instruction.operands[0]].as.value;
            places[instruction.result_id].initialized = true;
            produced.category = XR_CORE_IR_PLACE;
            produced.as.place = &places[instruction.result_id];
            break;
        case XR_CORE_OP_CORE_PLACE_LOAD:
            produced.as.value = *vm_place_value(values[instruction.operands[0]].as.place);
            break;
        case XR_CORE_OP_CORE_PLACE_STORE:
            *vm_place_value(values[instruction.operands[0]].as.place) =
                values[instruction.operands[1]].as.value;
            break;
        case XR_CORE_OP_CORE_PLACE_PROJECT: {
            XrVmValue *source = vm_place_value(values[instruction.operands[0]].as.place);
            XrVmAggregateValue *aggregate =
                (XrVmAggregateValue *) (void *) source->as.aggregate;
            places[instruction.result_id].alias =
                &aggregate->fields[instruction.immediate.field_ordinal];
            places[instruction.result_id].initialized = true;
            produced.category = XR_CORE_IR_PLACE;
            produced.as.place = &places[instruction.result_id];
            break;
        }
        case XR_CORE_OP_CORE_PLACE_TAKE:
            produced.as.value = *vm_place_value(values[instruction.operands[0]].as.place);
            values[instruction.operands[0]].as.place->initialized = false;
            break;
        case XR_CORE_OP_CORE_PLACE_EXCHANGE: {
            XrVmValue *place =
                vm_place_value(values[instruction.operands[0]].as.place);
            if (!place) {
                result = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
                goto done;
            }
            produced.as.value = *place;
            XrVmValue replacement = values[instruction.operands[1]].as.value;
            *place = replacement;
            emit_place_exchange(context, instruction.result_type_id, produced.as.value,
                                replacement);
            break;
        }
        default:
            goto done;
    }
    dispatch->terminal = false;
done:
    dispatch->produced = produced;
    dispatch->outcome = result;
}

static void vm_dispatch_aggregate_variant(XrVmDispatch *dispatch) {
    XrVmExecution *execution = dispatch->execution;
    XrVmInstructionView instruction = dispatch->instruction;
    XrVmContext *context = &execution->context;
    XrVmRuntimeValue *values = execution->values;
    XrVmRuntimeValue produced = dispatch->produced;
    XrVmOutcome result = dispatch->outcome;
    dispatch->terminal = true;
    switch (instruction.operation_id) {
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
        default:
            goto done;
    }
    dispatch->terminal = false;
done:
    dispatch->produced = produced;
    dispatch->outcome = result;
}

static void vm_dispatch_existential(XrVmDispatch *dispatch) {
    XrVmExecution *execution = dispatch->execution;
    XrVmInstructionView instruction = dispatch->instruction;
    XrVmContext *context = &execution->context;
    const XrValidatedFunction *function =
        &execution->context.code->program->functions[execution->function_id];
    XrVmRuntimeValue *values = execution->values;
    XrVmRuntimeValue produced = dispatch->produced;
    XrVmOutcome result = dispatch->outcome;
    dispatch->terminal = true;
    switch (instruction.operation_id) {
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
        case XR_CORE_OP_CORE_EXISTENTIAL_REBORROW_READ: {
            const XrVmExistentialValue *source =
                values[instruction.operands[0]].as.value.as.existential;
            XrVmExistentialValue *carrier = allocate_existential(context);
            if (!source || !carrier) {
                result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                goto done;
            }
            carrier->existential_type_id = instruction.result_type_id;
            carrier->concrete_type_id = source->concrete_type_id;
            carrier->conformance_id = source->conformance_id;
            carrier->payload.category = XR_CORE_IR_VALUE;
            carrier->payload.as.value =
                source->payload.category == XR_CORE_IR_PLACE
                    ? *vm_place_value_const(source->payload.as.place)
                    : source->payload.as.value;
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
                produced.as.value = *vm_place_value_const(&carrier->owned_storage);
            } else {
                produced = carrier->payload;
            }
            break;
        }
        default:
            goto done;
    }
    dispatch->terminal = false;
done:
    dispatch->produced = produced;
    dispatch->outcome = result;
}

static void vm_dispatch_suspension(XrVmDispatch *dispatch) {
    XrVmExecution *execution = dispatch->execution;
    XrVmInstructionView instruction = dispatch->instruction;
    const XrValidatedFunction *function =
        &execution->context.code->program->functions[execution->function_id];
    uint32_t instruction_id = dispatch->instruction_id;
    dispatch->terminal = true;
    switch (instruction.operation_id) {
        case XR_CORE_OP_CORE_COROUTINE_YIELD: {
            uint32_t safepoint_id = instruction.immediate.u32;
            const XrValidatedCoroutineSafepoint *safepoint =
                &function->coroutine_safepoints[safepoint_id];
            execution->suspension_block_id = execution->block_id;
            execution->suspension_instruction_id = instruction_id;
            execution->state_id = safepoint->resume_state_id;
            execution->block_id = instruction.successors[0];
            execution->instruction_id = 0u;
            execution->suspended = true;
            execution->cancel_block_id = instruction.successors[1];
            XrVmOutcome suspended_result = vm_execution_outcome(execution, XR_VM_OUTCOME_SUSPENDED);
            suspended_result.safepoint_id = safepoint_id;
            suspended_result.suspension.kind = XR_SUSPENSION_REQUEST_COOPERATIVE_YIELD;
            vm_dispatch_publish_outcome(dispatch, suspended_result);
            return;
        }
        case XR_CORE_OP_CORE_COROUTINE_SUSPEND: {
            uint32_t safepoint_id = instruction.immediate.coroutine_suspend.safepoint_id;
            uint32_t request_value = instruction.operands[0];
            const XrValidatedCoroutineSafepoint *safepoint =
                &function->coroutine_safepoints[safepoint_id];
            execution->suspension_block_id = execution->block_id;
            execution->suspension_instruction_id = instruction_id;
            execution->state_id = safepoint->resume_state_id;
            execution->block_id = instruction.successors[0];
            execution->instruction_id = 0u;
            execution->suspended = true;
            execution->cancel_block_id = instruction.successors[1];
            XrVmOutcome suspended_result = vm_execution_outcome(execution, XR_VM_OUTCOME_SUSPENDED);
            suspended_result.safepoint_id = safepoint_id;
            suspended_result.suspension.kind = XR_SUSPENSION_REQUEST_TIMER_AFTER_MS;
            suspended_result.suspension.operand_count = 1u;
            suspended_result.suspension.payload.timer_after_ms = xr_suspension_timer_normalize_ms(
                execution->values[request_value].as.value.as.i64);
            vm_dispatch_publish_outcome(dispatch, suspended_result);
            return;
        }
        default:
            return;
    }
}

static void vm_dispatch_coroutine_call(XrVmDispatch *dispatch) {
    XrVmExecution *execution = dispatch->execution;
    XrVmInstructionView instruction = dispatch->instruction;
    const XrValidatedFunction *function =
        &execution->context.code->program->functions[execution->function_id];
    uint32_t instruction_id = dispatch->instruction_id;
    uint32_t block_id = dispatch->block_id;
    bool transferred = dispatch->transferred;
    bool edge_materialized = dispatch->edge_materialized;
    dispatch->terminal = true;
    switch (instruction.operation_id) {
        case XR_CORE_OP_CORE_COROUTINE_CALL_SEALED:
        case XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT: {
            bool indirect = instruction.operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT;
            uint32_t callee_id = instruction.immediate.coroutine_call.function_id;
            uint32_t safepoint_id = indirect
                                        ? instruction.immediate.u32
                                        : instruction.immediate.coroutine_call.safepoint_id;
            const XrValidatedCoroutineSafepoint *safepoint =
                &function->coroutine_safepoints[safepoint_id];
            if (!execution->child &&
                !vm_child_execution_create(execution, &instruction, &execution->child)) {
                execution->finished = true;
                vm_execution_release_lease(execution);
                vm_dispatch_publish_outcome(dispatch, vm_execution_outcome(execution, XR_VM_OUTCOME_RESOURCE_LIMIT));
                return;
            }
            callee_id = execution->child->function_id;
            const XrValidatedFunction *callee =
                &execution->context.code->program->functions[callee_id];
            uint64_t child_steps = execution->child->context.steps;
            XrVmOutcome child = vm_execution_step(execution->child);
            uint64_t child_delta = execution->child->context.steps - child_steps;
            if (child_delta >
                execution->context.code->options.max_steps - execution->context.steps) {
                execution->finished = true;
                xr_vm_execution_free(execution->child);
                execution->child = NULL;
                vm_execution_release_lease(execution);
                vm_dispatch_publish_outcome(dispatch, vm_execution_outcome(execution, XR_VM_OUTCOME_RESOURCE_LIMIT));
                return;
            }
            execution->context.steps += child_delta;
            if (child.kind == XR_VM_OUTCOME_SUSPENDED) {
                execution->suspension_block_id = execution->block_id;
                execution->suspension_instruction_id = instruction_id;
                --execution->instruction_id;
                execution->state_id = safepoint->resume_state_id;
                execution->suspended = true;
                execution->cancel_block_id = instruction.successors[1];
                XrVmOutcome suspended =
                    vm_execution_outcome(execution, XR_VM_OUTCOME_SUSPENDED);
                suspended.safepoint_id = safepoint_id;
                suspended.suspension = child.suspension;
                vm_dispatch_publish_outcome(dispatch, suspended);
                return;
            }
            if (!vm_adopt_child_storage(execution)) {
                execution->finished = true;
                xr_vm_execution_free(execution->child);
                execution->child = NULL;
                vm_execution_release_lease(execution);
                vm_dispatch_publish_outcome(dispatch, vm_execution_outcome(execution, XR_VM_OUTCOME_RESOURCE_LIMIT));
                return;
            }
            if (child.kind != XR_VM_OUTCOME_RETURN) {
                if (child.kind == XR_VM_OUTCOME_TRAP &&
                    child.trap == XR_VM_TRAP_PROVIDER_CALL_FAILED &&
                    instruction.successor_count == 3u) {
                    execution->suspension_block_id = execution->block_id;
                    execution->suspension_instruction_id = instruction_id;
                    bool edge_valid = vm_materialize_suspension_edge(execution, 2u);
                    xr_vm_execution_free(execution->child);
                    execution->child = NULL;
                    execution->suspension_block_id = XR_PROGRAM_LOCATION_NONE;
                    execution->suspension_instruction_id = XR_PROGRAM_LOCATION_NONE;
                    if (!edge_valid) {
                        execution->finished = true;
                        vm_execution_release_lease(execution);
                        vm_dispatch_publish_outcome(dispatch, vm_execution_outcome(execution,
                                                    XR_VM_OUTCOME_INVALID_INVOCATION));
                        return;
                    }
                    execution->instruction_id = 0u;
                    block_id = execution->block_id;
                    transferred = true;
                    edge_materialized = true;
                    break;
                }
                execution->finished = true;
                xr_vm_execution_free(execution->child);
                execution->child = NULL;
                vm_execution_release_lease(execution);
                child.steps = execution->context.steps;
                child.state_id = execution->state_id;
                vm_dispatch_publish_outcome(dispatch, child);
                return;
            }
            const XrValidatedBlock *normal = &function->blocks[instruction.successors[0]];
            uint32_t implicit_result = callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
            uint32_t parameter_prefix = callee->parameter_count;
            if (indirect) {
                const XrVmCallableValue *carrier =
                    execution->values[instruction.operands[0]].as.value.as.callable;
                const XrValidatedType *callable =
                    carrier ? xr_validated_program_type(execution->context.code->program,
                                                        carrier->callable_type_id)
                            : NULL;
                if (!callable || callable->signature_id >=
                                     execution->context.code->program->signature_count) {
                    execution->finished = true;
                    xr_vm_execution_free(execution->child);
                    execution->child = NULL;
                    vm_execution_release_lease(execution);
                    vm_dispatch_publish_outcome(dispatch, vm_execution_outcome(execution, XR_VM_OUTCOME_INVALID_INVOCATION));
                    return;
                }
                parameter_prefix =
                    execution->context.code->program->signatures[callable->signature_id]
                        .parameter_count +
                    1u;
            }
            vm_execution_assign_edge(execution, normal, instruction.operands, parameter_prefix,
                                     implicit_result ? &child.value : NULL);
            xr_vm_execution_free(execution->child);
            execution->child = NULL;
            execution->block_id = instruction.successors[0];
            execution->instruction_id = 0u;
            block_id = execution->block_id;
            transferred = true;
            edge_materialized = true;
            break;
        }
        default:
            goto done;
    }
    dispatch->terminal = false;
done:
    dispatch->block_id = block_id;
    dispatch->transferred = transferred;
    dispatch->edge_materialized = edge_materialized;
}
