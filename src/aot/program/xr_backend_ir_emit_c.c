/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_backend_ir_emit_c.c - Mechanical portable-C11 spelling of XrBackendIR
 */

#include "xr_backend_ir_internal.h"

#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include "../../core/xr_core_spec_gen.h"
#include "../xi_cgen_verify_output.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define XR_GENERATED_C_MAX_SIZE (64u * 1024u * 1024u)

typedef struct CBuffer {
    char *bytes;
    size_t size;
    size_t capacity;
    bool failed;
} CBuffer;

static bool reserve(CBuffer *buffer, size_t extra) {
    if (buffer->failed || extra > XR_GENERATED_C_MAX_SIZE ||
        buffer->size > XR_GENERATED_C_MAX_SIZE - extra) {
        buffer->failed = true;
        return false;
    }
    size_t required = buffer->size + extra + 1u;
    if (required <= buffer->capacity)
        return true;
    size_t capacity = buffer->capacity ? buffer->capacity : 4096u;
    while (capacity < required) {
        if (capacity > XR_GENERATED_C_MAX_SIZE / 2u) {
            capacity = XR_GENERATED_C_MAX_SIZE + 1u;
            break;
        }
        capacity *= 2u;
    }
    if (capacity > XR_GENERATED_C_MAX_SIZE) {
        buffer->failed = true;
        return false;
    }
    char *grown = xr_realloc(buffer->bytes, capacity);
    if (!grown) {
        buffer->failed = true;
        return false;
    }
    buffer->bytes = grown;
    buffer->capacity = capacity;
    return true;
}

static bool append_text(CBuffer *buffer, const char *text) {
    size_t length = strlen(text);
    if (!reserve(buffer, length))
        return false;
    memcpy(buffer->bytes + buffer->size, text, length);
    buffer->size += length;
    buffer->bytes[buffer->size] = '\0';
    return true;
}

static bool append_format(CBuffer *buffer, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    va_list measure;
    va_copy(measure, arguments);
    int length = vsnprintf(NULL, 0u, format, measure);
    va_end(measure);
    if (length < 0 || !reserve(buffer, (size_t) length)) {
        va_end(arguments);
        return false;
    }
    (void) vsnprintf(buffer->bytes + buffer->size, (size_t) length + 1u, format, arguments);
    va_end(arguments);
    buffer->size += (size_t) length;
    return true;
}

static const char *type_c_name(uint16_t type_id, char storage[32]) {
    switch (type_id) {
        case XR_CORE_TYPE_BOOL:
            return "uint8_t";
        case XR_CORE_TYPE_I64:
            return "int64_t";
        case XR_CORE_TYPE_U32:
        case XR_CORE_TYPE_ERROR:
            return "uint32_t";
        case XR_CORE_TYPE_VOID:
            return "void";
        default:
            if (type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE)
                return NULL;
            (void) snprintf(storage, 32u, "XrAotType%u", type_id);
            return storage;
    }
    return NULL;
}

static uint32_t outcome_value_kind(uint16_t type_id) {
    switch (type_id) {
        case XR_CORE_TYPE_VOID:
            return 0u;
        case XR_CORE_TYPE_BOOL:
            return 1u;
        case XR_CORE_TYPE_I64:
            return 2u;
        case XR_CORE_TYPE_U32:
            return 3u;
        case XR_CORE_TYPE_ERROR:
            return 4u;
        default:
            return type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE ? 5u : UINT32_MAX;
    }
}

static bool emit_type_definition(CBuffer *buffer, const XrBackendIR *ir, uint32_t index,
                                 uint8_t *state) {
    if (state[index] == 2u)
        return true;
    if (state[index] == 1u)
        return false;
    state[index] = 1u;
    const XrValidatedType *type = &ir->program->types[index];
    for (uint32_t field = 0; field < type->field_count; ++field) {
        uint16_t child = type->field_types[field];
        if (child >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE &&
            !emit_type_definition(buffer, ir, child - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE, state))
            return false;
    }
    for (uint32_t variant = 0; variant < type->variant_count; ++variant) {
        for (uint32_t field = 0; field < type->variants[variant].payload_count; ++field) {
            uint16_t child = type->variants[variant].payload_types[field];
            if (child >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE &&
                !emit_type_definition(buffer, ir, child - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE, state))
                return false;
        }
    }
    if (!append_format(buffer, "struct XrAotType%u {\n", type->type_id))
        return false;
    if (type->kind == XR_CORE_IR_TYPE_AGGREGATE) {
        for (uint32_t field = 0; field < type->field_count; ++field) {
            char storage[32];
            const char *name = type_c_name(type->field_types[field], storage);
            if (!name || !append_format(buffer, "    %s f%u;\n", name, field))
                return false;
        }
    } else {
        if (!append_text(buffer, "    uint32_t tag;\n    union {\n"))
            return false;
        for (uint32_t variant = 0; variant < type->variant_count; ++variant) {
            const XrValidatedVariant *row = &type->variants[variant];
            if (!append_text(buffer, "        struct {\n"))
                return false;
            if (row->payload_count == 0u && !append_text(buffer, "            uint8_t empty;\n"))
                return false;
            for (uint32_t field = 0; field < row->payload_count; ++field) {
                char storage[32];
                const char *name = type_c_name(row->payload_types[field], storage);
                if (!name || !append_format(buffer, "            %s f%u;\n", name, field))
                    return false;
            }
            if (!append_format(buffer, "        } case_%u;\n", variant))
                return false;
        }
        if (!append_text(buffer, "    } payload;\n"))
            return false;
    }
    if (!append_text(buffer, "};\n\n"))
        return false;
    state[index] = 2u;
    return true;
}

static bool emit_type_definitions(CBuffer *buffer, const XrBackendIR *ir) {
    for (uint32_t index = 0; index < ir->program->type_count; ++index) {
        if (!append_format(buffer, "typedef struct XrAotType%u XrAotType%u;\n",
                           ir->program->types[index].type_id, ir->program->types[index].type_id))
            return false;
    }
    if (ir->program->type_count != 0u && !append_text(buffer, "\n"))
        return false;
    uint8_t *state =
        xr_calloc(ir->program->type_count ? ir->program->type_count : 1u, sizeof(uint8_t));
    if (!state)
        return false;
    bool emitted = true;
    for (uint32_t index = 0; emitted && index < ir->program->type_count; ++index)
        emitted = emit_type_definition(buffer, ir, index, state);
    xr_free(state);
    return emitted;
}

static const char *outcome_field(uint16_t type_id) {
    switch (type_id) {
        case XR_CORE_TYPE_BOOL:
            return "boolean";
        case XR_CORE_TYPE_I64:
            return "i64";
        case XR_CORE_TYPE_U32:
            return "u32";
        case XR_CORE_TYPE_ERROR:
            return "error";
        default:
            return NULL;
    }
}

static void scan_helpers(const XrBackendIR *ir, bool *checked, bool *wrapping) {
    *checked = false;
    *wrapping = false;
    for (uint32_t function = 0; function < ir->function_count; ++function) {
        const XrBackendFunction *fn = &ir->functions[function];
        for (uint32_t block = 0; block < fn->block_count; ++block) {
            const XrBackendBlock *row = &fn->blocks[block];
            for (uint32_t instruction = 0; instruction < row->instruction_count; ++instruction) {
                const XrBackendInstruction *op = &row->instructions[instruction];
                if (op->operation_id == XR_CORE_OP_CORE_ADD_I64 ||
                    op->operation_id == XR_CORE_OP_CORE_SUB_I64 ||
                    op->operation_id == XR_CORE_OP_CORE_MUL_I64) {
                    if (op->immediate.u32 == 0u)
                        *checked = true;
                    else
                        *wrapping = true;
                }
            }
        }
    }
}

static bool emit_prelude(CBuffer *buffer, const XrBackendIR *ir) {
    bool checked = false;
    bool wrapping = false;
    scan_helpers(ir, &checked, &wrapping);
    if (!append_text(buffer, "#include <stdint.h>\n"
                             "#include <limits.h>\n\n"
                             "typedef struct XrAotOutcome {\n"
                             "    uint32_t kind;\n"
                             "    uint32_t value_kind;\n"
                             "    uint32_t trap;\n"
                             "    uint32_t error;\n"
                             "    uint8_t boolean;\n"
                             "    int64_t i64;\n"
                             "    uint32_t u32;\n"
                             "} XrAotOutcome;\n\n"
                             "static XrAotOutcome xr_aot_make(uint32_t kind, uint32_t value_kind, "
                             "uint32_t trap, uint32_t error) {\n"
                             "    XrAotOutcome result = {0};\n"
                             "    result.kind = kind;\n"
                             "    result.value_kind = value_kind;\n"
                             "    result.trap = trap;\n"
                             "    result.error = error;\n"
                             "    return result;\n"
                             "}\n\n"))
        return false;
    if (wrapping &&
        !append_text(buffer, "static int64_t xr_aot_i64_from_bits(uint64_t bits) {\n"
                             "    if (bits <= (uint64_t)INT64_MAX) return (int64_t)bits;\n"
                             "    return -(int64_t)(~bits) - INT64_C(1);\n"
                             "}\n\n"))
        return false;
    if (checked &&
        !append_text(buffer, "int xr_aot_checked_add(int64_t left, int64_t right, int64_t *out) {\n"
                             "    if ((right > 0 && left > INT64_MAX - right) || "
                             "(right < 0 && left < INT64_MIN - right)) return 0;\n"
                             "    *out = left + right;\n"
                             "    return 1;\n"
                             "}\n"
                             "int xr_aot_checked_sub(int64_t left, int64_t right, int64_t *out) {\n"
                             "    if ((right < 0 && left > INT64_MAX + right) || "
                             "(right > 0 && left < INT64_MIN + right)) return 0;\n"
                             "    *out = left - right;\n"
                             "    return 1;\n"
                             "}\n"
                             "int xr_aot_checked_mul(int64_t left, int64_t right, int64_t *out) {\n"
                             "    if (left == 0 || right == 0) { *out = 0; return 1; }\n"
                             "    if ((left == -1 && right == INT64_MIN) || "
                             "(right == -1 && left == INT64_MIN)) return 0;\n"
                             "    if (left > 0) {\n"
                             "        if ((right > 0 && left > INT64_MAX / right) || "
                             "(right < 0 && right < INT64_MIN / left)) return 0;\n"
                             "    } else if ((right > 0 && left < INT64_MIN / right) || "
                             "(right < 0 && left < INT64_MAX / right)) return 0;\n"
                             "    *out = left * right;\n"
                             "    return 1;\n"
                             "}\n\n"))
        return false;
    return emit_type_definitions(buffer, ir);
}

static bool emit_function_signature(CBuffer *buffer, const XrBackendFunction *function,
                                    const XrBackendIR *ir, uint32_t function_id, bool prototype) {
    if (!append_format(buffer, "XrAotOutcome xr_aot_fn_%u(", function_id))
        return false;
    bool aggregate_result =
        xr_validated_program_type(ir->program, function->result_type_id) != NULL;
    if (function->parameter_count == 0u && !aggregate_result) {
        if (!append_text(buffer, "void"))
            return false;
    } else {
        for (uint32_t parameter = 0; parameter < function->parameter_count; ++parameter) {
            char storage[32];
            const char *name = type_c_name(function->parameter_types[parameter], storage);
            if (!name)
                return false;
            if (!append_format(buffer, "%s%s p%u", parameter ? ", " : "", name, parameter))
                return false;
        }
    }
    if (aggregate_result) {
        char storage[32];
        const char *name = type_c_name(function->result_type_id, storage);
        if (!name ||
            !append_format(buffer, "%s%s *out_result", function->parameter_count ? ", " : "", name))
            return false;
    }
    return append_text(buffer, prototype ? ");\n" : ") {\n");
}

static bool emit_i64_literal(CBuffer *buffer, int64_t value) {
    if (value == INT64_MIN)
        return append_text(buffer, "(-INT64_C(9223372036854775807) - INT64_C(1))");
    if (value < 0)
        return append_format(buffer, "(-INT64_C(%llu))", (unsigned long long) (-(value + 1)) + 1u);
    return append_format(buffer, "INT64_C(%llu)", (unsigned long long) value);
}

static bool emit_parallel_edge(CBuffer *buffer, const XrBackendFunction *function,
                               const XrBackendInstruction *instruction, uint32_t successor_index,
                               uint32_t operand_start, uint32_t function_id) {
    uint32_t target_id = instruction->successors[successor_index];
    const XrBackendBlock *target = &function->blocks[target_id];
    if (!append_text(buffer, "{\n"))
        return false;
    for (uint32_t argument = 0; argument < target->argument_count; ++argument) {
        uint32_t source_value = instruction->operands[operand_start + argument];
        char storage[32];
        const char *type = type_c_name(target->argument_types[argument], storage);
        if (!type ||
            !append_format(buffer, "        %s edge_%u = v%u;\n", type, argument, source_value))
            return false;
    }
    for (uint32_t argument = 0; argument < target->argument_count; ++argument) {
        if (!append_format(buffer, "        v%u = edge_%u;\n", target->argument_ids[argument],
                           argument))
            return false;
    }
    return append_format(buffer, "        goto xr_f%u_b%u;\n    }\n", function_id, target_id);
}

static bool emit_return(CBuffer *buffer, const XrBackendFunction *function,
                        const XrBackendInstruction *instruction) {
    uint32_t kind = outcome_value_kind(function->result_type_id);
    if (kind == UINT32_MAX)
        return false;
    if (!append_format(buffer, "        { XrAotOutcome result = xr_aot_make(0, %u, 0, 0);", kind))
        return false;
    if (instruction->operand_count != 0u) {
        if (function->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
            if (!append_format(buffer, " *out_result = v%u;", instruction->operands[0]))
                return false;
            return append_text(buffer, " return result; }\n");
        }
        const char *field = outcome_field(function->result_type_id);
        if (!field || !append_format(buffer, " result.%s = v%u;", field, instruction->operands[0]))
            return false;
    }
    return append_text(buffer, " return result; }\n");
}

static bool emit_call(CBuffer *buffer, const XrBackendIR *ir,
                      const XrBackendInstruction *instruction, uint32_t instruction_id) {
    uint32_t callee_id = instruction->immediate.function_id;
    const XrBackendFunction *callee = &ir->functions[callee_id];
    if (!append_format(buffer, "        XrAotOutcome call_%u = xr_aot_fn_%u(", instruction_id,
                       callee_id))
        return false;
    if (instruction->operand_count == 0u) {
        if (callee->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
            if (!append_format(buffer, "&v%u)", instruction->result_id))
                return false;
        } else if (!append_text(buffer, ")"))
            return false;
    } else {
        for (uint32_t operand = 0; operand < instruction->operand_count; ++operand) {
            if (!append_format(buffer, "%sv%u", operand ? ", " : "",
                               instruction->operands[operand]))
                return false;
        }
        if (callee->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
            if (!append_format(buffer, ", &v%u)", instruction->result_id))
                return false;
        } else if (!append_text(buffer, ")"))
            return false;
    }
    if (!append_text(buffer, ";\n") ||
        !append_format(buffer, "        if (call_%u.kind != 0) return call_%u;\n", instruction_id,
                       instruction_id))
        return false;
    if (instruction->result_id != XR_PROGRAM_LOCATION_NONE) {
        if (callee->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE)
            return true;
        const char *field = outcome_field(callee->result_type_id);
        if (!field || !append_format(buffer, "        v%u = call_%u.%s;\n", instruction->result_id,
                                     instruction_id, field))
            return false;
    }
    return true;
}

static bool emit_instruction(CBuffer *buffer, const XrBackendIR *ir,
                             const XrBackendFunction *function,
                             const XrBackendInstruction *instruction, uint32_t function_id,
                             uint32_t instruction_id) {
    switch (instruction->operation_id) {
        case XR_CORE_OP_CORE_CONSTANT_I64: {
            const XrValidatedConstant *constant =
                &ir->constants[instruction->immediate.constant_id];
            if (!append_format(buffer, "        v%u = ", instruction->result_id) ||
                !emit_i64_literal(buffer, constant->value.i64))
                return false;
            return append_text(buffer, ";\n");
        }
        case XR_CORE_OP_CORE_CONSTANT_BOOL: {
            const XrValidatedConstant *constant =
                &ir->constants[instruction->immediate.constant_id];
            return append_format(buffer, "        v%u = UINT8_C(%u);\n", instruction->result_id,
                                 constant->value.boolean ? 1u : 0u);
        }
        case XR_CORE_OP_CORE_ADD_I64:
        case XR_CORE_OP_CORE_SUB_I64:
        case XR_CORE_OP_CORE_MUL_I64: {
            const char *name = instruction->operation_id == XR_CORE_OP_CORE_ADD_I64   ? "add"
                               : instruction->operation_id == XR_CORE_OP_CORE_SUB_I64 ? "sub"
                                                                                      : "mul";
            const char *symbol = instruction->operation_id == XR_CORE_OP_CORE_ADD_I64   ? "+"
                                 : instruction->operation_id == XR_CORE_OP_CORE_SUB_I64 ? "-"
                                                                                        : "*";
            if (instruction->immediate.u32 == 0u)
                return append_format(buffer,
                                     "        if (!xr_aot_checked_%s(v%u, v%u, &v%u)) return "
                                     "xr_aot_make(1, 0, 1, 0);\n",
                                     name, instruction->operands[0], instruction->operands[1],
                                     instruction->result_id);
            return append_format(buffer,
                                 "        v%u = xr_aot_i64_from_bits((uint64_t)v%u %s "
                                 "(uint64_t)v%u);\n",
                                 instruction->result_id, instruction->operands[0], symbol,
                                 instruction->operands[1]);
        }
        case XR_CORE_OP_CORE_DIV_I64:
            return append_format(buffer,
                                 "        if (v%u == 0) return xr_aot_make(1, 0, 2, 0);\n"
                                 "        if (v%u == INT64_MIN && v%u == -1) return "
                                 "xr_aot_make(1, 0, 3, 0);\n"
                                 "        v%u = v%u / v%u;\n",
                                 instruction->operands[1], instruction->operands[0],
                                 instruction->operands[1], instruction->result_id,
                                 instruction->operands[0], instruction->operands[1]);
        case XR_CORE_OP_CORE_COMPARE_I64: {
            static const char *operators[] = {"==", "!=", "<", "<=", ">", ">="};
            return append_format(buffer, "        v%u = (uint8_t)(v%u %s v%u);\n",
                                 instruction->result_id, instruction->operands[0],
                                 operators[instruction->immediate.u32], instruction->operands[1]);
        }
        case XR_CORE_OP_CORE_BLOCK_ARGUMENT:
            return true;
        case XR_CORE_OP_CORE_BRANCH:
            return emit_parallel_edge(buffer, function, instruction, 0u, 0u, function_id);
        case XR_CORE_OP_CORE_CONDITIONAL_BRANCH: {
            uint32_t true_count = function->blocks[instruction->successors[0]].argument_count;
            if (!append_format(buffer, "        if (v%u) ", instruction->operands[0]) ||
                !emit_parallel_edge(buffer, function, instruction, 0u, 1u, function_id) ||
                !append_text(buffer, "        else "))
                return false;
            return emit_parallel_edge(buffer, function, instruction, 1u, 1u + true_count,
                                      function_id);
        }
        case XR_CORE_OP_CORE_RETURN:
            return emit_return(buffer, function, instruction);
        case XR_CORE_OP_CORE_CALL_SEALED_DIRECT:
            return emit_call(buffer, ir, instruction, instruction_id);
        case XR_CORE_OP_CORE_TRAP:
            return append_text(buffer, "        return xr_aot_make(1, 0, 4, 0);\n");
        case XR_CORE_OP_CORE_ERROR_PUBLISH:
            return append_format(buffer, "        return xr_aot_make(2, 0, 0, v%u);\n",
                                 instruction->operands[0]);
        case XR_CORE_OP_CORE_TARGET_POINTER_WIDTH:
            return append_format(buffer, "        v%u = UINT32_C(%u);\n", instruction->result_id,
                                 ir->pointer_width);
        case XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT: {
            char storage[32];
            const char *name = type_c_name(instruction->result_type_id, storage);
            if (!name ||
                !append_format(buffer, "        v%u = (%s){", instruction->result_id, name))
                return false;
            for (uint32_t field = 0; field < instruction->operand_count; ++field) {
                if (!append_format(buffer, "%s.f%u = v%u", field ? ", " : "", field,
                                   instruction->operands[field]))
                    return false;
            }
            return append_text(buffer, "};\n");
        }
        case XR_CORE_OP_CORE_AGGREGATE_PROJECT:
            return append_format(buffer, "        v%u = v%u.f%u;\n", instruction->result_id,
                                 instruction->operands[0], instruction->immediate.field_ordinal);
        case XR_CORE_OP_CORE_AGGREGATE_UPDATE:
            return append_format(buffer, "        v%u = v%u;\n        v%u.f%u = v%u;\n",
                                 instruction->result_id, instruction->operands[0],
                                 instruction->result_id, instruction->immediate.field_ordinal,
                                 instruction->operands[1]);
        case XR_CORE_OP_CORE_VARIANT_CONSTRUCT: {
            uint32_t variant = instruction->immediate.variant_ordinal;
            if (!append_format(buffer, "        v%u.tag = UINT32_C(%u);\n", instruction->result_id,
                               variant))
                return false;
            for (uint32_t field = 0; field < instruction->operand_count; ++field) {
                if (!append_format(buffer, "        v%u.payload.case_%u.f%u = v%u;\n",
                                   instruction->result_id, variant, field,
                                   instruction->operands[field]))
                    return false;
            }
            return true;
        }
        case XR_CORE_OP_CORE_VARIANT_TEST:
            return append_format(buffer, "        v%u = (uint8_t)(v%u.tag == UINT32_C(%u));\n",
                                 instruction->result_id, instruction->operands[0],
                                 instruction->immediate.variant_ordinal);
        case XR_CORE_OP_CORE_VARIANT_PROJECT:
            return append_format(
                buffer,
                "        if (v%u.tag != UINT32_C(%u)) return xr_aot_make(1, 0, 6, 0);\n"
                "        v%u = v%u.payload.case_%u.f%u;\n",
                instruction->operands[0], instruction->immediate.variant_field.variant_ordinal,
                instruction->result_id, instruction->operands[0],
                instruction->immediate.variant_field.variant_ordinal,
                instruction->immediate.variant_field.field_ordinal);
        default:
            return false;
    }
}

static bool emit_function(CBuffer *buffer, const XrBackendIR *ir, uint32_t function_id) {
    const XrBackendFunction *function = &ir->functions[function_id];
    if (!emit_function_signature(buffer, function, ir, function_id, false))
        return false;
    for (uint32_t parameter = 0; parameter < function->parameter_count; ++parameter) {
        if (!append_format(buffer, "    (void)p%u;\n", parameter))
            return false;
    }
    for (uint32_t value = 0; value < function->value_count; ++value) {
        char storage[32];
        const char *type = type_c_name(function->value_types[value], storage);
        const char *initializer =
            function->value_types[value] >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE ? "{0}" : "0";
        if (!type || !append_format(buffer, "    %s v%u = %s;\n", type, value, initializer) ||
            !append_format(buffer, "    (void)v%u;\n", value))
            return false;
    }
    const XrBackendBlock *entry = &function->blocks[function->entry_block];
    if (entry->argument_count != function->parameter_count)
        return false;
    for (uint32_t parameter = 0; parameter < function->parameter_count; ++parameter) {
        if (!append_format(buffer, "    v%u = p%u;\n", entry->argument_ids[parameter], parameter))
            return false;
    }
    if (!append_format(buffer, "    goto xr_f%u_b%u;\n", function_id, function->entry_block))
        return false;
    for (uint32_t block_id = 0; block_id < function->block_count; ++block_id) {
        const XrBackendBlock *block = &function->blocks[block_id];
        if (!append_format(buffer, "xr_f%u_b%u:\n    ;\n", function_id, block_id))
            return false;
        for (uint32_t instruction = 0; instruction < block->instruction_count; ++instruction) {
            if (!emit_instruction(buffer, ir, function, &block->instructions[instruction],
                                  function_id, instruction))
                return false;
        }
    }
    return append_text(buffer, "    return xr_aot_make(3, 0, 0, 0);\n}\n\n");
}

static bool emit_main(CBuffer *buffer, const XrBackendIR *ir) {
    const XrBackendFunction *entry = &ir->functions[ir->entry_function];
    if (entry->parameter_count != 0u)
        return false;
    if (!append_format(buffer,
                       "int main(void) {\n"
                       "    XrAotOutcome result = xr_aot_fn_%u();\n"
                       "    if (result.kind != 0) return "
                       "(int)(200u + result.kind * 10u + result.trap);\n",
                       ir->entry_function))
        return false;
    switch (entry->result_type_id) {
        case XR_CORE_TYPE_VOID:
            return append_text(buffer, "    return 0;\n}\n");
        case XR_CORE_TYPE_BOOL:
            return append_text(buffer, "    return result.boolean ? 1 : 0;\n}\n");
        case XR_CORE_TYPE_I64:
            return append_text(buffer,
                               "    return (int)((uint64_t)result.i64 & UINT64_C(255));\n}\n");
        case XR_CORE_TYPE_U32:
            return append_text(buffer, "    return (int)(result.u32 & UINT32_C(255));\n}\n");
        case XR_CORE_TYPE_ERROR:
            return append_text(buffer, "    return (int)(result.error & UINT32_C(255));\n}\n");
        default:
            return false;
    }
}

static bool uses_unrealized_place_contract(const XrBackendIR *ir) {
    for (uint32_t function = 0; function < ir->function_count; ++function) {
        const XrBackendFunction *row = &ir->functions[function];
        for (uint32_t parameter = 0; parameter < row->parameter_count; ++parameter) {
            if (row->parameter_modes[parameter] == XR_PARAM_REF)
                return true;
        }
        for (uint32_t value = 0; value < row->value_count; ++value) {
            if (row->value_categories[value] == XR_CORE_IR_PLACE)
                return true;
        }
    }
    return false;
}

XrBackendStatus xr_backend_ir_emit_c(const XrBackendIR *ir, bool standalone_main,
                                     XrGeneratedC *generated_out,
                                     XrBackendDiagnostic *diagnostic_out) {
    if (generated_out)
        memset(generated_out, 0, sizeof(*generated_out));
    xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_OK, 0u, 0u, 0u, 0u);
    if (!ir || !ir->verified || !generated_out || !xr_backend_ir_verify(ir, diagnostic_out) ||
        !xr_backend_ir_translation_validate(ir, diagnostic_out)) {
        if (!diagnostic_out || diagnostic_out->status == XR_BACKEND_OK)
            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVALID_INPUT, 0u, 0u, 0u, 0u);
        return diagnostic_out ? diagnostic_out->status : XR_BACKEND_INVALID_INPUT;
    }
    if (uses_unrealized_place_contract(ir)) {
        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_EMISSION_REJECTED, 0u, 0u, 0u, 0u);
        return XR_BACKEND_EMISSION_REJECTED;
    }
    CBuffer buffer = {0};
    bool emitted = emit_prelude(&buffer, ir);
    for (uint32_t function = 0; emitted && function < ir->function_count; ++function)
        emitted = emit_function_signature(&buffer, &ir->functions[function], ir, function, true);
    emitted = emitted && append_text(&buffer, "\n");
    for (uint32_t function = 0; emitted && function < ir->function_count; ++function)
        emitted = emit_function(&buffer, ir, function);
    if (emitted && standalone_main)
        emitted = emit_main(&buffer, ir);
    XiCgenVerifyResult verify = {0};
    if (!emitted || buffer.failed || !xi_cgen_verify_output(buffer.bytes, buffer.size, &verify)) {
        xr_free(buffer.bytes);
        xr_backend_set_diagnostic(
            diagnostic_out, buffer.failed ? XR_BACKEND_OUT_OF_MEMORY : XR_BACKEND_EMISSION_REJECTED,
            0u, 0u, 0u, 0u);
        return buffer.failed ? XR_BACKEND_OUT_OF_MEMORY : XR_BACKEND_EMISSION_REJECTED;
    }
    generated_out->bytes = buffer.bytes;
    generated_out->size = buffer.size;
    generated_out->execution_id = ir->execution_id;
    generated_out->backend_id = ir->backend_id;
    generated_out->optimization_policy_id = ir->optimization_policy_id;
    generated_out->target_profile_id = xr_target_profile_fingerprint(ir->profile);
    xr_semantic_fingerprint((const uint8_t *) buffer.bytes, buffer.size,
                            &generated_out->source_digest);
    return XR_BACKEND_OK;
}

void xr_generated_c_free(XrGeneratedC *generated) {
    if (!generated)
        return;
    xr_free(generated->bytes);
    memset(generated, 0, sizeof(*generated));
}
