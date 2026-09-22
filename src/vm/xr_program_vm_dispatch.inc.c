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
    bool terminal;
    bool returned;
    bool call_entered;
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
        case XR_CORE_OP_CORE_CONSTANT_F64: {
            const XrValidatedConstant *constant =
                &context->code->program->constants[instruction.immediate.constant_id];
            produced.as.value.kind = XR_VM_VALUE_F64;
            produced.as.value.as.f64_bits = constant->value.f64_bits;
            break;
        }
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

static bool vm_integer_value_bits(XrVmValue value, uint64_t *bits) {
    switch (value.kind) {
        case XR_VM_VALUE_I8: *bits = (uint64_t) value.as.i8; return true;
        case XR_VM_VALUE_U8: *bits = value.as.u8; return true;
        case XR_VM_VALUE_I16: *bits = (uint64_t) value.as.i16; return true;
        case XR_VM_VALUE_U16: *bits = value.as.u16; return true;
        case XR_VM_VALUE_I32: *bits = (uint64_t) value.as.i32; return true;
        case XR_VM_VALUE_U32: *bits = value.as.u32; return true;
        case XR_VM_VALUE_I64: *bits = (uint64_t) value.as.i64; return true;
        case XR_VM_VALUE_U64: *bits = value.as.u64; return true;
        default: return false;
    }
}

static bool vm_integer_value_from_bits(uint16_t type, uint64_t bits, XrVmValue *value) {
    switch (type) {
        case XR_CORE_TYPE_I8:
            *value = (XrVmValue) {.kind = XR_VM_VALUE_I8,
                                  .as.i8 = (int8_t) xr_integer_signed_from_bits(bits)};
            return true;
        case XR_CORE_TYPE_U8:
            *value = (XrVmValue) {.kind = XR_VM_VALUE_U8, .as.u8 = (uint8_t) bits};
            return true;
        case XR_CORE_TYPE_I16:
            *value = (XrVmValue) {.kind = XR_VM_VALUE_I16,
                                  .as.i16 = (int16_t) xr_integer_signed_from_bits(bits)};
            return true;
        case XR_CORE_TYPE_U16:
            *value = (XrVmValue) {.kind = XR_VM_VALUE_U16, .as.u16 = (uint16_t) bits};
            return true;
        case XR_CORE_TYPE_I32:
            *value = (XrVmValue) {.kind = XR_VM_VALUE_I32,
                                  .as.i32 = (int32_t) xr_integer_signed_from_bits(bits)};
            return true;
        case XR_CORE_TYPE_U32:
            *value = (XrVmValue) {.kind = XR_VM_VALUE_U32, .as.u32 = (uint32_t) bits};
            return true;
        case XR_CORE_TYPE_I64:
            *value = (XrVmValue) {.kind = XR_VM_VALUE_I64,
                                  .as.i64 = xr_integer_signed_from_bits(bits)};
            return true;
        case XR_CORE_TYPE_U64:
            *value = (XrVmValue) {.kind = XR_VM_VALUE_U64, .as.u64 = bits};
            return true;
        default:
            return false;
    }
}

static void vm_dispatch_panic_value(XrVmDispatch *dispatch, XrVmPanicInfo info) {
    XrVmExecution *execution = dispatch->execution;
    XrVmInstructionView instruction = dispatch->instruction;
    XrVmValue panic = {.kind = XR_VM_VALUE_PANIC_INFO, .as.panic_info = info};
    if (instruction.successor_count == 0u) {
        dispatch->terminal = true;
        dispatch->outcome = vm_outcome(XR_VM_OUTCOME_PANIC, &execution->context);
        dispatch->outcome.panic_value = panic;
        return;
    }
    const XrValidatedFunction *function =
        &execution->context.code->program->functions[execution->function_id];
    const XrValidatedBlock *target = &function->blocks[instruction.successors[0]];
    uint32_t prefix = xr_core_spec_panic_value_prefix(instruction.operation_id, instruction.immediate.u32);
    execution->edge_values[0] = (XrVmRuntimeValue) {
        .category = XR_CORE_IR_VALUE, .as.value = panic,
    };
    for (uint32_t index = 1u; index < target->argument_count; ++index)
        execution->edge_values[index] =
            execution->values[instruction.operands[prefix + index - 1u]];
    dispatch->incoming_count = target->argument_count;
    dispatch->block_id = instruction.successors[0];
    dispatch->transferred = true;
    dispatch->terminal = false;
}

static void vm_dispatch_panic(XrVmDispatch *dispatch, uint32_t panic_code) {
    vm_dispatch_panic_value(dispatch, (XrVmPanicInfo){.code = panic_code});
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
        case XR_CORE_OP_CORE_INTEGER_DIVMOD: {
            const XrCoreIntegerType *integer = xr_core_spec_integer_type(instruction.result_type_id);
            uint64_t left = 0u, right = 0u;
            if (!integer || !vm_integer_value_bits(values[instruction.operands[0]].as.value, &left) ||
                !vm_integer_value_bits(values[instruction.operands[1]].as.value, &right))
                goto done;
            XrIntegerDivModResult division = xr_integer_divmod_eval(
                left, right, integer->width, integer->is_signed, instruction.immediate.u32 != 0u);
            if (division.divisor_is_zero) {
                vm_dispatch_panic(dispatch, instruction.immediate.u32 == 0u ? 420u : 421u);
                return;
            }
            if (!vm_integer_value_from_bits(instruction.result_type_id, division.bits,
                                             &produced.as.value))
                goto done;
            break;
        }
        case XR_CORE_OP_CORE_SCALAR_BITCAST64: {
            XrVmValue source = values[instruction.operands[0]].as.value;
            uint64_t bits = source.as.f64_bits;
            if (source.kind != XR_VM_VALUE_F64 && !vm_integer_value_bits(source, &bits))
                goto done;
            if (instruction.result_type_id == XR_CORE_TYPE_F64) {
                produced.as.value.kind = XR_VM_VALUE_F64;
                produced.as.value.as.f64_bits = bits;
            } else if (!vm_integer_value_from_bits(instruction.result_type_id, bits, &produced.as.value))
                goto done;
            break;
        }
        case XR_CORE_OP_CORE_INTEGER_CONVERT: {
            const XrValidatedFunction *function =
                &context->code->program->functions[execution->function_id];
            const XrCoreIntegerType *source =
                xr_core_spec_integer_type(function->value_types[instruction.operands[0]]);
            const XrCoreIntegerType *target = xr_core_spec_integer_type(instruction.result_type_id);
            uint64_t bits = 0u;
            if (!source || !target ||
                !vm_integer_value_bits(values[instruction.operands[0]].as.value, &bits))
                goto done;
            bits = xr_integer_convert_bits(bits, source->width, source->is_signed,
                                           target->width, target->is_signed);
            if (!vm_integer_value_from_bits(instruction.result_type_id, bits, &produced.as.value))
                goto done;
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
        case XR_CORE_OP_CORE_COMPARE_F64: {
            double left, right;
            memcpy(&left, &values[instruction.operands[0]].as.value.as.f64_bits, sizeof(left));
            memcpy(&right, &values[instruction.operands[1]].as.value.as.f64_bits, sizeof(right));
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
                XrVmPanicInfo panic = {
                    .code = instruction.immediate.u32 & ~XR_CORE_ASSERT_MESSAGE_PRESENT,
                };
                if ((instruction.immediate.u32 & XR_CORE_ASSERT_MESSAGE_PRESENT) != 0u) {
                    XrVmValue copy;
                    if (!clone_vm_value(context, values[instruction.operands[1]].as.value,
                                         XR_CORE_TYPE_STRING, &copy)) {
                        dispatch->outcome = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                        dispatch->returned = true;
                        return;
                    }
                    panic.message = copy.as.string;
                }
                vm_dispatch_panic_value(dispatch, panic);
                return;
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
            XrVmOutcome nested =
                execute_function(context, target_function, scratch, callee->parameter_count,
                                 depth + 1u, &dispatch->call_entered);
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

/* Calls transfer only the selected outcome. Ordinary and suspended child
 * calls share parallel argument assignment; the child frame may be released
 * after its payload has entered the execution tree's edge scratch. */
static bool vm_dispatch_call_completion(XrVmDispatch *dispatch,
                                         const XrValidatedFunction *callee,
                                         const XrVmOutcome *outcome, uint32_t operand_prefix) {
    XrVmExecution *execution = dispatch->execution;
    const XrVmInstructionView *instruction = &dispatch->instruction;
    const XrValidatedFunction *function =
        &execution->context.code->program->functions[execution->function_id];
    bool coroutine = instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED ||
                     instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT;
    uint32_t typed_start = coroutine ? 2u : 1u;
    uint32_t typed_count = typed_start + (callee->error_type_id != XR_CORE_TYPE_VOID ? 1u : 0u) +
                           (callee->panic_type_id != XR_CORE_TYPE_VOID ? 1u : 0u);
    uint32_t successor = 0u;
    const XrVmValue *payload = NULL;
    if (outcome->kind == XR_VM_OUTCOME_RETURN) {
        if (callee->result_type_id != XR_CORE_TYPE_VOID)
            payload = &outcome->value;
    } else if (outcome->kind == XR_VM_OUTCOME_ERROR &&
               callee->error_type_id != XR_CORE_TYPE_VOID) {
        successor = typed_start;
        payload = &outcome->error_value;
    } else if (outcome->kind == XR_VM_OUTCOME_PANIC &&
               callee->panic_type_id != XR_CORE_TYPE_VOID) {
        successor = typed_start + (callee->error_type_id != XR_CORE_TYPE_VOID ? 1u : 0u);
        payload = &outcome->panic_value;
    } else if (outcome->kind == XR_VM_OUTCOME_TRAP &&
               outcome->trap == XR_VM_TRAP_PROVIDER_CALL_FAILED &&
               instruction->successor_count == typed_count + 1u) {
        successor = typed_count;
    } else {
        return false;
    }
    if (successor >= instruction->successor_count)
        return false;
    uint32_t operand = operand_prefix;
    for (uint32_t prior = 0u; prior < successor; ++prior) {
        const XrValidatedBlock *target = &function->blocks[instruction->successors[prior]];
        uint32_t implicit = prior == 0u
                                ? (callee->result_type_id != XR_CORE_TYPE_VOID ? 1u : 0u)
                                : (prior >= typed_start ? 1u : 0u);
        operand += target->argument_count - implicit;
    }
    const XrValidatedBlock *target = &function->blocks[instruction->successors[successor]];
    uint32_t implicit = payload ? 1u : 0u;
    if (payload)
        execution->edge_values[0] =
            (XrVmRuntimeValue) {.category = XR_CORE_IR_VALUE, .as.value = *payload};
    for (uint32_t index = implicit; index < target->argument_count; ++index)
        execution->edge_values[index] =
            execution->values[instruction->operands[operand + index - implicit]];
    dispatch->incoming_count = target->argument_count;
    dispatch->block_id = instruction->successors[successor];
    dispatch->transferred = true;
    return true;
}

static void vm_dispatch_invoke(XrVmDispatch *dispatch) {
    XrVmExecution *execution = dispatch->execution;
    XrVmInstructionView instruction = dispatch->instruction;
    XrVmContext *context = &execution->context;
    uint32_t depth = execution->depth;
    XrVmRuntimeValue *values = execution->values;
    XrVmRuntimeValue *scratch = execution->edge_values;
    XrVmOutcome result = dispatch->outcome;
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
            XrVmOutcome nested =
                execute_function(context, target_function, scratch, callee->parameter_count,
                                 depth + 1u, &dispatch->call_entered);
            if (!vm_dispatch_call_completion(dispatch, callee, &nested, source_argument)) {
                result = nested;
                goto done;
            }
            break;
        }
        default:
            goto done;
    }
    dispatch->terminal = false;
done:
    dispatch->outcome = result;
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
    const XrValidatedBlock *trap_target =
        instruction.successor_count != 0u ? &function->blocks[instruction.successors[0]] : NULL;
    uint32_t data_count =
        instruction.operand_count - (trap_target ? trap_target->argument_count : 0u);
    XrVmOutcome outcome = vm_outcome(XR_VM_OUTCOME_RETURN, context);
    dispatch->terminal = true;
    switch (instruction.operation_id) {
        case XR_CORE_OP_CORE_PROVIDER_CALL:
            outcome = vm_provider_call(context, function, &instruction, values, data_count);
            if (outcome.kind == XR_VM_OUTCOME_RETURN)
                produced.as.value = outcome.value;
            break;
        case XR_CORE_OP_CORE_CONSTANT_STRING:
        case XR_CORE_OP_CORE_CONSTANT_RUNE:
        case XR_CORE_OP_CORE_STRING_FROM_SCALAR:
        case XR_CORE_OP_CORE_STRING_CONCAT:
        case XR_CORE_OP_CORE_SEQUENCE_LENGTH:
        case XR_CORE_OP_CORE_COMPARE_STRING:
        case XR_CORE_OP_CORE_COMPARE_RUNE:
        case XR_CORE_OP_CORE_OUTPUT_GROUP: {
            VmTextStatus text_status = vm_execute_text_operation(
                context, instruction.operation_id, instruction.operands, data_count,
                instruction.immediate.constant_id, instruction.immediate.u32,
                instruction.immediate.provider_operation.requirement_index,
                instruction.immediate.provider_operation.operation_index, values,
                &produced.as.value);
            if (text_status != VM_TEXT_OK)
                outcome = vm_text_outcome(context, text_status);
            break;
        }
        default:
            goto done;
    }
    if (outcome.kind != XR_VM_OUTCOME_RETURN) {
        if (outcome.kind != XR_VM_OUTCOME_TRAP || outcome.trap != XR_VM_TRAP_PROVIDER_CALL_FAILED ||
            !trap_target) {
            result = outcome;
            goto done;
        }
        for (uint32_t index = 0u; index < trap_target->argument_count; ++index)
            scratch[index] = values[instruction.operands[data_count + index]];
        incoming_count = trap_target->argument_count;
        block_id = instruction.successors[0];
        transferred = true;
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
        case XR_CORE_OP_CORE_OWNER_ALIAS: {
            XrVmValue source = values[instruction.operands[0]].as.value;
            const XrValidatedType *type = xr_validated_program_type(context->code->program,
                                                                   instruction.result_type_id);
            if (type && type->kind == XR_CORE_IR_TYPE_ARRAY) {
                XrVmAggregateValue *array = source.kind == XR_VM_VALUE_AGGREGATE
                    ? (XrVmAggregateValue *)(void *)source.as.aggregate : NULL;
                if (!array || array->owner_count == 0u || array->owner_count == UINT32_MAX) {
                    result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                    goto done;
                }
                ++array->owner_count;
                produced.as.value = source;
                break;
            }
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
            if (!type || !xr_program_type_kind_is_reference_record(type->kind) ||
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
            places[instruction.result_id].container = &instance->cell;
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
    XrVmPlace *target = NULL;
    if (instruction.operation_id != XR_CORE_OP_CORE_PLACE_LOCAL &&
        instruction.operation_id != XR_CORE_OP_CORE_PLACE_MODULE &&
        instruction.operation_id != XR_CORE_OP_CORE_SEQUENCE_ELEMENT_PLACE) {
        target = values[instruction.operands[0]].as.place;
        if (!target) {
            result = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
            goto done;
        }
        if (instruction.operation_id == XR_CORE_OP_CORE_PLACE_INITIALIZE) {
            if (target->initialized) {
                result = vm_trap(XR_VM_TRAP_MODULE_SLOT_ALREADY_INITIALIZED, context);
                goto done;
            }
        } else if (!target->initialized) {
            result = vm_trap(XR_VM_TRAP_MODULE_SLOT_UNINITIALIZED, context);
            goto done;
        }
    }
    switch (instruction.operation_id) {
        case XR_CORE_OP_CORE_SEQUENCE_ELEMENT_PLACE: {
            XrVmValue source = values[instruction.operands[0]].as.value;
            XrVmAggregateValue *array = (XrVmAggregateValue *)(void *)source.as.aggregate;
            int64_t index = values[instruction.operands[1]].as.value.as.i64;
            if (source.kind != XR_VM_VALUE_AGGREGATE || !array) {
                result = vm_outcome(XR_VM_OUTCOME_INVALID_INVOCATION, context);
                goto done;
            }
            if (index < 0 || (uint64_t)index >= array->field_count) {
                vm_dispatch_panic_value(dispatch, (XrVmPanicInfo){
                    .code = 430u, .has_bounds = true, .index = index,
                    .length = array->field_count});
                return;
            }
            places[instruction.result_id].alias = &array->fields[(uint32_t)index];
            places[instruction.result_id].initialized = true;
            places[instruction.result_id].container = &array->cell;
            produced.category = XR_CORE_IR_PLACE;
            produced.as.place = &places[instruction.result_id];
            break;
        }
        case XR_CORE_OP_CORE_PLACE_MODULE:
            produced.category = XR_CORE_IR_PLACE;
            produced.as.place =
                vm_module_slot(context, instruction.immediate.module_slot.module_index,
                               instruction.immediate.module_slot.slot_index);
            if (!produced.as.place)
                goto done;
            break;
        case XR_CORE_OP_CORE_PLACE_INITIALIZE: {
            XrVmValue replacement = values[instruction.operands[1]].as.value;
            if (!target->module_state || !vm_publish_value(context, replacement)) {
                result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                goto done;
            }
            target->value = replacement;
            target->initialized = true;
            XrVmModuleState *state = target->module_state;
            state->publication_order[state->publication_count++] = target->module_slot;
            break;
        }
        case XR_CORE_OP_CORE_PLACE_LOCAL:
            places[instruction.result_id].alias = &values[instruction.operands[0]].as.value;
            places[instruction.result_id].initialized = true;
            produced.category = XR_CORE_IR_PLACE;
            produced.as.place = &places[instruction.result_id];
            break;
        case XR_CORE_OP_CORE_PLACE_LOAD:
            if (xr_validated_program_type_ownership(context->code->program,
                                                    instruction.result_type_id) ==
                XR_CORE_IR_TYPE_OWNERSHIP_AFFINE) {
                produced.as.value = *vm_place_value(target);
            } else if (!clone_vm_value(context, *vm_place_value(target), instruction.result_type_id,
                                       &produced.as.value)) {
                result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                goto done;
            }
            break;
        case XR_CORE_OP_CORE_PLACE_STORE: {
            uint16_t type = context->code->program->functions[execution->function_id]
                                .value_types[instruction.operands[1]];
            XrVmValue replacement = void_value();
            if (!clone_vm_value(context, values[instruction.operands[1]].as.value, type,
                                &replacement) ||
                (vm_place_is_persistent(target) && !vm_publish_value(context, replacement))) {
                result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                goto done;
            }
            drop_vm_value(context, vm_place_value(target), XR_VM_EVENT_ORIGIN_PROGRAM_OPERATION);
            *vm_place_value(target) = replacement;
            break;
        }
        case XR_CORE_OP_CORE_PLACE_PROJECT: {
            XrVmValue *source = vm_place_value(target);
            XrVmAggregateValue *aggregate =
                (XrVmAggregateValue *) (void *) source->as.aggregate;
            places[instruction.result_id].alias =
                &aggregate->fields[instruction.immediate.field_ordinal];
            places[instruction.result_id].initialized = true;
            places[instruction.result_id].container = &aggregate->cell;
            produced.category = XR_CORE_IR_PLACE;
            produced.as.place = &places[instruction.result_id];
            break;
        }
        case XR_CORE_OP_CORE_PLACE_TAKE:
            produced.as.value = *vm_place_value(target);
            *vm_place_value(target) = void_value();
            target->initialized = false;
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
            if (vm_place_is_persistent(target) && !vm_publish_value(context, replacement)) {
                result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                goto done;
            }
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
        case XR_CORE_OP_CORE_ATOMIC_CONSTRUCT: {
            XrVmValue initial = values[instruction.operands[0]].as.value;
            int64_t bits = initial.kind == XR_VM_VALUE_BOOL ? (initial.as.boolean ? 1 : 0)
                                                            : initial.as.i64;
            if (initial.kind == XR_VM_VALUE_F64)
                memcpy(&bits, &initial.as.f64_bits, sizeof(bits));
            XrVmAtomicValue *value = allocate_atomic(context, instruction.result_type_id, NULL, bits);
            if (!value) {
                result = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
                goto done;
            }
            produced.as.value.kind = XR_VM_VALUE_ATOMIC;
            produced.as.value.as.atomic_storage = value;
            break;
        }
        case XR_CORE_OP_CORE_ATOMIC_LOAD:
        case XR_CORE_OP_CORE_ATOMIC_EXCHANGE:
        case XR_CORE_OP_CORE_ATOMIC_COMPARE_EXCHANGE:
        case XR_CORE_OP_CORE_ATOMIC_UPDATE: {
            const XrVmAtomicValue *value = values[instruction.operands[0]].as.value.as.atomic_storage;
            int64_t bits;
            if (instruction.operation_id == XR_CORE_OP_CORE_ATOMIC_UPDATE) {
                XrVmValue operand = values[instruction.operands[1]].as.value;
                int64_t delta = operand.kind == XR_VM_VALUE_BOOL ? (operand.as.boolean ? 1 : 0) : operand.as.i64;
                uint32_t mode = instruction.immediate.u32 >> 3u;
                uint32_t ordering = instruction.immediate.u32 & 7u;
                if (operand.kind == XR_VM_VALUE_F64) {
                    double number;
                    memcpy(&number, &operand.as.f64_bits, sizeof(number));
                    double previous = xr_atomic_f64_fetch_update_core(&value->shared->value, number, mode == 1u, ordering);
                    memcpy(&bits, &previous, sizeof(bits));
                } else bits = mode == 0u ? xr_atomic_i64_fetch_add_core(&value->shared->value, delta, ordering)
                     : mode == 1u ? xr_atomic_i64_fetch_sub_core(&value->shared->value, delta, ordering)
                                  : xr_atomic_i64_fetch_xor_core(&value->shared->value, delta, ordering);
            } else if (instruction.operation_id == XR_CORE_OP_CORE_ATOMIC_COMPARE_EXCHANGE) {
                XrVmValue expected = values[instruction.operands[1]].as.value;
                XrVmValue replacement = values[instruction.operands[2]].as.value;
                bits = expected.kind == XR_VM_VALUE_BOOL ? (expected.as.boolean ? 1 : 0) : expected.as.i64;
                int64_t desired = replacement.kind == XR_VM_VALUE_BOOL ? (replacement.as.boolean ? 1 : 0) : replacement.as.i64;
                if (expected.kind == XR_VM_VALUE_F64) {
                    memcpy(&bits, &expected.as.f64_bits, sizeof(bits));
                    memcpy(&desired, &replacement.as.f64_bits, sizeof(desired));
                }
                (void)xr_atomic_i64_compare_exchange_core(&value->shared->value, &bits, desired, instruction.immediate.u32);
            } else if (instruction.operation_id == XR_CORE_OP_CORE_ATOMIC_EXCHANGE) {
                XrVmValue replacement = values[instruction.operands[1]].as.value;
                int64_t desired = replacement.kind == XR_VM_VALUE_BOOL ? (replacement.as.boolean ? 1 : 0)
                                                                       : replacement.as.i64;
                if (replacement.kind == XR_VM_VALUE_F64)
                    memcpy(&desired, &replacement.as.f64_bits, sizeof(desired));
                bits = xr_atomic_i64_exchange_core(&value->shared->value, desired, instruction.immediate.u32);
            } else {
                bits = xr_atomic_i64_load_core(&value->shared->value, instruction.immediate.u32);
            }
            if (instruction.result_type_id == XR_CORE_TYPE_BOOL) {
                produced.as.value.kind = XR_VM_VALUE_BOOL;
                produced.as.value.as.boolean = bits != 0;
            } else if (instruction.result_type_id == XR_CORE_TYPE_F64) {
                produced.as.value.kind = XR_VM_VALUE_F64;
                memcpy(&produced.as.value.as.f64_bits, &bits, sizeof(bits));
            } else {
                produced.as.value.kind = XR_VM_VALUE_I64;
                produced.as.value.as.i64 = bits;
            }
            break;
        }
        case XR_CORE_OP_CORE_ARRAY_CONSTRUCT:
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

            uint32_t parameter_prefix = callee->parameter_count;
            if (indirect && !callee->has_receiver)
                ++parameter_prefix;
            bool completed =
                vm_dispatch_call_completion(dispatch, callee, &child, parameter_prefix);
            xr_vm_execution_free(execution->child);
            execution->child = NULL;
            if (!completed) {
                execution->finished = true;
                child.steps = execution->context.steps;
                child.state_id = execution->state_id;
                vm_dispatch_publish_outcome(dispatch, child);
                return;
            }
            break;
        }
        default:
            return;
    }
    dispatch->terminal = false;
}
