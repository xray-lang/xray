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
        case XR_CORE_TYPE_PANIC_INFO:
            return "uint32_t";
        case XR_CORE_TYPE_U16:
            return "uint16_t";
        case XR_CORE_TYPE_VOID:
            return "void";
        default:
            if (type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE)
                return NULL;
            (void) snprintf(storage, 32u, "XrAotType%u", type_id);
            return storage;
    }
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
        case XR_CORE_TYPE_U16:
            return 6u;
        default:
            return type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE ? 5u : UINT32_MAX;
    }
}

static bool type_is_generator_handle(const XrBackendIR *ir, uint16_t type_id) {
    for (uint32_t function = 0u; ir && function < ir->function_count; ++function)
        if ((ir->functions[function].flags & XR_PROGRAM_FUNCTION_GENERATOR) != 0u &&
            ir->functions[function].result_type_id == type_id)
            return true;
    return false;
}

static uint32_t generator_target_for_handle(const XrBackendIR *ir, uint16_t type_id) {
    uint32_t match = UINT32_MAX;
    for (uint32_t function = 0u; ir && function < ir->function_count; ++function) {
        if ((ir->functions[function].flags & XR_PROGRAM_FUNCTION_GENERATOR) == 0u ||
            ir->functions[function].result_type_id != type_id)
            continue;
        if (match != UINT32_MAX)
            return UINT32_MAX;
        match = function;
    }
    return match;
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
        if (type_is_generator_handle(ir, type->type_id) &&
            !append_text(buffer, "    void *private_frame;\n    uint32_t function_id;\n"))
            return false;
        if (!type_is_generator_handle(ir, type->type_id) && type->field_count == 0u &&
            !append_text(buffer, "    uint8_t xr_unit;\n"))
            return false;
        for (uint32_t field = 0; field < type->field_count; ++field) {
            char storage[32];
            const char *name = type_c_name(type->field_types[field], storage);
            if (!name || !append_format(buffer, "    %s f%u;\n", name, field))
                return false;
        }
    } else if (type->kind == XR_CORE_IR_TYPE_VARIANT) {
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
    } else if (type->kind == XR_CORE_IR_TYPE_VIEW) {
        if (!append_text(buffer, "    void *data;\n"))
            return false;
    } else if (type->kind == XR_CORE_IR_TYPE_CALLABLE) {
        if (!append_text(buffer, "    uint32_t function_id;\n    void *capture;\n"))
            return false;
    } else if (type->kind == XR_CORE_IR_TYPE_EXISTENTIAL) {
        if (!append_text(
                buffer,
                "    uint16_t concrete_type_id;\n    uint32_t conformance_id;\n    void *data;\n"))
            return false;
    } else {
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
        case XR_CORE_TYPE_U16:
            return "u16";
        case XR_CORE_TYPE_ERROR:
            return "error";
        default:
            return NULL;
    }
}

static bool conformance_id(const XrValidatedProgram *program, uint16_t concrete_type_id,
                           uint32_t interface_id, uint32_t *id_out) {
    if (!program || !id_out)
        return false;
    for (uint32_t index = 0; index < program->conformance_count; ++index) {
        const XrValidatedConformance *row = &program->conformances[index];
        if (row->implementor_type_id == concrete_type_id && row->interface_id == interface_id) {
            *id_out = index;
            return true;
        }
    }
    return false;
}

static void scan_helpers(const XrBackendIR *ir, bool *checked, bool *wrapping, bool *arena) {
    *checked = false;
    *wrapping = false;
    *arena = false;
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
                if (op->operation_id == XR_CORE_OP_CORE_EXISTENTIAL_PACK ||
                    op->operation_id == XR_CORE_OP_CORE_GENERATOR_CREATE ||
                    (op->operation_id == XR_CORE_OP_CORE_CALLABLE_PACK && op->operand_count != 0u))
                    *arena = true;
            }
        }
    }
}

static bool emit_prelude(CBuffer *buffer, const XrBackendIR *ir) {
    bool checked = false;
    bool wrapping = false;
    bool arena = false;
    scan_helpers(ir, &checked, &wrapping, &arena);
    if (!append_text(buffer, "#include <stdint.h>\n"
                             "#include <limits.h>\n"
                             "#include <stddef.h>\n") ||
        (arena && !append_text(buffer, "#include <stdlib.h>\n")) || !append_text(buffer, "\n"))
        return false;
    if (arena) {
        if (!append_text(buffer,
                         "typedef union XrAotAllocation XrAotAllocation;\n"
                         "union XrAotAllocation {\n"
                         "    struct { XrAotAllocation *next; } link;\n"
                         "    max_align_t alignment;\n"
                         "};\n"
                         "typedef struct XrAotContext {\n"
                         "    XrAotAllocation *allocations;\n"
                         "} XrAotContext;\n\n"
                         "static inline void *xr_aot_alloc(XrAotContext *context, size_t size) "
                         "{\n"
                         "    if (!context || size > SIZE_MAX - sizeof(XrAotAllocation)) "
                         "return NULL;\n"
                         "    XrAotAllocation *allocation = "
                         "(XrAotAllocation *)malloc(sizeof(XrAotAllocation) + size);\n"
                         "    if (!allocation) return NULL;\n"
                         "    allocation->link.next = context->allocations;\n"
                         "    context->allocations = allocation;\n"
                         "    return (void *)(allocation + 1);\n"
                         "}\n"
                         "static inline void xr_aot_context_destroy(XrAotContext *context) "
                         "{\n"
                         "    XrAotAllocation *allocation = context->allocations;\n"
                         "    while (allocation) {\n"
                         "        XrAotAllocation *next = allocation->link.next;\n"
                         "        free(allocation);\n"
                         "        allocation = next;\n"
                         "    }\n"
                         "    context->allocations = NULL;\n"
                         "}\n\n"))
            return false;
    } else if (!append_text(buffer, "typedef struct XrAotContext {\n"
                                    "    uint8_t unused;\n"
                                    "} XrAotContext;\n\n")) {
        return false;
    }
    if (!append_text(buffer, "typedef struct XrAotOutcome {\n"
                             "    uint32_t kind;\n"
                             "    uint32_t value_kind;\n"
                             "    uint32_t trap;\n"
                             "    uint32_t error;\n"
                             "    uint8_t boolean;\n"
                             "    int64_t i64;\n"
                             "    uint32_t u32;\n"
                             "    uint16_t u16;\n"
                             "} XrAotOutcome;\n\n"
                             "static XrAotOutcome xr_aot_make(uint32_t kind, uint32_t value_kind, "
                             "uint32_t trap) {\n"
                             "    XrAotOutcome result = {0};\n"
                             "    result.kind = kind;\n"
                             "    result.value_kind = value_kind;\n"
                             "    result.trap = trap;\n"
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

static bool emit_coroutine_frames(CBuffer *buffer, const XrBackendIR *ir) {
    for (uint32_t function_id = 0; function_id < ir->function_count; ++function_id) {
        const XrBackendFunction *function = &ir->functions[function_id];
        if (function->coroutine_safepoint_count == 0u)
            continue;
        if (!append_format(buffer, "typedef struct XrAotCoroutineFrame%u {\n    uint32_t state;\n",
                           function_id))
            return false;
        for (uint32_t safepoint = 0; safepoint < function->coroutine_safepoint_count; ++safepoint) {
            const XrBackendCoroutineSafepoint *point = &function->coroutine_safepoints[safepoint];
            uint32_t resume_block =
                function->coroutine_states[point->resume_state_id].continuation_block;
            const XrBackendBlock *resume = &function->blocks[resume_block];
            if (resume->argument_count != point->live_value_count)
                return false;
            for (uint32_t live = 0; live < point->live_value_count; ++live) {
                char storage[32];
                const char *type = type_c_name(resume->argument_types[live], storage);
                if (!type || resume->argument_categories[live] != XR_CORE_IR_VALUE ||
                    !append_format(buffer, "    %s live_%u_%u;\n", type, safepoint, live))
                    return false;
            }
        }
        if (!append_format(buffer, "} XrAotCoroutineFrame%u;\n\n", function_id))
            return false;
    }
    return true;
}

static bool emit_function_signature(CBuffer *buffer, const XrBackendFunction *function,
                                    const XrBackendIR *ir, uint32_t function_id, bool prototype) {
    const char *suffix = function->coroutine_safepoint_count != 0u ? "_step" : "";
    if (!append_format(buffer, "XrAotOutcome xr_aot_fn_%u%s(XrAotContext *xr_ctx", function_id,
                       suffix))
        return false;
    if (function->coroutine_safepoint_count != 0u &&
        !append_format(buffer, ", XrAotCoroutineFrame%u *frame", function_id))
        return false;
    bool aggregate_result =
        xr_validated_program_type(ir->program, function->result_type_id) != NULL &&
        (function->flags & XR_PROGRAM_FUNCTION_GENERATOR) == 0u;
    bool has_error = function->error_type_id != XR_CORE_TYPE_VOID;
    bool has_panic = function->panic_type_id != XR_CORE_TYPE_VOID;
    for (uint32_t parameter = 0; parameter < function->parameter_count; ++parameter) {
        char storage[32];
        const char *name = type_c_name(function->parameter_types[parameter], storage);
        if (!name)
            return false;
        const char *pointer = function->parameter_modes[parameter] == XR_PARAM_REF ? " *" : "";
        if (!append_format(buffer, ", %s%s p%u", name, pointer, parameter))
            return false;
    }
    if (aggregate_result) {
        char storage[32];
        const char *name = type_c_name(function->result_type_id, storage);
        if (!name || !append_format(buffer, ", %s *out_result", name))
            return false;
    }
    if (has_error) {
        char storage[32];
        const char *name = type_c_name(function->error_type_id, storage);
        if (!name || !append_format(buffer, ", %s *out_error", name))
            return false;
    }
    if (has_panic) {
        char storage[32];
        const char *name = type_c_name(function->panic_type_id, storage);
        if (!name || !append_format(buffer, ", %s *out_panic", name))
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
        const char *pointer = target->argument_categories[argument] == XR_CORE_IR_PLACE ? " *" : "";
        if (!type || !append_format(buffer, "        %s%s edge_%u = v%u;\n", type, pointer,
                                    argument, source_value))
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
    if (!append_format(buffer, "        { XrAotOutcome result = xr_aot_make(0, %u, 0);", kind))
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
    if (!append_format(buffer, "        XrAotOutcome call_%u = xr_aot_fn_%u(xr_ctx", instruction_id,
                       callee_id))
        return false;
    for (uint32_t operand = 0; operand < instruction->operand_count; ++operand) {
        if (!append_format(buffer, ", v%u", instruction->operands[operand]))
            return false;
    }
    if (callee->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE &&
        !append_format(buffer, ", &v%u", instruction->result_id))
        return false;
    if (!append_text(buffer, ")"))
        return false;
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

static const XrValidatedSignature *witness_signature(const XrBackendIR *ir,
                                                     const XrBackendFunction *function,
                                                     const XrBackendInstruction *instruction,
                                                     uint32_t *interface_id_out) {
    if (!ir || !function || !instruction || !interface_id_out || instruction->operand_count == 0u)
        return NULL;
    uint32_t receiver_value = instruction->operands[0];
    if (receiver_value >= function->value_count)
        return NULL;
    const XrValidatedType *receiver =
        xr_validated_program_type(ir->program, function->value_types[receiver_value]);
    if (!receiver || receiver->kind != XR_CORE_IR_TYPE_EXISTENTIAL ||
        receiver->interface_id >= ir->program->interface_count)
        return NULL;
    const XrValidatedInterface *interface_row = &ir->program->interfaces[receiver->interface_id];
    uint32_t slot = instruction->immediate.u32;
    if (slot >= interface_row->slot_count ||
        interface_row->slot_signature_ids[slot] >= ir->program->signature_count)
        return NULL;
    *interface_id_out = receiver->interface_id;
    return &ir->program->signatures[interface_row->slot_signature_ids[slot]];
}

static bool emit_witness_call_cases(CBuffer *buffer, const XrBackendIR *ir,
                                    const XrBackendInstruction *instruction,
                                    const XrValidatedSignature *signature, uint32_t interface_id,
                                    uint32_t instruction_id, const char *result_expression,
                                    const char *error_expression, const char *panic_expression) {
    uint32_t receiver_value = instruction->operands[0];
    uint32_t slot = instruction->immediate.u32;
    for (uint32_t conformance_id = 0; conformance_id < ir->program->conformance_count;
         ++conformance_id) {
        const XrValidatedConformance *conformance = &ir->program->conformances[conformance_id];
        if (conformance->interface_id != interface_id)
            continue;
        if (slot >= conformance->slot_count ||
            conformance->slot_function_ids[slot] >= ir->function_count)
            return false;
        uint32_t callee_id = conformance->slot_function_ids[slot];
        const XrBackendFunction *callee = &ir->functions[callee_id];
        if (callee->parameter_count != signature->parameter_count || callee->parameter_count == 0u)
            return false;
        char storage[32];
        const char *receiver_type = type_c_name(conformance->implementor_type_id, storage);
        if (!receiver_type ||
            !append_format(buffer,
                           "            case UINT32_C(%u):\n"
                           "                if (v%u.concrete_type_id != UINT16_C(%u)) break;\n"
                           "                call_%u = xr_aot_fn_%u(xr_ctx, ",
                           conformance_id, receiver_value, conformance->implementor_type_id,
                           instruction_id, callee_id))
            return false;
        if (callee->parameter_modes[0] == XR_PARAM_REF) {
            if (!append_format(buffer, "(%s *)v%u.data", receiver_type, receiver_value))
                return false;
        } else if (!append_format(buffer, "*(const %s *)v%u.data", receiver_type, receiver_value)) {
            return false;
        }
        for (uint32_t parameter = 1u; parameter < callee->parameter_count; ++parameter)
            if (!append_format(buffer, ", v%u", instruction->operands[parameter]))
                return false;
        if (result_expression && !append_format(buffer, ", %s", result_expression))
            return false;
        if (error_expression && !append_format(buffer, ", %s", error_expression))
            return false;
        if (panic_expression && !append_format(buffer, ", %s", panic_expression))
            return false;
        if (!append_text(buffer, ");\n                break;\n"))
            return false;
    }
    return append_text(buffer, "            default:\n                break;\n        }\n");
}

static bool emit_witness_call(CBuffer *buffer, const XrBackendIR *ir,
                              const XrBackendFunction *function,
                              const XrBackendInstruction *instruction, uint32_t instruction_id) {
    uint32_t interface_id = 0u;
    const XrValidatedSignature *signature =
        witness_signature(ir, function, instruction, &interface_id);
    if (!signature || !append_format(buffer,
                                     "        XrAotOutcome call_%u = xr_aot_make(4, 0, 0);\n"
                                     "        switch (v%u.conformance_id) {\n",
                                     instruction_id, instruction->operands[0]))
        return false;
    char result_expression[64];
    const char *result = NULL;
    if (signature->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
        (void) snprintf(result_expression, sizeof(result_expression), "&v%u",
                        instruction->result_id);
        result = result_expression;
    }
    if (!emit_witness_call_cases(buffer, ir, instruction, signature, interface_id, instruction_id,
                                 result, NULL, NULL) ||
        !append_format(buffer, "        if (call_%u.kind != 0) return call_%u;\n", instruction_id,
                       instruction_id))
        return false;
    if (instruction->result_id == XR_PROGRAM_LOCATION_NONE ||
        signature->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE)
        return true;
    const char *field = outcome_field(signature->result_type_id);
    return field && append_format(buffer, "        v%u = call_%u.%s;\n", instruction->result_id,
                                  instruction_id, field);
}

static const XrValidatedSignature *callable_signature(const XrBackendIR *ir,
                                                      const XrBackendFunction *function,
                                                      const XrBackendInstruction *instruction) {
    if (!ir || !function || !instruction || instruction->operand_count == 0u ||
        instruction->operands[0] >= function->value_count)
        return NULL;
    const XrValidatedType *callable =
        xr_validated_program_type(ir->program, function->value_types[instruction->operands[0]]);
    if (!callable || callable->kind != XR_CORE_IR_TYPE_CALLABLE ||
        callable->signature_id >= ir->program->signature_count)
        return NULL;
    return &ir->program->signatures[callable->signature_id];
}

static bool callable_type_can_target(const XrBackendIR *ir, uint16_t callable_type,
                                     uint32_t target_function) {
    for (uint32_t function = 0; function < ir->function_count; ++function) {
        const XrBackendFunction *row = &ir->functions[function];
        for (uint32_t block = 0; block < row->block_count; ++block) {
            for (uint32_t instruction = 0; instruction < row->blocks[block].instruction_count;
                 ++instruction) {
                const XrBackendInstruction *candidate =
                    &row->blocks[block].instructions[instruction];
                if (candidate->operation_id == XR_CORE_OP_CORE_CALLABLE_PACK &&
                    candidate->result_type_id == callable_type &&
                    candidate->immediate.function_id == target_function)
                    return true;
            }
        }
    }
    return false;
}

static bool emit_callable_call_cases(CBuffer *buffer, const XrBackendIR *ir,
                                     const XrBackendFunction *caller,
                                     const XrBackendInstruction *instruction,
                                     const XrValidatedSignature *signature, uint32_t instruction_id,
                                     const char *result_expression, const char *error_expression,
                                     const char *panic_expression) {
    uint32_t callable_value = instruction->operands[0];
    uint16_t callable_type = caller->value_types[callable_value];
    for (uint32_t target_id = 0; target_id < ir->function_count; ++target_id) {
        if (!callable_type_can_target(ir, callable_type, target_id))
            continue;
        const XrBackendFunction *target = &ir->functions[target_id];
        const XrValidatedFunction *semantic_target = &ir->program->functions[target_id];
        uint32_t receiver_count = semantic_target->has_receiver ? 1u : 0u;
        if (target->parameter_count != signature->parameter_count + receiver_count ||
            !append_format(buffer,
                           "            case UINT32_C(%u):\n"
                           "                call_%u = xr_aot_fn_%u(xr_ctx",
                           target_id, instruction_id, target_id))
            return false;
        if (receiver_count != 0u) {
            char storage[32];
            const char *capture_type = type_c_name(target->parameter_types[0], storage);
            if (!capture_type ||
                !append_format(buffer, ", *(const %s *)v%u.capture", capture_type, callable_value))
                return false;
        }
        for (uint32_t parameter = 0u; parameter < signature->parameter_count; ++parameter)
            if (!append_format(buffer, ", v%u", instruction->operands[parameter + 1u]))
                return false;
        if (result_expression && !append_format(buffer, ", %s", result_expression))
            return false;
        if (error_expression && !append_format(buffer, ", %s", error_expression))
            return false;
        if (panic_expression && !append_format(buffer, ", %s", panic_expression))
            return false;
        if (!append_text(buffer, ");\n                break;\n"))
            return false;
    }
    return append_text(buffer, "            default:\n                break;\n        }\n");
}

static bool emit_callable_call(CBuffer *buffer, const XrBackendIR *ir,
                               const XrBackendFunction *function,
                               const XrBackendInstruction *instruction, uint32_t instruction_id) {
    const XrValidatedSignature *signature = callable_signature(ir, function, instruction);
    if (!signature || !append_format(buffer,
                                     "        XrAotOutcome call_%u = xr_aot_make(4, 0, 0);\n"
                                     "        switch (v%u.function_id) {\n",
                                     instruction_id, instruction->operands[0]))
        return false;
    char result_expression[64];
    const char *result = NULL;
    if (signature->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
        (void) snprintf(result_expression, sizeof(result_expression), "&v%u",
                        instruction->result_id);
        result = result_expression;
    }
    if (!emit_callable_call_cases(buffer, ir, function, instruction, signature, instruction_id,
                                  result, NULL, NULL) ||
        !append_format(buffer, "        if (call_%u.kind != 0) return call_%u;\n", instruction_id,
                       instruction_id))
        return false;
    if (instruction->result_id == XR_PROGRAM_LOCATION_NONE ||
        signature->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE)
        return true;
    const char *field = outcome_field(signature->result_type_id);
    return field && append_format(buffer, "        v%u = call_%u.%s;\n", instruction->result_id,
                                  instruction_id, field);
}

static bool emit_invoke_edge(CBuffer *buffer, const XrBackendFunction *function,
                             const XrBackendInstruction *instruction, uint32_t successor_index,
                             uint32_t target_start, uint32_t operand_start, uint32_t function_id,
                             const char *implicit_expression) {
    const XrBackendBlock *target = &function->blocks[instruction->successors[successor_index]];
    if (!append_text(buffer, "{\n"))
        return false;
    if (target_start != 0u &&
        (!implicit_expression || !append_format(buffer, "            v%u = %s;\n",
                                                target->argument_ids[0], implicit_expression)))
        return false;
    for (uint32_t argument = target_start; argument < target->argument_count; ++argument) {
        uint32_t source_value = instruction->operands[operand_start + argument - target_start];
        char storage[32];
        const char *type = type_c_name(target->argument_types[argument], storage);
        const char *pointer = target->argument_categories[argument] == XR_CORE_IR_PLACE ? " *" : "";
        if (!type || !append_format(buffer, "            %s%s edge_%u = v%u;\n", type, pointer,
                                    argument, source_value))
            return false;
    }
    for (uint32_t argument = target_start; argument < target->argument_count; ++argument) {
        if (!append_format(buffer, "            v%u = edge_%u;\n", target->argument_ids[argument],
                           argument))
            return false;
    }
    return append_format(buffer, "            goto xr_f%u_b%u;\n        }\n", function_id,
                         instruction->successors[successor_index]);
}

static bool emit_invoke(CBuffer *buffer, const XrBackendIR *ir, const XrBackendFunction *function,
                        const XrBackendInstruction *instruction, uint32_t function_id,
                        uint32_t instruction_id) {
    const XrBackendFunction *callee = &ir->functions[instruction->immediate.function_id];
    const XrBackendBlock *normal = &function->blocks[instruction->successors[0]];
    uint32_t normal_implicit = callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
    bool has_error = callee->error_type_id != XR_CORE_TYPE_VOID;
    bool has_panic = callee->panic_type_id != XR_CORE_TYPE_VOID;
    if (has_error) {
        char storage[32];
        const char *type = type_c_name(callee->error_type_id, storage);
        const char *initializer =
            callee->error_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE ? "{0}" : "0";
        if (!type || !append_format(buffer, "        %s invoke_error_%u = %s;\n", type,
                                    instruction_id, initializer))
            return false;
    }
    if (has_panic &&
        !append_format(buffer, "        uint32_t invoke_panic_%u = 0;\n", instruction_id))
        return false;
    if (!append_format(buffer, "        XrAotOutcome call_%u = xr_aot_fn_%u(xr_ctx", instruction_id,
                       instruction->immediate.function_id))
        return false;
    for (uint32_t parameter = 0; parameter < callee->parameter_count; ++parameter) {
        if (!append_format(buffer, ", v%u", instruction->operands[parameter]))
            return false;
    }
    if (callee->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
        if (!append_format(buffer, ", &v%u", normal->argument_ids[0]))
            return false;
    }
    if (has_error) {
        if (!append_format(buffer, ", &invoke_error_%u", instruction_id))
            return false;
    }
    if (has_panic && !append_format(buffer, ", &invoke_panic_%u", instruction_id))
        return false;
    if (!append_text(buffer, ");\n"))
        return false;

    char normal_expression[64];
    const char *normal_value = NULL;
    if (normal_implicit != 0u && callee->result_type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
        const char *field = outcome_field(callee->result_type_id);
        if (!field)
            return false;
        snprintf(normal_expression, sizeof(normal_expression), "call_%u.%s", instruction_id, field);
        normal_value = normal_expression;
    } else if (normal_implicit != 0u) {
        snprintf(normal_expression, sizeof(normal_expression), "v%u", normal->argument_ids[0]);
        normal_value = normal_expression;
    }
    if (!append_format(buffer, "        if (call_%u.kind == 0) ", instruction_id) ||
        !emit_invoke_edge(buffer, function, instruction, 0u, normal_implicit,
                          callee->parameter_count, function_id, normal_value))
        return false;
    uint32_t operand = callee->parameter_count + normal->argument_count - normal_implicit;
    uint32_t successor = 1u;
    if (has_error) {
        char expression[64];
        snprintf(expression, sizeof(expression), "invoke_error_%u", instruction_id);
        if (!append_format(buffer, "        if (call_%u.kind == 2) ", instruction_id) ||
            !emit_invoke_edge(buffer, function, instruction, successor, 1u, operand, function_id,
                              expression))
            return false;
        operand += function->blocks[instruction->successors[successor]].argument_count - 1u;
        ++successor;
    }
    if (has_panic) {
        char expression[64];
        snprintf(expression, sizeof(expression), "invoke_panic_%u", instruction_id);
        if (!append_format(buffer, "        if (call_%u.kind == 3) ", instruction_id) ||
            !emit_invoke_edge(buffer, function, instruction, successor, 1u, operand, function_id,
                              expression))
            return false;
    }
    return append_format(buffer, "        return call_%u;\n", instruction_id);
}

static bool emit_witness_invoke(CBuffer *buffer, const XrBackendIR *ir,
                                const XrBackendFunction *function,
                                const XrBackendInstruction *instruction, uint32_t function_id,
                                uint32_t instruction_id) {
    uint32_t interface_id = 0u;
    const XrValidatedSignature *signature =
        witness_signature(ir, function, instruction, &interface_id);
    if (!signature)
        return false;
    const XrBackendBlock *normal = &function->blocks[instruction->successors[0]];
    uint32_t normal_implicit = signature->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
    bool has_error = signature->error_type_id != XR_CORE_TYPE_VOID;
    bool has_panic = signature->panic_type_id != XR_CORE_TYPE_VOID;
    if (has_error) {
        char storage[32];
        const char *type = type_c_name(signature->error_type_id, storage);
        const char *initializer =
            signature->error_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE ? "{0}" : "0";
        if (!type || !append_format(buffer, "        %s invoke_error_%u = %s;\n", type,
                                    instruction_id, initializer))
            return false;
    }
    if (has_panic &&
        !append_format(buffer, "        uint32_t invoke_panic_%u = 0;\n", instruction_id))
        return false;
    if (!append_format(buffer,
                       "        XrAotOutcome call_%u = xr_aot_make(4, 0, 0);\n"
                       "        switch (v%u.conformance_id) {\n",
                       instruction_id, instruction->operands[0]))
        return false;
    char result_expression[64];
    char error_expression[64];
    char panic_expression[64];
    const char *result = NULL;
    const char *error = NULL;
    const char *panic = NULL;
    if (signature->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
        (void) snprintf(result_expression, sizeof(result_expression), "&v%u",
                        normal->argument_ids[0]);
        result = result_expression;
    }
    if (has_error) {
        (void) snprintf(error_expression, sizeof(error_expression), "&invoke_error_%u",
                        instruction_id);
        error = error_expression;
    }
    if (has_panic) {
        (void) snprintf(panic_expression, sizeof(panic_expression), "&invoke_panic_%u",
                        instruction_id);
        panic = panic_expression;
    }
    if (!emit_witness_call_cases(buffer, ir, instruction, signature, interface_id, instruction_id,
                                 result, error, panic))
        return false;

    char normal_expression[64];
    const char *normal_value = NULL;
    if (normal_implicit != 0u && signature->result_type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
        const char *field = outcome_field(signature->result_type_id);
        if (!field)
            return false;
        (void) snprintf(normal_expression, sizeof(normal_expression), "call_%u.%s", instruction_id,
                        field);
        normal_value = normal_expression;
    } else if (normal_implicit != 0u) {
        (void) snprintf(normal_expression, sizeof(normal_expression), "v%u",
                        normal->argument_ids[0]);
        normal_value = normal_expression;
    }
    if (!append_format(buffer, "        if (call_%u.kind == 0) ", instruction_id) ||
        !emit_invoke_edge(buffer, function, instruction, 0u, normal_implicit,
                          signature->parameter_count, function_id, normal_value))
        return false;
    uint32_t operand = signature->parameter_count + normal->argument_count - normal_implicit;
    uint32_t successor = 1u;
    if (has_error) {
        char expression[64];
        (void) snprintf(expression, sizeof(expression), "invoke_error_%u", instruction_id);
        if (!append_format(buffer, "        if (call_%u.kind == 2) ", instruction_id) ||
            !emit_invoke_edge(buffer, function, instruction, successor, 1u, operand, function_id,
                              expression))
            return false;
        operand += function->blocks[instruction->successors[successor]].argument_count - 1u;
        ++successor;
    }
    if (has_panic) {
        char expression[64];
        (void) snprintf(expression, sizeof(expression), "invoke_panic_%u", instruction_id);
        if (!append_format(buffer, "        if (call_%u.kind == 3) ", instruction_id) ||
            !emit_invoke_edge(buffer, function, instruction, successor, 1u, operand, function_id,
                              expression))
            return false;
    }
    return append_format(buffer, "        return call_%u;\n", instruction_id);
}

static bool emit_callable_invoke(CBuffer *buffer, const XrBackendIR *ir,
                                 const XrBackendFunction *function,
                                 const XrBackendInstruction *instruction, uint32_t function_id,
                                 uint32_t instruction_id) {
    const XrValidatedSignature *signature = callable_signature(ir, function, instruction);
    if (!signature)
        return false;
    const XrBackendBlock *normal = &function->blocks[instruction->successors[0]];
    uint32_t normal_implicit = signature->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
    bool has_error = signature->error_type_id != XR_CORE_TYPE_VOID;
    bool has_panic = signature->panic_type_id != XR_CORE_TYPE_VOID;
    if (has_error) {
        char storage[32];
        const char *type = type_c_name(signature->error_type_id, storage);
        const char *initializer =
            signature->error_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE ? "{0}" : "0";
        if (!type || !append_format(buffer, "        %s invoke_error_%u = %s;\n", type,
                                    instruction_id, initializer))
            return false;
    }
    if (has_panic &&
        !append_format(buffer, "        uint32_t invoke_panic_%u = 0;\n", instruction_id))
        return false;
    if (!append_format(buffer,
                       "        XrAotOutcome call_%u = xr_aot_make(4, 0, 0);\n"
                       "        switch (v%u.function_id) {\n",
                       instruction_id, instruction->operands[0]))
        return false;
    char result_expression[64];
    char error_expression[64];
    char panic_expression[64];
    const char *result = NULL;
    const char *error = NULL;
    const char *panic = NULL;
    if (signature->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
        (void) snprintf(result_expression, sizeof(result_expression), "&v%u",
                        normal->argument_ids[0]);
        result = result_expression;
    }
    if (has_error) {
        (void) snprintf(error_expression, sizeof(error_expression), "&invoke_error_%u",
                        instruction_id);
        error = error_expression;
    }
    if (has_panic) {
        (void) snprintf(panic_expression, sizeof(panic_expression), "&invoke_panic_%u",
                        instruction_id);
        panic = panic_expression;
    }
    if (!emit_callable_call_cases(buffer, ir, function, instruction, signature, instruction_id,
                                  result, error, panic))
        return false;

    char normal_expression[64];
    const char *normal_value = NULL;
    if (normal_implicit != 0u && signature->result_type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
        const char *field = outcome_field(signature->result_type_id);
        if (!field)
            return false;
        (void) snprintf(normal_expression, sizeof(normal_expression), "call_%u.%s", instruction_id,
                        field);
        normal_value = normal_expression;
    } else if (normal_implicit != 0u) {
        (void) snprintf(normal_expression, sizeof(normal_expression), "v%u",
                        normal->argument_ids[0]);
        normal_value = normal_expression;
    }
    uint32_t operand = signature->parameter_count + 1u;
    if (!append_format(buffer, "        if (call_%u.kind == 0) ", instruction_id) ||
        !emit_invoke_edge(buffer, function, instruction, 0u, normal_implicit, operand, function_id,
                          normal_value))
        return false;
    operand += normal->argument_count - normal_implicit;
    uint32_t successor = 1u;
    if (has_error) {
        char expression[64];
        (void) snprintf(expression, sizeof(expression), "invoke_error_%u", instruction_id);
        if (!append_format(buffer, "        if (call_%u.kind == 2) ", instruction_id) ||
            !emit_invoke_edge(buffer, function, instruction, successor, 1u, operand, function_id,
                              expression))
            return false;
        operand += function->blocks[instruction->successors[successor]].argument_count - 1u;
        ++successor;
    }
    if (has_panic) {
        char expression[64];
        (void) snprintf(expression, sizeof(expression), "invoke_panic_%u", instruction_id);
        if (!append_format(buffer, "        if (call_%u.kind == 3) ", instruction_id) ||
            !emit_invoke_edge(buffer, function, instruction, successor, 1u, operand, function_id,
                              expression))
            return false;
    }
    return append_format(buffer, "        return call_%u;\n", instruction_id);
}

static bool emit_callable_copy(CBuffer *buffer, const XrBackendIR *ir,
                               const XrBackendInstruction *instruction) {
    uint16_t callable_type = instruction->result_type_id;
    uint32_t source_value = instruction->operands[0];
    uint32_t result_value = instruction->result_id;
    if (!append_format(buffer,
                       "        v%u = v%u;\n"
                       "        switch (v%u.function_id) {\n",
                       result_value, source_value, source_value))
        return false;
    for (uint32_t target_id = 0; target_id < ir->function_count; ++target_id) {
        if (!callable_type_can_target(ir, callable_type, target_id))
            continue;
        const XrValidatedFunction *target = &ir->program->functions[target_id];
        if (!append_format(buffer, "            case UINT32_C(%u):\n", target_id))
            return false;
        if (!target->has_receiver) {
            if (!append_format(buffer,
                               "                v%u.capture = NULL;\n"
                               "                break;\n",
                               result_value))
                return false;
            continue;
        }
        char storage[32];
        const char *capture_type = type_c_name(target->parameter_types[0], storage);
        if (!capture_type ||
            !append_format(buffer,
                           "                { %s *callable_capture_%u = "
                           "(%s *)xr_aot_alloc(xr_ctx, sizeof(%s));\n"
                           "                  if (!callable_capture_%u) return "
                           "xr_aot_make(4, 0, 0);\n"
                           "                  *callable_capture_%u = "
                           "*(const %s *)v%u.capture;\n"
                           "                  v%u.capture = (void *)callable_capture_%u; }\n"
                           "                break;\n",
                           capture_type, result_value, capture_type, capture_type, result_value,
                           result_value, capture_type, source_value, result_value, result_value))
            return false;
    }
    return append_text(buffer, "            default:\n"
                               "                return xr_aot_make(4, 0, 0);\n"
                               "        }\n");
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
                                     "xr_aot_make(1, 0, 1);\n",
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
                                 "        if (v%u == 0) return xr_aot_make(1, 0, 2);\n"
                                 "        if (v%u == INT64_MIN && v%u == -1) return "
                                 "xr_aot_make(1, 0, 3);\n"
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
            if (function->coroutine_safepoint_count != 0u &&
                !append_text(buffer, "        frame->state = UINT32_MAX;\n"))
                return false;
            return emit_return(buffer, function, instruction);
        case XR_CORE_OP_CORE_COROUTINE_YIELD: {
            uint32_t safepoint_id = instruction->immediate.u32;
            const XrBackendCoroutineSafepoint *safepoint =
                &function->coroutine_safepoints[safepoint_id];
            if (instruction->operand_count != safepoint->live_value_count)
                return false;
            for (uint32_t live = 0; live < instruction->operand_count; ++live) {
                if (!append_format(buffer, "        frame->live_%u_%u = v%u;\n", safepoint_id, live,
                                   instruction->operands[live]))
                    return false;
            }
            return append_format(buffer,
                                 "        frame->state = UINT32_C(%u);\n"
                                 "        return xr_aot_make(5, UINT32_C(%u), 0);\n",
                                 safepoint->resume_state_id, safepoint_id);
        }
        case XR_CORE_OP_CORE_GENERATOR_YIELD: {
            uint32_t safepoint_id = instruction->immediate.u32;
            const XrBackendCoroutineSafepoint *safepoint =
                &function->coroutine_safepoints[safepoint_id];
            if (instruction->operand_count != safepoint->live_value_count + 1u ||
                function->value_types[instruction->operands[0]] != XR_CORE_TYPE_I64)
                return false;
            for (uint32_t live = 0u; live < safepoint->live_value_count; ++live) {
                if (!append_format(buffer, "        frame->live_%u_%u = v%u;\n", safepoint_id,
                                   live, instruction->operands[live + 1u]))
                    return false;
            }
            return append_format(buffer,
                                 "        frame->state = UINT32_C(%u);\n"
                                 "        { XrAotOutcome yielded = xr_aot_make(5, "
                                 "UINT32_C(%u), 0); yielded.i64 = v%u; return yielded; }\n",
                                 safepoint->resume_state_id, safepoint_id,
                                 instruction->operands[0]);
        }
        case XR_CORE_OP_CORE_GENERATOR_CREATE: {
            uint32_t target = instruction->immediate.function_id;
            if (target >= ir->function_count || instruction->operand_count != 0u ||
                (ir->functions[target].flags & XR_PROGRAM_FUNCTION_GENERATOR) == 0u)
                return false;
            return append_format(
                buffer,
                "        XrAotCoroutineFrame%u *generator_frame_%u = "
                "(XrAotCoroutineFrame%u *)xr_aot_alloc(xr_ctx, "
                "sizeof(XrAotCoroutineFrame%u));\n"
                "        if (!generator_frame_%u) return xr_aot_make(4, 0, 0);\n"
                "        *generator_frame_%u = (XrAotCoroutineFrame%u){0};\n"
                "        v%u.private_frame = (void *)generator_frame_%u;\n"
                "        v%u.function_id = UINT32_C(%u);\n",
                target, instruction->result_id, target, target, instruction->result_id,
                instruction->result_id, target, instruction->result_id, instruction->result_id,
                instruction->result_id, target);
        }
        case XR_CORE_OP_CORE_GENERATOR_RESUME: {
            uint32_t handle_value = instruction->operands[0];
            uint16_t handle_type = function->value_types[handle_value];
            uint32_t target = generator_target_for_handle(ir, handle_type);
            if (target == UINT32_MAX || instruction->result_type_id <
                                            XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE)
                return false;
            return append_format(
                buffer,
                "        if (v%u.function_id != UINT32_C(%u) || !v%u.private_frame) "
                "return xr_aot_make(4, 0, 0);\n"
                "        XrAotOutcome generator_step_%u = xr_aot_fn_%u_step("
                "xr_ctx, (XrAotCoroutineFrame%u *)v%u.private_frame);\n"
                "        if (generator_step_%u.kind == UINT32_C(5)) {\n"
                "            v%u.tag = UINT32_C(0);\n"
                "            v%u.payload.case_0.f0 = generator_step_%u.i64;\n"
                "        } else if (generator_step_%u.kind == UINT32_C(0)) {\n"
                "            v%u.tag = UINT32_C(1);\n"
                "        } else if (generator_step_%u.kind == UINT32_C(2)) {\n"
                "            v%u.tag = UINT32_C(2);\n"
                "            v%u.payload.case_2.f0 = generator_step_%u.error;\n"
                "        } else if (generator_step_%u.kind == UINT32_C(3)) {\n"
                "            v%u.tag = UINT32_C(3);\n"
                "            v%u.payload.case_3.f0 = generator_step_%u.trap;\n"
                "        } else {\n"
                "            v%u.tag = UINT32_C(4);\n"
                "        }\n",
                handle_value, target, handle_value, instruction_id, target, target, handle_value,
                instruction_id, instruction->result_id, instruction->result_id, instruction_id,
                instruction_id, instruction->result_id, instruction_id, instruction->result_id,
                instruction->result_id, instruction_id, instruction_id, instruction->result_id,
                instruction->result_id, instruction_id, instruction->result_id);
        }
        case XR_CORE_OP_CORE_CALL_SEALED_DIRECT:
            return emit_call(buffer, ir, instruction, instruction_id);
        case XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT:
            return emit_callable_call(buffer, ir, function, instruction, instruction_id);
        case XR_CORE_OP_CORE_CALL_SEALED_INVOKE:
            return emit_invoke(buffer, ir, function, instruction, function_id, instruction_id);
        case XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE:
            return emit_callable_invoke(buffer, ir, function, instruction, function_id,
                                        instruction_id);
        case XR_CORE_OP_CORE_CALL_WITNESS_DIRECT:
            return emit_witness_call(buffer, ir, function, instruction, instruction_id);
        case XR_CORE_OP_CORE_CALL_WITNESS_INVOKE:
            return emit_witness_invoke(buffer, ir, function, instruction, function_id,
                                       instruction_id);
        case XR_CORE_OP_CORE_TRAP:
            return append_text(buffer, "        return xr_aot_make(1, 0, 4);\n");
        case XR_CORE_OP_CORE_ERROR_PUBLISH:
            return append_format(buffer,
                                 "        *out_error = v%u;\n"
                                 "        return xr_aot_make(2, 0, 0);\n",
                                 instruction->operands[0]);
        case XR_CORE_OP_CORE_PANIC_PUBLISH:
            return append_format(buffer,
                                 "        *out_panic = v%u;\n"
                                 "        return xr_aot_make(3, 0, 0);\n",
                                 instruction->operands[0]);
        case XR_CORE_OP_CORE_TARGET_POINTER_WIDTH:
            return append_format(buffer, "        v%u = UINT16_C(%u);\n", instruction->result_id,
                                 ir->pointer_width);
        case XR_CORE_OP_CORE_CALLABLE_PACK: {
            uint32_t target_id = instruction->immediate.function_id;
            if (instruction->operand_count == 0u)
                return append_format(buffer,
                                     "        v%u = (XrAotType%u){.function_id = "
                                     "UINT32_C(%u), .capture = NULL};\n",
                                     instruction->result_id, instruction->result_type_id,
                                     target_id);
            uint16_t capture_type = function->value_types[instruction->operands[0]];
            char storage[32];
            const char *name = type_c_name(capture_type, storage);
            if (!name ||
                !append_format(buffer,
                               "        %s *callable_capture_%u = "
                               "(%s *)xr_aot_alloc(xr_ctx, sizeof(%s));\n",
                               name, instruction->result_id, name, name) ||
                !append_format(buffer,
                               "        if (!callable_capture_%u) return "
                               "xr_aot_make(4, 0, 0);\n"
                               "        *callable_capture_%u = v%u;\n",
                               instruction->result_id, instruction->result_id,
                               instruction->operands[0]))
                return false;
            return append_format(buffer,
                                 "        v%u = (XrAotType%u){.function_id = UINT32_C(%u), "
                                 ".capture = (void *)callable_capture_%u};\n",
                                 instruction->result_id, instruction->result_type_id, target_id,
                                 instruction->result_id);
        }
        case XR_CORE_OP_CORE_OWNER_COPY: {
            const XrValidatedType *type =
                xr_validated_program_type(ir->program, instruction->result_type_id);
            if (type && type->kind == XR_CORE_IR_TYPE_CALLABLE)
                return emit_callable_copy(buffer, ir, instruction);
            return append_format(buffer, "        v%u = v%u;\n", instruction->result_id,
                                 instruction->operands[0]);
        }
        case XR_CORE_OP_CORE_OWNER_MOVE:
            return append_format(buffer, "        v%u = v%u;\n", instruction->result_id,
                                 instruction->operands[0]);
        case XR_CORE_OP_CORE_OWNER_DROP:
            if (type_is_generator_handle(ir,
                                         function->value_types[instruction->operands[0]])) {
                uint32_t target = generator_target_for_handle(
                    ir, function->value_types[instruction->operands[0]]);
                if (target == UINT32_MAX)
                    return false;
                return append_format(
                    buffer,
                    "        if (v%u.private_frame) ((XrAotCoroutineFrame%u *)"
                    "v%u.private_frame)->state = UINT32_MAX;\n",
                    instruction->operands[0], target, instruction->operands[0]);
            }
            return append_format(buffer, "        (void)v%u;\n", instruction->operands[0]);
        case XR_CORE_OP_CORE_PLACE_LOCAL:
            return append_format(buffer,
                                 "        xr_place_%u = v%u;\n        v%u = &xr_place_%u;\n",
                                 instruction->result_id, instruction->operands[0],
                                 instruction->result_id, instruction->result_id);
        case XR_CORE_OP_CORE_PLACE_LOAD:
            return append_format(buffer, "        v%u = *v%u;\n", instruction->result_id,
                                 instruction->operands[0]);
        case XR_CORE_OP_CORE_PLACE_STORE:
            return append_format(buffer, "        *v%u = v%u;\n", instruction->operands[0],
                                 instruction->operands[1]);
        case XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT: {
            char storage[32];
            const char *name = type_c_name(instruction->result_type_id, storage);
            if (!name ||
                !append_format(buffer, "        v%u = (%s){", instruction->result_id, name))
                return false;
            if (instruction->operand_count == 0u && !append_text(buffer, ".xr_unit = UINT8_C(0)"))
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
                "        if (v%u.tag != UINT32_C(%u)) return xr_aot_make(1, 0, 6);\n"
                "        v%u = v%u.payload.case_%u.f%u;\n",
                instruction->operands[0], instruction->immediate.variant_field.variant_ordinal,
                instruction->result_id, instruction->operands[0],
                instruction->immediate.variant_field.variant_ordinal,
                instruction->immediate.variant_field.field_ordinal);
        case XR_CORE_OP_CORE_EXISTENTIAL_PACK: {
            uint16_t concrete_type = function->value_types[instruction->operands[0]];
            const XrValidatedType *existential =
                xr_validated_program_type(ir->program, instruction->result_type_id);
            uint32_t conformance = 0u;
            if (!existential || existential->kind != XR_CORE_IR_TYPE_EXISTENTIAL ||
                !conformance_id(ir->program, concrete_type, existential->interface_id,
                                &conformance))
                return false;
            if (function->value_categories[instruction->operands[0]] == XR_CORE_IR_PLACE)
                return append_format(
                    buffer,
                    "        v%u = (XrAotType%u){.concrete_type_id = UINT16_C(%u), "
                    ".conformance_id = UINT32_C(%u), .data = (void *)v%u};\n",
                    instruction->result_id, instruction->result_type_id, concrete_type, conformance,
                    instruction->operands[0]);
            char storage[32];
            const char *name = type_c_name(concrete_type, storage);
            if (!name ||
                !append_format(buffer,
                               "        %s *existential_payload_%u = "
                               "(%s *)xr_aot_alloc(xr_ctx, sizeof(%s));\n",
                               name, instruction->result_id, name, name) ||
                !append_format(buffer,
                               "        if (!existential_payload_%u) return "
                               "xr_aot_make(4, 0, 0);\n",
                               instruction->result_id) ||
                !append_format(buffer, "        *existential_payload_%u = v%u;\n",
                               instruction->result_id, instruction->operands[0]))
                return false;
            return append_format(buffer,
                                 "        v%u = (XrAotType%u){.concrete_type_id = UINT16_C(%u), "
                                 ".conformance_id = UINT32_C(%u), "
                                 ".data = (void *)existential_payload_%u};\n",
                                 instruction->result_id, instruction->result_type_id, concrete_type,
                                 conformance, instruction->result_id);
        }
        case XR_CORE_OP_CORE_EXISTENTIAL_TEST:
            return append_format(buffer,
                                 "        v%u = (uint8_t)(v%u.concrete_type_id == "
                                 "UINT16_C(%u));\n",
                                 instruction->result_id, instruction->operands[0],
                                 instruction->immediate.type_id);
        case XR_CORE_OP_CORE_EXISTENTIAL_PROJECT: {
            char storage[32];
            const char *name = type_c_name(instruction->immediate.type_id, storage);
            if (!name)
                return false;
            if (instruction->result_category == XR_CORE_IR_PLACE)
                return append_format(buffer, "        v%u = (%s *)v%u.data;\n",
                                     instruction->result_id, name, instruction->operands[0]);
            return append_format(buffer, "        v%u = *(const %s *)v%u.data;\n",
                                 instruction->result_id, name, instruction->operands[0]);
        }
        default:
            return false;
    }
}

static bool emit_function(CBuffer *buffer, const XrBackendIR *ir, uint32_t function_id) {
    const XrBackendFunction *function = &ir->functions[function_id];
    if (!emit_function_signature(buffer, function, ir, function_id, false))
        return false;
    if (!append_text(buffer, "    (void)xr_ctx;\n"))
        return false;
    for (uint32_t parameter = 0; parameter < function->parameter_count; ++parameter) {
        if (!append_format(buffer, "    (void)p%u;\n", parameter))
            return false;
    }
    if (function->error_type_id != XR_CORE_TYPE_VOID &&
        !append_text(buffer, "    (void)out_error;\n"))
        return false;
    if (function->panic_type_id != XR_CORE_TYPE_VOID &&
        !append_text(buffer, "    (void)out_panic;\n"))
        return false;
    for (uint32_t value = 0; value < function->value_count; ++value) {
        char storage[32];
        const char *type = type_c_name(function->value_types[value], storage);
        const char *initializer =
            function->value_types[value] >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE ? "{0}" : "0";
        const char *pointer = function->value_categories[value] == XR_CORE_IR_PLACE ? " *" : "";
        const char *value_initializer =
            function->value_categories[value] == XR_CORE_IR_PLACE ? "0" : initializer;
        if (!type ||
            !append_format(buffer, "    %s%s v%u = %s;\n", type, pointer, value,
                           value_initializer) ||
            !append_format(buffer, "    (void)v%u;\n", value))
            return false;
        if (function->value_categories[value] == XR_CORE_IR_PLACE &&
            (!append_format(buffer, "    %s xr_place_%u = %s;\n", type, value, initializer) ||
             !append_format(buffer, "    (void)xr_place_%u;\n", value)))
            return false;
    }
    const XrBackendBlock *entry = &function->blocks[function->entry_block];
    if (entry->argument_count != function->parameter_count)
        return false;
    for (uint32_t parameter = 0; parameter < function->parameter_count; ++parameter) {
        if (!append_format(buffer, "    v%u = p%u;\n", entry->argument_ids[parameter], parameter))
            return false;
    }
    if (function->coroutine_safepoint_count != 0u) {
        if (!append_text(buffer, "    if (!frame) return xr_aot_make(4, 0, 0);\n"
                                 "    switch (frame->state) {\n") ||
            !append_format(buffer, "        case UINT32_C(0): goto xr_f%u_b%u;\n", function_id,
                           function->entry_block))
            return false;
        for (uint32_t state = 1u; state < function->coroutine_state_count; ++state) {
            uint32_t resume_block = function->coroutine_states[state].continuation_block;
            const XrBackendBlock *resume = &function->blocks[resume_block];
            const XrBackendCoroutineSafepoint *point = NULL;
            uint32_t safepoint_id = 0u;
            for (; safepoint_id < function->coroutine_safepoint_count; ++safepoint_id) {
                if (function->coroutine_safepoints[safepoint_id].resume_state_id == state) {
                    point = &function->coroutine_safepoints[safepoint_id];
                    break;
                }
            }
            if (!point || resume->argument_count != point->live_value_count ||
                !append_format(buffer, "        case UINT32_C(%u):\n", state))
                return false;
            for (uint32_t live = 0; live < point->live_value_count; ++live) {
                if (!append_format(buffer, "            v%u = frame->live_%u_%u;\n",
                                   resume->argument_ids[live], safepoint_id, live))
                    return false;
            }
            if (!append_format(buffer, "            goto xr_f%u_b%u;\n", function_id, resume_block))
                return false;
        }
        if (!append_text(buffer, "        default: return xr_aot_make(4, 0, 0);\n    }\n"))
            return false;
    } else if (!append_format(buffer, "    goto xr_f%u_b%u;\n", function_id,
                              function->entry_block)) {
        return false;
    }
    uint32_t instruction_serial = 0u;
    for (uint32_t block_id = 0; block_id < function->block_count; ++block_id) {
        const XrBackendBlock *block = &function->blocks[block_id];
        if (!append_format(buffer, "xr_f%u_b%u:\n    ;\n", function_id, block_id))
            return false;
        for (uint32_t instruction = 0; instruction < block->instruction_count;
             ++instruction, ++instruction_serial) {
            if (!emit_instruction(buffer, ir, function, &block->instructions[instruction],
                                  function_id, instruction_serial))
                return false;
        }
    }
    return append_text(buffer, "}\n\n");
}

static bool emit_coroutine_entry_adapter(CBuffer *buffer, const XrBackendIR *ir) {
    const XrBackendFunction *entry = &ir->functions[ir->entry_function];
    if (entry->coroutine_safepoint_count == 0u)
        return true;
    if (entry->parameter_count != 0u || entry->result_type_id != XR_CORE_TYPE_I64 ||
        entry->error_type_id != XR_CORE_TYPE_VOID || entry->panic_type_id != XR_CORE_TYPE_VOID)
        return false;
    bool checked = false;
    bool wrapping = false;
    bool arena = false;
    scan_helpers(ir, &checked, &wrapping, &arena);
    if (!append_format(buffer,
                       "typedef struct XrAotEntryCoroutineFrame {\n"
                       "    XrAotContext context;\n"
                       "    XrAotCoroutineFrame%u function;\n"
                       "} XrAotEntryCoroutineFrame;\n\n"
                       "typedef struct XrBackendNativeOutcome {\n"
                       "    uint32_t kind;\n"
                       "    int64_t value;\n"
                       "    uint32_t state_id;\n"
                       "    uint32_t safepoint_id;\n"
                       "} XrBackendNativeOutcome;\n\n"
                       "typedef struct XrBackendNativeExecutionId {\n"
                       "    uint8_t bytes[32];\n"
                       "} XrBackendNativeExecutionId;\n\n"
                       "typedef void (*XrBackendNativeInitialize)(void *frame);\n"
                       "typedef XrBackendNativeOutcome (*XrBackendNativeStep)(void *frame);\n"
                       "typedef void (*XrBackendNativeDrop)(void *frame);\n\n"
                       "typedef struct XrBackendNativeDescriptor {\n"
                       "    uint32_t schema_version;\n"
                       "    uint32_t reserved32;\n"
                       "    XrBackendNativeExecutionId execution_id;\n"
                       "    size_t frame_size;\n"
                       "    XrBackendNativeInitialize initialize;\n"
                       "    XrBackendNativeStep step;\n"
                       "    XrBackendNativeDrop drop;\n"
                       "} XrBackendNativeDescriptor;\n\n"
                       "size_t xr_aot_entry_coroutine_frame_size(void) {\n"
                       "    return sizeof(XrAotEntryCoroutineFrame);\n"
                       "}\n\n"
                       "void xr_aot_entry_coroutine_frame_initialize(void *opaque) {\n"
                       "    if (!opaque) return;\n"
                       "    XrAotEntryCoroutineFrame zero = {0};\n"
                       "    *(XrAotEntryCoroutineFrame *)opaque = zero;\n"
                       "}\n\n",
                       ir->entry_function))
        return false;
    if (!append_text(buffer, "void xr_aot_entry_coroutine_frame_dispose(void *opaque) {\n"
                             "    if (!opaque) return;\n") ||
        (arena && !append_text(buffer, "    xr_aot_context_destroy("
                                       "&((XrAotEntryCoroutineFrame *)opaque)->context);\n")) ||
        !append_text(buffer,
                     "    ((XrAotEntryCoroutineFrame *)opaque)->function.state = UINT32_MAX;\n"
                     "}\n\n") ||
        !append_format(buffer,
                       "XrBackendNativeOutcome xr_aot_entry_coroutine_step(void *opaque) {\n"
                       "    XrBackendNativeOutcome invalid = {UINT32_C(3), 0, 0, 0};\n"
                       "    if (!opaque) return invalid;\n"
                       "    XrAotEntryCoroutineFrame *frame = "
                       "(XrAotEntryCoroutineFrame *)opaque;\n"
                       "    XrAotOutcome native = xr_aot_fn_%u_step(&frame->context, "
                       "&frame->function);\n"
                       "    if (native.kind == UINT32_C(0)) {\n"
                       "        XrBackendNativeOutcome result = {UINT32_C(0), native.i64, "
                       "frame->function.state, 0};\n"
                       "        return result;\n"
                       "    }\n"
                       "    if (native.kind == UINT32_C(5)) {\n"
                       "        XrBackendNativeOutcome result = {UINT32_C(1), 0, "
                       "frame->function.state, native.value_kind};\n"
                       "        return result;\n"
                       "    }\n"
                       "    if (native.kind == UINT32_C(1)) {\n"
                       "        XrBackendNativeOutcome result = {UINT32_C(2), 0, "
                       "frame->function.state, native.trap};\n"
                       "        return result;\n"
                       "    }\n"
                       "    return invalid;\n"
                       "}\n\n",
                       ir->entry_function))
        return false;
    if (!append_text(buffer, "const XrBackendNativeDescriptor "
                             "xr_aot_entry_coroutine_descriptor = {\n"
                             "    UINT32_C(1),\n"
                             "    UINT32_C(0),\n"
                             "    {{"))
        return false;
    for (size_t index = 0u; index < sizeof(ir->execution_id.bytes); ++index) {
        if (!append_format(buffer, "%sUINT8_C(%u)", index == 0u ? "" : ", ",
                           (unsigned) ir->execution_id.bytes[index]))
            return false;
    }
    return append_text(buffer, "}},\n"
                               "    sizeof(XrAotEntryCoroutineFrame),\n"
                               "    xr_aot_entry_coroutine_frame_initialize,\n"
                               "    xr_aot_entry_coroutine_step,\n"
                               "    xr_aot_entry_coroutine_frame_dispose,\n"
                               "};\n\n");
}

static bool emit_main(CBuffer *buffer, const XrBackendIR *ir) {
    const XrBackendFunction *entry = &ir->functions[ir->entry_function];
    bool checked = false;
    bool wrapping = false;
    bool arena = false;
    scan_helpers(ir, &checked, &wrapping, &arena);
    if (entry->parameter_count != 0u)
        return false;
    if (!append_text(buffer, "int main(void) {\n    XrAotContext xr_ctx = {0};\n"))
        return false;
    if (entry->error_type_id != XR_CORE_TYPE_VOID) {
        char storage[32];
        const char *type = type_c_name(entry->error_type_id, storage);
        const char *initializer =
            entry->error_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE ? "{0}" : "0";
        if (!type || !append_format(buffer, "    %s entry_error = %s;\n", type, initializer))
            return false;
    }
    if (entry->panic_type_id != XR_CORE_TYPE_VOID &&
        !append_text(buffer, "    uint32_t entry_panic = 0;\n"))
        return false;
    if (entry->coroutine_safepoint_count != 0u) {
        if (entry->error_type_id != XR_CORE_TYPE_VOID ||
            entry->panic_type_id != XR_CORE_TYPE_VOID ||
            !append_format(buffer,
                           "    XrAotCoroutineFrame%u frame = {0};\n"
                           "    XrAotOutcome result;\n"
                           "    do { result = xr_aot_fn_%u_step(&xr_ctx, &frame); } "
                           "while (result.kind == UINT32_C(5));\n",
                           ir->entry_function, ir->entry_function))
            return false;
    } else if (!append_format(buffer, "    XrAotOutcome result = xr_aot_fn_%u(&xr_ctx",
                              ir->entry_function)) {
        return false;
    }
    if (entry->coroutine_safepoint_count == 0u && entry->error_type_id != XR_CORE_TYPE_VOID) {
        if (!append_text(buffer, ", &entry_error"))
            return false;
    }
    if (entry->coroutine_safepoint_count == 0u && entry->panic_type_id != XR_CORE_TYPE_VOID &&
        !append_text(buffer, ", &entry_panic"))
        return false;
    if (entry->coroutine_safepoint_count == 0u && !append_text(buffer, ");\n"))
        return false;
    if (!append_text(buffer, "    int exit_code = (int)(200u + result.kind * 10u + result.trap);\n"
                             "    if (result.kind == 0) {\n"))
        return false;
    switch (entry->result_type_id) {
        case XR_CORE_TYPE_VOID:
            if (!append_text(buffer, "        exit_code = 0;\n"))
                return false;
            break;
        case XR_CORE_TYPE_BOOL:
            if (!append_text(buffer, "        exit_code = result.boolean ? 1 : 0;\n"))
                return false;
            break;
        case XR_CORE_TYPE_I64:
            if (!append_text(buffer,
                             "        exit_code = (int)((uint64_t)result.i64 & UINT64_C(255));\n"))
                return false;
            break;
        case XR_CORE_TYPE_U32:
            if (!append_text(buffer, "        exit_code = (int)(result.u32 & UINT32_C(255));\n"))
                return false;
            break;
        case XR_CORE_TYPE_U16:
            if (!append_text(buffer, "        exit_code = (int)(result.u16 & UINT16_C(255));\n"))
                return false;
            break;
        case XR_CORE_TYPE_ERROR:
            if (!append_text(buffer, "        exit_code = (int)(result.error & UINT32_C(255));\n"))
                return false;
            break;
        default:
            return false;
    }
    if (!append_text(buffer, "    }\n"))
        return false;
    if (arena && !append_text(buffer, "    xr_aot_context_destroy(&xr_ctx);\n"))
        return false;
    return append_text(buffer, "    return exit_code;\n}\n");
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
    CBuffer buffer = {0};
    bool emitted = emit_prelude(&buffer, ir);
    emitted = emitted && emit_coroutine_frames(&buffer, ir);
    for (uint32_t function = 0; emitted && function < ir->function_count; ++function)
        emitted = emit_function_signature(&buffer, &ir->functions[function], ir, function, true);
    emitted = emitted && append_text(&buffer, "\n");
    for (uint32_t function = 0; emitted && function < ir->function_count; ++function)
        emitted = emit_function(&buffer, ir, function);
    emitted = emitted && emit_coroutine_entry_adapter(&buffer, ir);
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
