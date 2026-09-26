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
#include "../../plan/semantic/xr_semantic_ids.h"
#include "../../runtime/abi/xr_builtin_provider_contract.h"
#include "../xi_cgen_verify_output.h"
#include "xr_text_kernel_embedded.inc.c"
#include "xr_array_allocation_kernel_embedded.inc.c"
#include "xr_array_append_kernel_embedded.inc.c"
#include "xr_float_format_embedded.inc.c"
#include "xr_integer_kernel_embedded.inc.c"
#include "xr_integer_division_kernel_embedded.inc.c"
#include "xr_integer_bitwise_kernel_embedded.inc.c"
#include "xr_value_format_embedded.inc.c"
#include "xr_atomic_compat_embedded.inc.c"
#include "xr_semantic_owner_ids_gen_embedded.inc.c"
#include "xr_sync_core_embedded.inc.c"
#include "xr_channel_buffer_embedded.inc.c"
#include "xr_channel_storage_embedded.inc.c"
#include "xr_byte_compare_core_embedded.inc.c"
#include "xr_buffer_capacity_core_embedded.inc.c"
#include "xr_string_builder_storage_embedded.inc.c"
#include "xstable_id_embedded.inc.c"
#include "xr_provider_value_embedded.inc.c"

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
    const XrCoreIntegerType *integer = xr_core_spec_integer_type(type_id);
    if (integer) {
        (void) snprintf(storage, 32u, "%sint%u_t", integer->is_signed ? "" : "u", integer->width);
        return storage;
    }
    switch (type_id) {
        case XR_CORE_TYPE_F64:
            return "double";
        case XR_CORE_TYPE_BOOL:
            return "uint8_t";
        case XR_CORE_TYPE_ERROR:
            return "uint32_t";
        case XR_CORE_TYPE_PANIC_INFO:
            return "XrAotPanicInfo";
        case XR_CORE_TYPE_TARGET_OS:
        case XR_CORE_TYPE_TARGET_ARCH:
        case XR_CORE_TYPE_TARGET_ABI:
        case XR_CORE_TYPE_TARGET_ENDIAN:
            return "uint16_t";
        case XR_CORE_TYPE_VOID:
            return "void";
        case XR_CORE_TYPE_STRING_BUILDER:
            return "XrStringBuilderStorage *";
        case XR_CORE_TYPE_STRING:
            return "XrAotString *";
        case XR_CORE_TYPE_RUNE:
            return "uint32_t";
        default:
            if (type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE)
                return NULL;
            (void) snprintf(storage, 32u, "XrAotType%u", type_id);
            return storage;
    }
}

static uint32_t outcome_value_kind(uint16_t type_id) {
    switch (type_id) {
        case XR_CORE_TYPE_F64:
            return 15u;
        case XR_CORE_TYPE_I8:
            return 10u;
        case XR_CORE_TYPE_U8:
            return 11u;
        case XR_CORE_TYPE_I16:
            return 12u;
        case XR_CORE_TYPE_I32:
            return 13u;
        case XR_CORE_TYPE_U64:
            return 14u;

        case XR_CORE_TYPE_VOID:
            return 0u;
        case XR_CORE_TYPE_BOOL:
            return 1u;
        case XR_CORE_TYPE_I64:
            return 2u;
        case XR_CORE_TYPE_U32:
            return 3u;
        case XR_CORE_TYPE_PANIC_INFO:
            return 7u;
        case XR_CORE_TYPE_ERROR:
            return 4u;
        case XR_CORE_TYPE_U16:
        case XR_CORE_TYPE_TARGET_OS:
        case XR_CORE_TYPE_TARGET_ARCH:
        case XR_CORE_TYPE_TARGET_ABI:
        case XR_CORE_TYPE_TARGET_ENDIAN:
            return 6u;
        case XR_CORE_TYPE_STRING_BUILDER:
            return 15u;
        case XR_CORE_TYPE_STRING:
            return 8u;
        case XR_CORE_TYPE_RUNE:
            return 9u;
        default:
            return type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE ? 5u : UINT32_MAX;
    }
}

static bool emit_allocation_alignment(CBuffer *buffer, const char *type, const char *indent) {
    return append_format(buffer,
                         "%s_Static_assert(_Alignof(XrAotAllocation) >= _Alignof(%s) && "
                         "sizeof(XrAotAllocation) %% _Alignof(%s) == 0, "
                         "\"allocation payload alignment is unsupported\");\n",
                         indent, type, type);
}

static bool emit_allocation_profile_abi(CBuffer *buffer, const XrBackendIR *ir) {
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(ir->profile);
    if (!machine)
        return false;
    const XrTargetDataLayout *layout = &machine->data_layout;
    const char *types[] = {"uint8_t", "uint16_t", "uint32_t", "int64_t", "void *"};
    const XrTargetTypeLayout *facts[] = {&layout->u8, &layout->u16, &layout->u32, &layout->i64,
                                         &layout->pointer};
    for (size_t index = 0u; index < sizeof(types) / sizeof(types[0]); ++index) {
        if (!append_format(buffer,
                           "_Static_assert(sizeof(%s) == UINT32_C(%u) && "
                           "_Alignof(%s) == UINT32_C(%u), "
                           "\"allocation ABI disagrees with TargetProfile\");\n",
                           types[index], facts[index]->size, types[index], facts[index]->align))
            return false;
    }
    return append_text(buffer, "\n");
}

static bool type_is_class_reference(const XrBackendIR *ir, uint16_t type_id) {
    const XrValidatedType *type = ir ? xr_validated_program_type(ir->program, type_id) : NULL;
    return type && type->kind == XR_CORE_IR_TYPE_CLASS_REFERENCE;
}

static bool type_is_reference_record(const XrBackendIR *ir, uint16_t type_id) {
    const XrValidatedType *type = ir ? xr_validated_program_type(ir->program, type_id) : NULL;
    return type && xr_program_type_kind_is_reference_record(type->kind);
}

static bool parameter_is_class_receiver(const XrBackendIR *ir, const XrValidatedFunction *function,
                                        uint32_t parameter) {
    return function && parameter == 0u && function->parameter_count != 0u &&
           function->parameter_modes[0] == XR_PARAM_REF &&
           type_is_class_reference(ir, function->parameter_types[0]);
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
        if (child >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE && !type_is_reference_record(ir, child) &&
            !emit_type_definition(buffer, ir, child - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE, state))
            return false;
    }
    for (uint32_t variant = 0; variant < type->variant_count; ++variant) {
        for (uint32_t field = 0; field < type->variants[variant].payload_count; ++field) {
            uint16_t child = type->variants[variant].payload_types[field];
            if (child >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE && !type_is_reference_record(ir, child) &&
                !emit_type_definition(buffer, ir, child - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE, state))
                return false;
        }
    }
    if (xr_program_type_kind_is_reference_record(type->kind)) {
        if (!append_format(buffer,
                           "struct XrAotClass%u {\n"
                           "    uint32_t owners;\n"
                           "    uint64_t identity;\n",
                           type->type_id))
            return false;
        for (uint32_t field = 0; field < type->field_count; ++field) {
            char storage[32];
            const char *name = type_c_name(type->field_types[field], storage);
            if (!name || !append_format(buffer, "    %s f%u;\n", name, field))
                return false;
        }
        if (!append_text(buffer, "};\n\n"))
            return false;
        state[index] = 2u;
        return true;
    }
    if (!append_format(buffer, "struct XrAotType%u {\n", type->type_id))
        return false;
    if (type->kind == XR_CORE_IR_TYPE_AGGREGATE) {
        if (type->field_count == 0u && !append_text(buffer, "    uint8_t xr_unit;\n"))
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
    } else if (type->kind == XR_CORE_IR_TYPE_PROVIDER_RESOURCE) {
        if (!append_text(buffer, "    XrExecutionResource *owner;\n"
                                 "    void (*release)(XrExecutionResource **);\n"))
            return false;
    } else if (type->kind == XR_CORE_IR_TYPE_CHANNEL) {
        if (!append_text(buffer, "    XrChannelStorage *storage;\n"))
            return false;
    } else if (type->kind == XR_CORE_IR_TYPE_ATOMIC) {
        if (!append_text(buffer, "    XrAtomicStorageCore *storage;\n"))
            return false;
    } else if (type->kind == XR_CORE_IR_TYPE_ARRAY) {
        char storage[32];
        const char *element = type_c_name(type->array_element_type, storage);
        if (!element || !append_format(buffer,
                "    struct XrAotArrayStorage%u *storage;\n};\n"
                "struct XrAotArrayStorage%u {\n"
                "    uint32_t owners;\n    size_t length;\n    uint32_t capacity;\n    %s *data;\n",
                type->type_id, type->type_id, element))
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
        const XrValidatedType *type = &ir->program->types[index];
        bool emitted =
            xr_program_type_kind_is_reference_record(type->kind)
                ? append_format(buffer,
                                "typedef struct XrAotClass%u XrAotClass%u;\n"
                                "typedef XrAotClass%u *XrAotType%u;\n",
                                type->type_id, type->type_id, type->type_id, type->type_id)
                : append_format(buffer, "typedef struct XrAotType%u XrAotType%u;\n", type->type_id,
                                type->type_id);
        if (!emitted)
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

static bool callable_type_can_target(const XrBackendIR *ir, uint16_t callable_type,
                                     uint32_t target_function);
static bool has_class_reference_types(const XrBackendIR *ir);

static bool has_panic_messages(const XrBackendIR *ir) {
    for (uint32_t f = 0u; f < ir->program->function_count; ++f) {
        const XrValidatedFunction *function = &ir->program->functions[f];
        for (uint32_t b = 0u; b < function->block_count; ++b) {
            const XrValidatedBlock *block = &function->blocks[b];
            for (uint32_t i = 0u; i < block->instruction_count; ++i)
                if (block->instructions[i].operation_id == XR_CORE_OP_CORE_ASSERT_CONDITION &&
                    (block->instructions[i].immediate.u32 & XR_CORE_ASSERT_MESSAGE_PRESENT))
                    return true;
        }
    }
    return false;
}

#include "xr_backend_ir_emit_class.inc.c"

static const char *outcome_field(uint16_t type_id) {
    switch (type_id) {
        case XR_CORE_TYPE_F64:
            return "f64";
        case XR_CORE_TYPE_I8:
            return "i8";
        case XR_CORE_TYPE_U8:
            return "u8";
        case XR_CORE_TYPE_I16:
            return "i16";
        case XR_CORE_TYPE_I32:
            return "i32";
        case XR_CORE_TYPE_U64:
            return "u64";

        case XR_CORE_TYPE_BOOL:
            return "boolean";
        case XR_CORE_TYPE_I64:
            return "i64";
        case XR_CORE_TYPE_U32:
            return "u32";
        case XR_CORE_TYPE_PANIC_INFO:
            return "panic_info";
        case XR_CORE_TYPE_U16:
        case XR_CORE_TYPE_TARGET_OS:
        case XR_CORE_TYPE_TARGET_ARCH:
        case XR_CORE_TYPE_TARGET_ABI:
        case XR_CORE_TYPE_TARGET_ENDIAN:
            return "u16";
        case XR_CORE_TYPE_ERROR:
            return "error";
        case XR_CORE_TYPE_STRING_BUILDER:
        case XR_CORE_TYPE_STRING:
            return "pointer";
        case XR_CORE_TYPE_RUNE:
            return "u32";
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

static bool has_class_reference_types(const XrBackendIR *ir) {
    for (uint32_t index = 0u; ir && index < ir->program->type_count; ++index)
        if (xr_program_type_kind_is_reference_record(ir->program->types[index].kind))
            return true;
    return false;
}

static uint32_t module_slot_count(const XrBackendIR *ir) {
    uint32_t count = 0u;
    for (uint32_t module = 0u; module < ir->program->module_count; ++module)
        count += ir->program->modules[module].slot_count;
    return count;
}

/* Whether any function carries a string-typed value: such programs embed the
 * shared text kernel and the arena-backed string helpers. */
static bool has_string_values(const XrBackendIR *ir) {
    for (uint32_t module = 0u; module < ir->program->module_count; ++module)
        for (uint32_t slot = 0u; slot < ir->program->modules[module].slot_count; ++slot)
            if (ir->program->modules[module].slots[slot].type_id == XR_CORE_TYPE_STRING)
                return true;
    for (uint32_t index = 0u; index < ir->program->type_count; ++index) {
        const XrValidatedType *type = &ir->program->types[index];
        if (type->kind == XR_CORE_IR_TYPE_ARRAY && type->array_element_type == XR_CORE_TYPE_STRING)
            return true;
        for (uint32_t field = 0u; field < type->field_count; ++field)
            if (type->field_types[field] == XR_CORE_TYPE_STRING)
                return true;
        for (uint32_t variant = 0u; variant < type->variant_count; ++variant)
            for (uint32_t field = 0u; field < type->variants[variant].payload_count; ++field)
                if (type->variants[variant].payload_types[field] == XR_CORE_TYPE_STRING)
                    return true;
    }
    for (uint32_t function = 0; ir && function < ir->program->function_count; ++function) {
        const XrValidatedFunction *fn = &ir->program->functions[function];
        for (uint32_t value = 0; value < fn->value_count; ++value)
            if (fn->value_types[value] == XR_CORE_TYPE_STRING)
                return true;
        for (uint32_t parameter = 0; parameter < fn->parameter_count; ++parameter)
            if (fn->parameter_types[parameter] == XR_CORE_TYPE_STRING)
                return true;
        if (fn->result_type_id == XR_CORE_TYPE_STRING)
            return true;
    }
    return false;
}

static bool has_string_builder_values(const XrBackendIR *ir) {
    for (uint32_t module = 0u; module < ir->program->module_count; ++module)
        for (uint32_t slot = 0u; slot < ir->program->modules[module].slot_count; ++slot)
            if (ir->program->modules[module].slots[slot].type_id == XR_CORE_TYPE_STRING_BUILDER)
                return true;
    for (uint32_t index = 0u; index < ir->program->type_count; ++index) {
        const XrValidatedType *type = &ir->program->types[index];
        if (type->kind == XR_CORE_IR_TYPE_ARRAY && type->array_element_type == XR_CORE_TYPE_STRING_BUILDER)
            return true;
        for (uint32_t field = 0u; field < type->field_count; ++field)
            if (type->field_types[field] == XR_CORE_TYPE_STRING_BUILDER)
                return true;
        for (uint32_t variant = 0u; variant < type->variant_count; ++variant)
            for (uint32_t field = 0u; field < type->variants[variant].payload_count; ++field)
                if (type->variants[variant].payload_types[field] == XR_CORE_TYPE_STRING_BUILDER)
                    return true;
    }
    for (uint32_t function = 0; ir && function < ir->program->function_count; ++function) {
        const XrValidatedFunction *fn = &ir->program->functions[function];
        for (uint32_t value = 0; value < fn->value_count; ++value)
            if (fn->value_types[value] == XR_CORE_TYPE_STRING_BUILDER)
                return true;
        for (uint32_t parameter = 0; parameter < fn->parameter_count; ++parameter)
            if (fn->parameter_types[parameter] == XR_CORE_TYPE_STRING_BUILDER)
                return true;
        if (fn->result_type_id == XR_CORE_TYPE_STRING_BUILDER)
            return true;
    }
    return false;
}

/* Whether an instruction releases a string owner through the arena free
 * helper; the emitter and the helper gate ask this one predicate. */
static bool instruction_drops_string_owner(const XrValidatedFunction *function,
                                           const XrValidatedInstruction *instruction) {
    return instruction->operation_id == XR_CORE_OP_CORE_OWNER_DROP &&
           function->value_types[instruction->operands[0]] == XR_CORE_TYPE_STRING;
}

static void scan_string_helpers(const XrBackendIR *ir, bool *bytes, bool *integer, bool *concat) {
    *bytes = *integer = *concat = false;
    bool string_values = has_string_values(ir);
    for (uint32_t f = 0u; f < ir->program->function_count; ++f) {
        const XrValidatedFunction *function = &ir->program->functions[f];
        for (uint32_t b = 0u; b < function->block_count; ++b) {
            const XrValidatedBlock *block = &function->blocks[b];
            for (uint32_t i = 0u; i < block->instruction_count; ++i) {
                const XrValidatedInstruction *op = &block->instructions[i];
                /* Copies may reach strings through nested values or captures;
                 * the inline byte helper is also available to typed helpers. */
                *bytes |= op->operation_id == XR_CORE_OP_CORE_STRING_BUILDER_SNAPSHOT ||
                          op->operation_id == XR_CORE_OP_CORE_STRING_SLICE ||
                          op->operation_id == XR_CORE_OP_CORE_CONSTANT_STRING ||
                          (op->operation_id == XR_CORE_OP_CORE_OWNER_COPY && string_values) ||
                          (op->operation_id == XR_CORE_OP_CORE_ASSERT_CONDITION &&
                           (op->immediate.u32 & XR_CORE_ASSERT_MESSAGE_PRESENT));
                *integer |= op->operation_id == XR_CORE_OP_CORE_STRING_FROM_SCALAR;
                *concat |= op->operation_id == XR_CORE_OP_CORE_STRING_CONCAT;
            }
        }
    }
}

static void scan_helpers(const XrBackendIR *ir, bool *checked, bool *wrapping, bool *arena,
                         bool *output) {
    *checked = false;
    *wrapping = false;
    *arena = has_class_reference_types(ir) || has_string_values(ir) || has_string_builder_values(ir);
    for (uint32_t index = 0u; index < ir->program->type_count; ++index)
        *arena |= ir->program->types[index].kind == XR_CORE_IR_TYPE_ARRAY ||
                  ir->program->types[index].kind == XR_CORE_IR_TYPE_ATOMIC ||
                  ir->program->types[index].kind == XR_CORE_IR_TYPE_CHANNEL;
    for (uint32_t module = 0u; module < ir->program->module_count; ++module)
        for (uint32_t slot = 0u; slot < ir->program->modules[module].slot_count; ++slot)
            *arena |= type_needs_owned_drop(ir, ir->program->modules[module].slots[slot].type_id);
    *output = false;
    for (uint32_t function = 0; function < ir->program->function_count; ++function) {
        const XrValidatedFunction *fn = &ir->program->functions[function];
        for (uint32_t block = 0; block < fn->block_count; ++block) {
            const XrValidatedBlock *row = &fn->blocks[block];
            for (uint32_t instruction = 0; instruction < row->instruction_count; ++instruction) {
                const XrValidatedInstruction *op = &row->instructions[instruction];
                if (op->operation_id == XR_CORE_OP_CORE_ADD_I64 ||
                    op->operation_id == XR_CORE_OP_CORE_SUB_I64 ||
                    op->operation_id == XR_CORE_OP_CORE_MUL_I64) {
                    if (op->immediate.u32 == 0u)
                        *checked = true;
                    else
                        *wrapping = true;
                }
                if (op->operation_id == XR_CORE_OP_CORE_EXISTENTIAL_PACK ||
                    (op->operation_id == XR_CORE_OP_CORE_CALLABLE_PACK && op->operand_count != 0u))
                    *arena = true;
                if (op->operation_id == XR_CORE_OP_CORE_OUTPUT_GROUP)
                    *output = true;
            }
        }
    }
}

#include "xr_backend_ir_provider.inc.c"
#include "xr_backend_ir_emit_module.inc.c"
#include "xr_backend_ir_emit_format.inc.c"

static bool has_timer_suspension(const XrBackendIR *ir) {
    for (uint32_t function = 0u; ir && function < ir->program->function_count; ++function)
        for (uint32_t block = 0u; block < ir->program->functions[function].block_count; ++block)
            for (uint32_t instruction = 0u;
                 instruction < ir->program->functions[function].blocks[block].instruction_count;
                 ++instruction)
                if (ir->program->functions[function]
                        .blocks[block]
                        .instructions[instruction]
                        .operation_id == XR_CORE_OP_CORE_COROUTINE_SUSPEND)
                    return true;
    return false;
}

static bool has_direct_suspension(const XrBackendIR *ir) {
    for (uint32_t function = 0u; ir && function < ir->program->function_count; ++function)
        for (uint32_t block = 0u; block < ir->program->functions[function].block_count; ++block)
            for (uint32_t instruction = 0u;
                 instruction < ir->program->functions[function].blocks[block].instruction_count;
                 ++instruction) {
                uint16_t operation = ir->program->functions[function]
                                         .blocks[block]
                                         .instructions[instruction]
                                         .operation_id;
                if (operation == XR_CORE_OP_CORE_COROUTINE_YIELD ||
                    operation == XR_CORE_OP_CORE_COROUTINE_SUSPEND)
                    return true;
            }
    return false;
}

static bool program_uses_operation(const XrBackendIR *ir, uint16_t operation) {
    for (uint32_t function = 0u; function < ir->program->function_count; ++function) {
        const XrValidatedFunction *fn = &ir->program->functions[function];
        for (uint32_t block = 0u; block < fn->block_count; ++block)
            for (uint32_t instruction = 0u; instruction < fn->blocks[block].instruction_count;
                 ++instruction)
                if (fn->blocks[block].instructions[instruction].operation_id == operation)
                    return true;
    }
    return false;
}

static bool emit_string_builder_runtime(CBuffer *buffer, bool snapshot);

static bool emit_prelude(CBuffer *buffer, const XrBackendIR *ir, bool standalone_main) {
    bool checked = false;
    bool wrapping = false;
    bool arena = false;
    bool output = false;
    scan_helpers(ir, &checked, &wrapping, &arena, &output);
    bool classes = has_class_reference_types(ir);
    bool builders = has_string_builder_values(ir);
    bool owner_drops = has_affine_owner_drops(ir) || has_panic_messages(ir) || builders;
    bool host_providers =
        standalone_main && xr_validated_program_provider_requirement_count(ir->program) != 0u;
    bool text = builders || has_string_values(ir) || output || (standalone_main && has_boundary_error(ir));
    bool f64_constants = program_uses_operation(ir, XR_CORE_OP_CORE_SCALAR_BITCAST64);
    for (uint32_t index = 0u; index < ir->program->constant_count; ++index)
        f64_constants |= ir->program->constants[index].kind == XR_CORE_IR_CONSTANT_F64;
    bool strings = has_string_values(ir);
    bool arena_allocations = !arena || classes || strings || builders;
    for (uint32_t index = 0u; index < ir->program->type_count; ++index)
        arena_allocations |= ir->program->types[index].kind == XR_CORE_IR_TYPE_ARRAY ||
                             ir->program->types[index].kind == XR_CORE_IR_TYPE_CALLABLE ||
                             ir->program->types[index].kind == XR_CORE_IR_TYPE_EXISTENTIAL;

    bool string_bytes, string_integer, string_concat;
    scan_string_helpers(ir, &string_bytes, &string_integer, &string_concat);
    bool host_timer = standalone_main && has_timer_suspension(ir);
    if (((host_providers || host_timer) &&
         !append_text(buffer, "#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)\n"
                              "#define _POSIX_C_SOURCE 200809L\n"
                              "#endif\n")) ||
        !append_text(buffer, "#include <stdint.h>\n"
                             "#include <limits.h>\n"
                             "#include <stddef.h>\n") ||
        (f64_constants && !append_text(buffer, "#include <float.h>\n"
            "_Static_assert(sizeof(double) == 8 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024, \"binary64 required\");\n")) ||
        /* typed output renders through a heap line buffer */
        ((arena || output) && !append_text(buffer, "#include <stdlib.h>\n")) ||
        ((text || f64_constants || program_uses_operation(ir, XR_CORE_OP_CORE_PROVIDER_CALL)) &&
         !append_text(buffer, "#include <string.h>\n")) ||
        ((host_providers || host_timer) && !append_text(buffer, "#include <time.h>\n"
                                                                "#if defined(_WIN32)\n"
                                                                "#ifndef WIN32_LEAN_AND_MEAN\n"
                                                                "#define WIN32_LEAN_AND_MEAN\n"
                                                                "#endif\n"
                                                                "#include <windows.h>\n"
                                                                "#else\n"
                                                                "#include <errno.h>\n"
                                                                "#include <fcntl.h>\n"
                                                                "#include <unistd.h>\n"
                                                                "#endif\n")) ||
        ((output || has_boundary_error(ir) || has_boundary_panic(ir)) && standalone_main &&
         !append_text(buffer, "#include <stdio.h>\n"
                              "#if defined(_WIN32)\n"
                              "#include <fcntl.h>\n"
                              "#include <io.h>\n"
                              "#endif\n")) ||
        (host_providers && !emit_native_provider_headers(buffer, ir)) || !append_text(buffer, "\n"))
        return false;
    bool atomics = false, channels = false;
    for (uint32_t index = 0u; index < ir->program->type_count; ++index) {
        atomics |= ir->program->types[index].kind == XR_CORE_IR_TYPE_ATOMIC;
        channels |= ir->program->types[index].kind == XR_CORE_IR_TYPE_CHANNEL;
    }
    if (channels && (!append_text(buffer, (const char *) xr_atomic_compat_source) ||
                     !append_text(buffer, (const char *) xr_channel_buffer_source) ||
                     !append_text(buffer, (const char *) xr_channel_storage_source)))
        return false;
    if (atomics && (!append_text(buffer, (const char *) xr_atomic_compat_source) ||
                    !append_text(buffer, (const char *) xr_semantic_owner_ids_gen_source) ||
                    !append_text(buffer, (const char *) xr_sync_core_source)))
        return false;
    if (classes && needs_failure_storage(ir) && !append_text(buffer, "#include <stdatomic.h>\n"))
        return false;
    if (!append_text(buffer, (const char *) xstable_id_source) ||
        !append_text(buffer, (const char *) xr_provider_value_source))
        return false;
    if (!append_text(buffer, "typedef struct XrAotModules XrAotModules;\n"
                             "typedef struct XrAotFailure XrAotFailure;\n"))
        return false;
    if (!append_text(
            buffer, "typedef struct XrAotPanicInfo { uint32_t code; uint32_t has_bounds; int64_t index; uint64_t length; struct XrAotString *message; } XrAotPanicInfo;\n"
                    "typedef int (*XrAotProviderOutputWrite)(void *context, uint32_t "
                    "requirement, uint32_t operation, const uint8_t *bytes, size_t size);\n"
                    "typedef int (*XrAotProviderCallTyped)(void *, uint32_t, uint32_t, const XrProviderValuePack *, XrProviderValuePack *);\n"
                    "typedef void (*XrAotProviderDisposeTyped)(XrProviderValuePack *);\n"
                    "typedef void (*XrAotResourceFree)(XrExecutionResource **);\n\n"))
        return false;
    /* The shared text kernel is the same source every executor compiles; it
     * is embedded verbatim so generated C stays self-contained. */
    if (text &&
        (!append_text(buffer, (const char *) xr_float_format_source) ||
         !append_text(buffer, (const char *) xr_text_kernel_source) || !append_text(buffer, "\n")))
        return false;
    if (program_uses_operation(ir, XR_CORE_OP_CORE_BYTES_TIMING_SAFE_EQUAL) &&
        !append_text(buffer, (const char *) xr_byte_compare_core_source)) return false;
    if (builders && (!append_text(buffer, (const char *) xr_buffer_capacity_core_source) ||
                     !append_text(buffer, (const char *) xr_string_builder_storage_source)))
        return false;
    if (program_uses_operation(ir, XR_CORE_OP_CORE_ARRAY_APPEND) &&
        ((!builders && !append_text(buffer, (const char *) xr_buffer_capacity_core_source)) ||
         !append_text(buffer, (const char *) xr_array_append_kernel_source)))
        return false;
    if (program_uses_operation(ir, XR_CORE_OP_CORE_ARRAY_ALLOCATE_DEFAULT) &&
        !append_text(buffer, (const char *) xr_array_allocation_kernel_source))
        return false;
    bool integer_division = program_uses_operation(ir, XR_CORE_OP_CORE_INTEGER_DIVMOD);
    bool integer_bitwise = program_uses_operation(ir, XR_CORE_OP_CORE_INTEGER_BITWISE);
    if ((integer_bitwise || integer_division || program_uses_operation(ir, XR_CORE_OP_CORE_INTEGER_CONVERT)) &&
        (!append_text(buffer, (const char *) xr_integer_kernel_source) ||
         !append_text(buffer, "\n")))
        return false;
    if (integer_bitwise && !append_text(buffer, (const char *) xr_integer_bitwise_kernel_source))
        return false;
    if (integer_division &&
        (!append_text(buffer, (const char *) xr_integer_division_kernel_source) ||
         !append_text(buffer, "\n")))
        return false;
    if (arena) {
        if ((classes && !emit_class_lifecycle_abi(buffer)) ||
            !emit_allocation_profile_abi(buffer, ir) ||
            !append_text(buffer, "typedef union XrAotAllocation XrAotAllocation;\n"
                                 "union XrAotAllocation {\n"
                                 "    struct { XrAotAllocation *next; XrAotAllocation *previous; "
                                 "struct XrAotContext *owner; size_t size; } link;\n"
                                 "    uint8_t align_u8;\n"
                                 "    uint16_t align_u16;\n"
                                 "    uint32_t align_u32;\n"
                                 "    int64_t align_i64;\n"
                                 "    void *align_pointer;\n"
                                 "};\n"
                                 "typedef struct XrAotContext {\n"
                                 "    XrAotModules *modules;\n"
                                 "    XrAotFailure *failure;\n"
                                 "    XrAotAllocation *allocations;\n"
                                 "    void *provider_context;\n"
                                 "    uint32_t initialized_modules;\n"
                                 "    uint32_t initialization_failed;\n"
                                 "    uint32_t initialization_failure_trap;\n"
                                 "    uint16_t initialization_error_type;\n"
                                 "    const void *initialization_error_value;\n"
                                 "    uint32_t initialization_panic_present;\n"
                                 "    XrAotPanicInfo initialization_panic_info;\n"
                                 "    XrAotProviderOutputWrite provider_output_write;\n"
                                 "    XrAotProviderCallTyped provider_call_typed;\n"
                                 "    XrAotProviderDisposeTyped provider_dispose_typed;\n"
                                 "    XrAotResourceFree resource_free;\n") ||
            (classes && !append_text(buffer, "    void *lifecycle_context;\n"
                                             "    XrAotLifecycleEventHandler lifecycle_event;\n"
                                             "    uint64_t next_class_identity;\n")) ||
            !append_text(buffer, "} XrAotContext;\n\n") ||
            (arena_allocations && !append_text(buffer,
                         "static inline void *xr_aot_alloc(XrAotContext *context, size_t size) "
                         "{\n"
                         "    if (!context || size > SIZE_MAX - sizeof(XrAotAllocation)) "
                         "return NULL;\n"
                         "    XrAotAllocation *allocation = "
                         "(XrAotAllocation *)malloc(sizeof(XrAotAllocation) + size);\n"
                         "    if (!allocation) return NULL;\n"
                         "    allocation->link.owner = context;\n"
                         "    allocation->link.size = size;\n"
                         "    allocation->link.previous = NULL;\n"
                         "    allocation->link.next = context->allocations;\n"
                         "    if (context->allocations) "
                         "context->allocations->link.previous = allocation;\n"
                         "    context->allocations = allocation;\n"
                         "    return (void *)(allocation + 1);\n"
                         "}\n")) ||
            /* Typed drops reclaim owned payloads before arena teardown.
             * Emitting the
             * helper without a caller
             * fails -Werror=unused-function on toolchains
             * that warn for unused static inline functions in the main translation unit. */
            (owner_drops && arena_allocations &&
             !append_text(buffer,
                          "static inline void xr_aot_free(XrAotContext *context, void *payload) "
                          "{\n"
                          "    if (!context || !payload) return;\n"
                          "    XrAotAllocation *allocation = ((XrAotAllocation *)payload) - 1;\n"
                          "    context = allocation->link.owner;\n"
                          "    if (allocation->link.previous) "
                          "allocation->link.previous->link.next = allocation->link.next;\n"
                          "    else context->allocations = allocation->link.next;\n"
                          "    if (allocation->link.next) "
                          "allocation->link.next->link.previous = allocation->link.previous;\n"
                          "    free(allocation);\n"
                          "}\n")) ||
            (classes &&
             !append_text(buffer,
                          "static inline void xr_aot_lifecycle_emit(\n"
                          "    XrAotContext *context, XrAotLifecycleEvent event) {\n"
                          "    if (context && context->lifecycle_event)\n"
                          "        context->lifecycle_event(context->lifecycle_context, &event);\n"
                          "}\n")) ||
            !append_text(buffer, "static inline void xr_aot_context_destroy(XrAotContext *context) "
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
                                    "    XrAotModules *modules;\n"
                                 "    XrAotFailure *failure;\n"
                                    "    void *provider_context;\n"
                                    "    uint32_t initialized_modules;\n"
                                    "    uint32_t initialization_failed;\n"
                                 "    uint32_t initialization_failure_trap;\n"
                                 "    uint16_t initialization_error_type;\n"
                                 "    const void *initialization_error_value;\n"
                                    "    uint32_t initialization_panic_present;\n"
                                    "    XrAotPanicInfo initialization_panic_info;\n"
                                    "    XrAotProviderOutputWrite provider_output_write;\n"
                                 "    XrAotProviderCallTyped provider_call_typed;\n"
                                 "    XrAotProviderDisposeTyped provider_dispose_typed;\n"
                                 "    XrAotResourceFree resource_free;\n"
                                    "} XrAotContext;\n\n")) {
        return false;
    }
    if (strings && !append_text(buffer, "typedef struct XrAotString {\n"
                                        "    size_t size;\n"
                                        "    size_t scalar_count;\n"
                                        "    uint8_t bytes[1];\n"
                                        "} XrAotString;\n\n"))
        return false;
    if ((string_bytes || string_integer || string_concat) &&
        !append_text(buffer,
                     "static XrAotString *xr_aot_string_new(XrAotContext *context, size_t size) "
                     "{\n"
                     "    XrAotString *string;\n"
                     "    if (size > SIZE_MAX - offsetof(XrAotString, bytes) ||\n"
                     "        (uint64_t)size > (uint64_t)INT64_MAX) return NULL;\n"
                     "    string = (XrAotString *)xr_aot_alloc(context, "
                     "offsetof(XrAotString, bytes) + size);\n"
                     "    if (!string) return NULL;\n"
                     "    string->size = size;\n"
                     "    return string;\n"
                     "}\n\n"))
        return false;
    if (string_bytes &&
        !append_text(buffer,
                     "static inline XrAotString *xr_aot_string_from_bytes(XrAotContext *context, "
                     "const uint8_t *bytes, size_t size) {\n"
                     "    XrAotString *string = xr_aot_string_new(context, size);\n"
                     "    if (!string) return NULL;\n"
                     "    if (size != 0) memcpy(string->bytes, bytes, size);\n"
                     "    string->scalar_count = xr_text_scalar_count(bytes, size);\n"
                     "    return string;\n"
                     "}\n\n"))
        return false;
    if (program_uses_operation(ir, XR_CORE_OP_CORE_ARRAY_APPEND) && !append_text(buffer,
        "static void *xr_aot_array_allocate(void *context, size_t size) {\n"
        "    return xr_aot_alloc((XrAotContext *)context, size);\n}\n"
        "static void xr_aot_array_release(void *context, void *pointer) {\n"
        "    xr_aot_free((XrAotContext *)context, pointer);\n}\n"))
        return false;
    if (builders && !emit_string_builder_runtime(buffer,
            program_uses_operation(ir, XR_CORE_OP_CORE_STRING_BUILDER_SNAPSHOT)))
        return false;
    if (string_integer &&
        !append_text(buffer, "static XrAotString *xr_aot_string_from_scalar(XrAotContext *context, "
                             "XrTextDisplayOperand value) {\n"
                             "    XrAotString *string = xr_aot_string_new(context, "
                             "xr_text_display_operand(&value, NULL));\n"
                             "    if (!string) return NULL;\n"
                             "    (void)xr_text_display_operand(&value, string->bytes);\n"
                             "    string->scalar_count = xr_text_scalar_count(string->bytes, string->size);\n"
                             "    return string;\n"
                             "}\n\n"))
        return false;
    if (string_concat &&
        !append_text(buffer,
                     "static XrAotString *xr_aot_string_concat(XrAotContext *context, "
                     "const XrAotString *left, const XrAotString *right) {\n"
                     "    int ok = 0;\n"
                     "    size_t size = xr_text_concat_size(left->size, right->size, &ok);\n"
                     "    XrAotString *string = ok ? xr_aot_string_new(context, size) : NULL;\n"
                     "    if (!string) return NULL;\n"
                     "    xr_text_concat(left->bytes, left->size, right->bytes, right->size, "
                     "string->bytes);\n"
                     "    string->scalar_count = left->scalar_count + right->scalar_count;\n"
                     "    return string;\n"
                     "}\n\n"))
        return false;
    if (!append_text(buffer, "typedef struct XrAotOutcome {\n"
                             "    uint32_t kind;\n"
                             "    uint32_t value_kind;\n"
                             "    uint32_t trap;\n"
                             "    uint32_t error;\n"
                             "    uint16_t error_type_id;\n"
                             "    const void *error_value;\n"
                             "    uint32_t panic_present;\n"
                             "    XrAotPanicInfo panic_info;\n"
                             "    uint8_t boolean;\n"
                             "    int64_t i64;\n"
                             "    uint32_t u32;\n"
                             "    uint16_t u16;\n"
                             "    int8_t i8;\n"
                             "    uint8_t u8;\n"
                             "    int16_t i16;\n"
                             "    int32_t i32;\n"
                             "    uint64_t u64;\n"
                             "    double f64;\n"
                             "    void *pointer;\n"
                             "    uint32_t safepoint_id;\n"
                             "    uint32_t suspension_kind;\n"
                             "    uint32_t suspension_operand_count;\n"
                             "    int64_t suspension_timer_after_ms;\n"
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
    if (has_direct_suspension(ir) &&
        !append_text(buffer, "static XrAotOutcome xr_aot_suspend(uint32_t safepoint_id, "
                             "uint32_t request_kind, uint32_t operand_count, int64_t timer_ms) {\n"
                             "    XrAotOutcome result = xr_aot_make(UINT32_C(5), 0, 0);\n"
                             "    result.safepoint_id = safepoint_id;\n"
                             "    result.suspension_kind = request_kind;\n"
                             "    result.suspension_operand_count = operand_count;\n"
                             "    result.suspension_timer_after_ms = timer_ms;\n"
                             "    return result;\n"
                             "}\n\n"))
        return false;
    if (standalone_main && has_timer_suspension(ir) &&
        !append_text(buffer, "static void xr_aot_host_wait_timer(int64_t milliseconds) {\n"
                             "    if (milliseconds <= 0) return;\n"
                             "#if defined(_WIN32)\n"
                             "    Sleep((DWORD)milliseconds);\n"
                             "#else\n"
                             "    struct timespec delay;\n"
                             "    delay.tv_sec = (time_t)(milliseconds / INT64_C(1000));\n"
                             "    delay.tv_nsec = (long)((milliseconds % INT64_C(1000)) * "
                             "INT64_C(1000000));\n"
                             "    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}\n"
                             "#endif\n"
                             "}\n\n"))
        return false;
    if (wrapping &&
        !append_text(buffer, "static int64_t xr_aot_i64_from_bits(uint64_t bits) {\n"
                             "    if (bits <= (uint64_t)INT64_MAX) return (int64_t)bits;\n"
                             "    return -(int64_t)(~bits) - INT64_C(1);\n"
                             "}\n\n"))
        return false;
    if (output && standalone_main &&
        !append_text(buffer,
                     "static int xr_aot_host_output_write(void *context, uint32_t requirement, "
                     "uint32_t operation, const uint8_t *bytes, size_t size) {\n"
                     "    (void)context;\n"
                     "    (void)requirement;\n"
                     "    (void)operation;\n"
                     "#if defined(_WIN32)\n"
                     "    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 1;\n"
                     "#endif\n"
                     "    if ((!bytes && size != 0) || fwrite(bytes, 1, size, stdout) != size) "
                     "return 1;\n"
                     "    return 0;\n"
                     "}\n\n"))
        return false;
    if (host_providers && !emit_native_provider_definitions(buffer, ir))
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

static const XrValidatedInstruction *
coroutine_suspension_instruction(const XrValidatedFunction *function, uint32_t safepoint_id,
                                 uint32_t *block_id_out) {
    const XrValidatedInstruction *found = NULL;
    for (uint32_t block = 0u; function && block < function->block_count; ++block) {
        const XrValidatedBlock *row = &function->blocks[block];
        for (uint32_t instruction = 0u; instruction < row->instruction_count; ++instruction) {
            const XrValidatedInstruction *candidate = &row->instructions[instruction];
            bool matches = (candidate->operation_id == XR_CORE_OP_CORE_COROUTINE_YIELD &&
                            candidate->immediate.u32 == safepoint_id) ||
                           (candidate->operation_id == XR_CORE_OP_CORE_COROUTINE_SUSPEND &&
                            candidate->immediate.coroutine_suspend.safepoint_id == safepoint_id) ||
                           (candidate->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED &&
                            candidate->immediate.coroutine_call.safepoint_id == safepoint_id) ||
                           (candidate->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT &&
                            candidate->immediate.u32 == safepoint_id);
            if (!matches)
                continue;
            if (found)
                return NULL;
            found = candidate;
            if (block_id_out)
                *block_id_out = block;
        }
    }
    return found;
}

/* Safepoint rows are canonical value-id sets.  Frame slots instead follow the
 * suspension
 * instruction's live-operand tuple because that tuple is positionally
 * paired with the resume
 * block arguments. */
static bool coroutine_live_operand_start(const XrBackendIR *ir, const XrValidatedFunction *function,
                                         const XrValidatedInstruction *instruction,
                                         const XrValidatedCoroutineSafepoint *point,
                                         uint32_t *start_out) {
    if (!ir || !instruction || !point || !start_out)
        return false;
    uint32_t start = 0u;
    if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED) {
        uint32_t callee = instruction->immediate.coroutine_call.function_id;
        if (callee >= ir->program->function_count)
            return false;
        start = ir->program->functions[callee].parameter_count;
    } else if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT) {
        if (instruction->operand_count == 0u || instruction->operands[0] >= function->value_count)
            return false;
        const XrValidatedType *callable =
            xr_validated_program_type(ir->program, function->value_types[instruction->operands[0]]);
        if (!callable || callable->kind != XR_CORE_IR_TYPE_CALLABLE ||
            callable->signature_id >= ir->program->signature_count)
            return false;
        start = ir->program->signatures[callable->signature_id].parameter_count + 1u;
    } else if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_SUSPEND) {
        start = instruction->immediate.coroutine_suspend.request_operand_count;
    } else if (instruction->operation_id != XR_CORE_OP_CORE_COROUTINE_YIELD) {
        return false;
    }
    if (start > instruction->operand_count ||
        point->live_value_count > instruction->operand_count - start)
        return false;
    *start_out = start;
    return true;
}

static bool callable_type_can_target(const XrBackendIR *ir, uint16_t callable_type,
                                     uint32_t target_function);

static bool emit_coroutine_frame_definition(CBuffer *buffer, const XrBackendIR *ir,
                                            uint32_t function_id, uint8_t *state) {
    if (state[function_id] == 2u)
        return true;
    if (state[function_id] == 1u)
        return false;
    state[function_id] = 1u;
    const XrValidatedFunction *function = &ir->program->functions[function_id];
    for (uint32_t safepoint = 0u; safepoint < function->coroutine_safepoint_count; ++safepoint) {
        const XrValidatedInstruction *instruction =
            coroutine_suspension_instruction(function, safepoint, NULL);
        if (!instruction)
            return false;
        if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED) {
            if (!emit_coroutine_frame_definition(
                    buffer, ir, instruction->immediate.coroutine_call.function_id, state))
                return false;
        } else if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT) {
            uint16_t callable_type = function->value_types[instruction->operands[0]];
            for (uint32_t target = 0u; target < ir->program->function_count; ++target)
                if (callable_type_can_target(ir, callable_type, target) &&
                    ir->program->functions[target].coroutine_safepoint_count != 0u &&
                    !emit_coroutine_frame_definition(buffer, ir, target, state))
                    return false;
        }
    }
    if (!append_format(buffer, "struct XrAotCoroutineFrame%u {\n    uint32_t state;\n",
                       function_id))
        return false;
    for (uint32_t parameter = 0u; parameter < function->parameter_count; ++parameter) {
        char storage[32];
        const char *type = type_c_name(function->parameter_types[parameter], storage);
        const char *pointer = function->parameter_modes[parameter] == XR_PARAM_REF &&
                                      !parameter_is_class_receiver(ir, function, parameter)
                                  ? " *"
                                  : "";
        if (!type || !append_format(buffer, "    %s%s parameter_%u;\n", type, pointer, parameter))
            return false;
    }
    for (uint32_t safepoint = 0u; safepoint < function->coroutine_safepoint_count; ++safepoint) {
        const XrValidatedCoroutineSafepoint *point = &function->coroutine_safepoints[safepoint];
        const XrValidatedInstruction *instruction =
            coroutine_suspension_instruction(function, safepoint, NULL);
        uint32_t live_operand_start = 0u;
        if (!instruction ||
            !coroutine_live_operand_start(ir, function, instruction, point, &live_operand_start))
            return false;
        for (uint32_t live = 0u; live < point->live_value_count; ++live) {
            char storage[32];
            uint32_t value = instruction->operands[live_operand_start + live];
            const char *type = type_c_name(function->value_types[value], storage);
            const char *pointer = function->value_categories[value] == XR_CORE_IR_PLACE ? " *" : "";
            if (!type ||
                !append_format(buffer, "    %s%s live_%u_%u;\n", type, pointer, safepoint, live))
                return false;
        }
        if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED &&
            (!append_format(buffer, "    XrAotCoroutineFrame%u child_%u;\n",
                            instruction->immediate.coroutine_call.function_id, safepoint) ||
             !append_format(buffer, "    uint8_t child_active_%u;\n", safepoint)))
            return false;
        if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT) {
            uint16_t callable_type = function->value_types[instruction->operands[0]];
            if (!append_text(buffer, "    union {\n"))
                return false;
            uint32_t child_count = 0u;
            for (uint32_t target = 0u; target < ir->program->function_count; ++target) {
                if (!callable_type_can_target(ir, callable_type, target) ||
                    ir->program->functions[target].coroutine_safepoint_count == 0u)
                    continue;
                if (!append_format(buffer, "        XrAotCoroutineFrame%u target_%u;\n", target,
                                   target))
                    return false;
                ++child_count;
            }
            if (child_count == 0u || !append_format(buffer,
                                                    "    } child_%u;\n"
                                                    "    uint32_t child_function_%u;\n"
                                                    "    uint8_t child_active_%u;\n",
                                                    safepoint, safepoint, safepoint))
                return false;
        }
    }
    if (!append_text(buffer, "};\n\n"))
        return false;
    state[function_id] = 2u;
    return true;
}

static bool emit_coroutine_frames(CBuffer *buffer, const XrBackendIR *ir) {
    for (uint32_t function_id = 0u; function_id < ir->program->function_count; ++function_id) {
        if (ir->program->functions[function_id].coroutine_safepoint_count != 0u &&
            !append_format(buffer, "typedef struct XrAotCoroutineFrame%u XrAotCoroutineFrame%u;\n",
                           function_id, function_id))
            return false;
    }
    if (!append_text(buffer, "\n"))
        return false;
    uint8_t *state =
        xr_calloc(ir->program->function_count ? ir->program->function_count : 1u, sizeof(*state));
    if (!state)
        return false;
    bool emitted = true;
    for (uint32_t function_id = 0u; emitted && function_id < ir->program->function_count;
         ++function_id) {
        if (ir->program->functions[function_id].coroutine_safepoint_count != 0u)
            emitted = emit_coroutine_frame_definition(buffer, ir, function_id, state);
    }
    xr_free(state);
    return emitted;
}

static const XrValidatedInstruction *backend_value_instruction(const XrValidatedFunction *function,
                                                               uint32_t value_id) {
    const XrValidatedInstruction *found = NULL;
    for (uint32_t block = 0u; function && block < function->block_count; ++block) {
        const XrValidatedBlock *row = &function->blocks[block];
        for (uint32_t instruction = 0u; instruction < row->instruction_count; ++instruction) {
            const XrValidatedInstruction *candidate = &row->instructions[instruction];
            if (candidate->result_id != value_id)
                continue;
            if (found)
                return NULL;
            found = candidate;
        }
    }
    return found;
}

typedef struct XrCoroutineRefFrameSlot {
    bool uses_slot;
    bool class_field;
    uint32_t owner;
    uint32_t live;
    uint32_t class_field_ordinal;
    uint32_t projection_count;
} XrCoroutineRefFrameSlot;

typedef struct XrCoroutineRefContext {
    const XrBackendIR *ir;
    const XrValidatedFunction *function;
    const XrValidatedInstruction *call;
    const XrValidatedCoroutineSafepoint *point;
    uint32_t live_operand_start;
} XrCoroutineRefContext;

// A child can retain projected places while suspended. Preserve their common
// storage root once in the parent frame and rebuild each exact field address.
static bool coroutine_ref_parameter_frame_slot(const XrCoroutineRefContext *context,
                                               uint32_t parameter,
                                               XrCoroutineRefFrameSlot *slot_out) {
    if (!context || !context->ir || !context->function || !context->call || !context->point ||
        !slot_out || parameter >= context->call->operand_count)
        return false;
    const XrValidatedFunction *function = context->function;
    const XrValidatedInstruction *call = context->call;
    const XrValidatedCoroutineSafepoint *point = context->point;
    *slot_out = (XrCoroutineRefFrameSlot) {0};
    uint32_t place = call->operands[parameter];
    for (uint32_t depth = 0u; depth < function->value_count; ++depth) {
        if (place >= function->value_count ||
            function->value_categories[place] != XR_CORE_IR_PLACE ||
            function->value_ownerships[place] != XR_CORE_IR_NON_OWNER)
            return false;
        const XrValidatedInstruction *definition = backend_value_instruction(function, place);
        if (!definition)
            return depth == 0u;
        if (definition->operand_count != 1u || !definition->operands ||
            definition->operands[0] >= function->value_count ||
            definition->result_type_id != function->value_types[place])
            return false;
        uint32_t owner = definition->operands[0];
        if (definition->operation_id == XR_CORE_OP_CORE_PLACE_PROJECT) {
            const XrValidatedType *aggregate =
                xr_validated_program_type(context->ir->program, function->value_types[owner]);
            if (definition->immediate_kind != XR_CORE_IR_IMMEDIATE_FIELD || !aggregate ||
                aggregate->kind != XR_CORE_IR_TYPE_AGGREGATE ||
                definition->immediate.field_ordinal >= aggregate->field_count ||
                aggregate->field_types[definition->immediate.field_ordinal] !=
                    function->value_types[place])
                return false;
            place = definition->operands[0];
            ++slot_out->projection_count;
            continue;
        }
        uint32_t occurrence = 0u;
        for (uint32_t live = 0u; live < point->live_value_count; ++live) {
            uint32_t operand = context->live_operand_start + live;
            if (operand >= call->operand_count || call->operands[operand] != owner)
                continue;
            ++occurrence;
            slot_out->live = live;
        }
        if (occurrence != 1u || function->value_categories[owner] != XR_CORE_IR_VALUE)
            return false;
        if (definition->operation_id == XR_CORE_OP_CORE_CLASS_FIELD_PLACE) {
            const XrValidatedType *class_type =
                xr_validated_program_type(context->ir->program, function->value_types[owner]);
            if (definition->immediate_kind != XR_CORE_IR_IMMEDIATE_FIELD || !class_type ||
                !xr_program_type_kind_is_reference_record(class_type->kind) ||
                function->value_ownerships[owner] != XR_CORE_IR_OWNER ||
                definition->immediate.field_ordinal >= class_type->field_count ||
                class_type->field_types[definition->immediate.field_ordinal] !=
                    function->value_types[place])
                return false;
            slot_out->class_field = true;
            slot_out->class_field_ordinal = definition->immediate.field_ordinal;
        } else if (definition->operation_id != XR_CORE_OP_CORE_PLACE_LOCAL ||
                   definition->immediate_kind != XR_CORE_IR_IMMEDIATE_NONE ||
                   function->value_types[owner] != function->value_types[place]) {
            return false;
        }
        slot_out->uses_slot = true;
        slot_out->owner = owner;
        return true;
    }
    return false;
}

static bool emit_coroutine_ref_frame_transfers(CBuffer *buffer, const XrBackendIR *ir,
                                               const XrValidatedFunction *function,
                                               const XrValidatedFunction *callee,
                                               const XrValidatedInstruction *call, bool restore) {
    uint32_t safepoint = call->immediate.coroutine_call.safepoint_id;
    const XrValidatedCoroutineSafepoint *point = &function->coroutine_safepoints[safepoint];
    XrCoroutineRefContext context = {
        .ir = ir,
        .function = function,
        .call = call,
        .point = point,
        .live_operand_start = callee->parameter_count,
    };
    uint8_t *transferred =
        point->live_value_count ? xr_calloc(point->live_value_count, sizeof(*transferred)) : NULL;
    if (point->live_value_count != 0u && !transferred)
        return false;
    bool emitted = true;
    for (uint32_t parameter = 0u; emitted && parameter < callee->parameter_count; ++parameter) {
        if (callee->parameter_modes[parameter] != XR_PARAM_REF)
            continue;
        XrCoroutineRefFrameSlot slot;
        if (!coroutine_ref_parameter_frame_slot(&context, parameter, &slot)) {
            emitted = false;
            break;
        }
        if (!slot.uses_slot || transferred[slot.live])
            continue;
        transferred[slot.live] = 1u;
        emitted = restore ? append_format(buffer, "        v%u = frame->live_%u_%u;\n", slot.owner,
                                          safepoint, slot.live)
                          : append_format(buffer, "        frame->live_%u_%u = v%u;\n", safepoint,
                                          slot.live, slot.owner);
    }
    xr_free(transferred);
    return emitted;
}

static bool emit_coroutine_ref_place(CBuffer *buffer, const XrValidatedFunction *function,
                                     const XrCoroutineRefFrameSlot *slot, uint32_t place,
                                     uint32_t safepoint) {
    uint32_t *fields =
        slot->projection_count ? xr_calloc(slot->projection_count, sizeof(*fields)) : NULL;
    if (slot->projection_count != 0u && !fields)
        return false;
    uint32_t projected = place;
    for (uint32_t depth = 0u; depth < slot->projection_count; ++depth) {
        const XrValidatedInstruction *definition = backend_value_instruction(function, projected);
        if (!definition || definition->operation_id != XR_CORE_OP_CORE_PLACE_PROJECT ||
            definition->operand_count != 1u || !definition->operands) {
            xr_free(fields);
            return false;
        }
        fields[depth] = definition->immediate.field_ordinal;
        projected = definition->operands[0];
    }
    const XrValidatedInstruction *root = backend_value_instruction(function, projected);
    if (!root || root->operand_count != 1u || !root->operands || root->operands[0] != slot->owner ||
        (slot->class_field && (root->operation_id != XR_CORE_OP_CORE_CLASS_FIELD_PLACE ||
                               root->immediate_kind != XR_CORE_IR_IMMEDIATE_FIELD ||
                               root->immediate.field_ordinal != slot->class_field_ordinal)) ||
        (!slot->class_field && root->operation_id != XR_CORE_OP_CORE_PLACE_LOCAL)) {
        xr_free(fields);
        return false;
    }
    bool emitted =
        append_format(buffer, "        v%u = &frame->live_%u_%u", place, safepoint, slot->live);
    if (emitted && slot->class_field)
        emitted = append_format(buffer, "->f%u", slot->class_field_ordinal);
    for (uint32_t depth = slot->projection_count; emitted && depth != 0u; --depth)
        emitted = append_format(buffer, ".f%u", fields[depth - 1u]);
    xr_free(fields);
    return emitted && append_text(buffer, ";\n");
}

static bool emit_coroutine_ref_bindings(CBuffer *buffer, const XrBackendIR *ir,
                                        const XrValidatedFunction *function,
                                        const XrValidatedFunction *callee,
                                        const XrValidatedInstruction *call) {
    uint32_t safepoint = call->immediate.coroutine_call.safepoint_id;
    const XrValidatedCoroutineSafepoint *point = &function->coroutine_safepoints[safepoint];
    XrCoroutineRefContext context = {
        .ir = ir,
        .function = function,
        .call = call,
        .point = point,
        .live_operand_start = callee->parameter_count,
    };
    for (uint32_t parameter = 0u; parameter < callee->parameter_count; ++parameter) {
        if (callee->parameter_modes[parameter] != XR_PARAM_REF)
            continue;
        XrCoroutineRefFrameSlot slot;
        if (!coroutine_ref_parameter_frame_slot(&context, parameter, &slot))
            return false;
        if (slot.uses_slot && !emit_coroutine_ref_place(buffer, function, &slot,
                                                        call->operands[parameter], safepoint))
            return false;
    }
    return true;
}

static bool emit_function_signature(CBuffer *buffer, const XrValidatedFunction *function,
                                    const XrBackendIR *ir, uint32_t function_id, bool prototype) {
    const char *suffix = function->coroutine_safepoint_count != 0u ? "_step" : "";
    if (!append_format(buffer, "XrAotOutcome xr_aot_fn_%u%s(XrAotContext *xr_ctx", function_id,
                       suffix))
        return false;
    if (function->coroutine_safepoint_count != 0u &&
        !append_format(buffer, ", XrAotCoroutineFrame%u *frame, uint8_t xr_cancel", function_id))
        return false;
    bool aggregate_result =
        xr_validated_program_type(ir->program, function->result_type_id) != NULL;
    bool has_error = function->error_type_id != XR_CORE_TYPE_VOID;
    bool has_panic = function->panic_type_id != XR_CORE_TYPE_VOID;
    for (uint32_t parameter = 0; parameter < function->parameter_count; ++parameter) {
        char storage[32];
        const char *name = type_c_name(function->parameter_types[parameter], storage);
        if (!name)
            return false;
        const char *pointer = function->parameter_modes[parameter] == XR_PARAM_REF &&
                                      !parameter_is_class_receiver(ir, function, parameter)
                                  ? " *"
                                  : "";
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

static bool emit_parallel_edge(CBuffer *buffer, const XrValidatedFunction *function,
                               const XrValidatedInstruction *instruction, uint32_t successor_index,
                               uint32_t operand_start, uint32_t function_id) {
    uint32_t target_id = instruction->successors[successor_index];
    const XrValidatedBlock *target = &function->blocks[target_id];
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

static bool emit_provider_failure(CBuffer *buffer, const XrValidatedFunction *function,
                                  const XrValidatedInstruction *instruction,
                                  uint32_t provider_operand_count, uint32_t function_id) {
    if (instruction->successor_count == 1u)
        return emit_parallel_edge(buffer, function, instruction, 0u, provider_operand_count,
                                  function_id);
    return append_text(buffer, "XR_AOT_FAIL(xr_aot_make(1, 0, 7));\n");
}

static bool emit_direct_call_outcome(CBuffer *buffer, const XrValidatedFunction *function,
                                     const XrValidatedInstruction *instruction,
                                     uint32_t call_operand_count, uint32_t function_id,
                                     uint32_t instruction_id, bool has_panic) {
    if (instruction->successor_count == 1u) {
        if (!append_format(buffer, "        if (call_%u.kind == 1 && call_%u.trap == 7) ",
                           instruction_id, instruction_id) ||
            !emit_parallel_edge(buffer, function, instruction, 0u, call_operand_count, function_id))
            return false;
    }
    if (has_panic && !append_format(buffer,
                                    "        if (call_%u.kind == UINT32_C(3)) { *out_panic = "
                                    "call_panic_%u; XR_AOT_FAIL(call_%u); }\n",
                                    instruction_id, instruction_id, instruction_id))
        return false;
    return append_format(buffer, "        if (call_%u.kind != 0) XR_AOT_FAIL(call_%u);\n",
                         instruction_id, instruction_id);
}

static bool emit_return(CBuffer *buffer, const XrValidatedFunction *function,
                        const XrValidatedInstruction *instruction) {
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

static bool emit_call(CBuffer *buffer, const XrBackendIR *ir, const XrValidatedFunction *function,
                      const XrValidatedInstruction *instruction, uint32_t function_id,
                      uint32_t instruction_id) {
    uint32_t callee_id = instruction->immediate.function_id;
    const XrValidatedFunction *callee = &ir->program->functions[callee_id];
    bool has_panic = callee->panic_type_id != XR_CORE_TYPE_VOID;
    if (has_panic &&
        !append_format(buffer, "        XrAotPanicInfo call_panic_%u = {0};\n", instruction_id))
        return false;
    if (!append_format(buffer, "        XrAotOutcome call_%u = xr_aot_fn_%u(xr_ctx", instruction_id,
                       callee_id))
        return false;
    uint32_t call_operand_count = callee->parameter_count;
    for (uint32_t operand = 0; operand < call_operand_count; ++operand) {
        if (!append_format(buffer, ", v%u", instruction->operands[operand]))
            return false;
    }
    if (callee->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE &&
        !append_format(buffer, ", &v%u", instruction->result_id))
        return false;
    if (has_panic && !append_format(buffer, ", &call_panic_%u", instruction_id))
        return false;
    if (!append_text(buffer, ")"))
        return false;
    if (!append_text(buffer, ";\n"))
        return false;
    if (!emit_direct_call_outcome(buffer, function, instruction, call_operand_count, function_id,
                                  instruction_id, has_panic)) {
        return false;
    }
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
                                                     const XrValidatedFunction *function,
                                                     const XrValidatedInstruction *instruction,
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
                                    const XrValidatedInstruction *instruction,
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
            conformance->slot_function_ids[slot] >= ir->program->function_count)
            return false;
        uint32_t callee_id = conformance->slot_function_ids[slot];
        const XrValidatedFunction *callee = &ir->program->functions[callee_id];
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
        if (callee->parameter_modes[0] == XR_PARAM_REF &&
            type_is_class_reference(ir, conformance->implementor_type_id)) {
            if (!append_format(buffer, "*(const %s *)v%u.data", receiver_type, receiver_value))
                return false;
        } else if (callee->parameter_modes[0] == XR_PARAM_REF) {
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
                              const XrValidatedFunction *function,
                              const XrValidatedInstruction *instruction, uint32_t function_id,
                              uint32_t instruction_id) {
    uint32_t interface_id = 0u;
    const XrValidatedSignature *signature =
        witness_signature(ir, function, instruction, &interface_id);
    if (!signature)
        return false;
    bool has_panic = signature->panic_type_id != XR_CORE_TYPE_VOID;
    char panic_expression[64];
    (void) snprintf(panic_expression, sizeof(panic_expression), "&call_panic_%u", instruction_id);
    if (has_panic &&
        !append_format(buffer, "        XrAotPanicInfo call_panic_%u = {0};\n", instruction_id))
        return false;
    if (!append_format(buffer,
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
                                 result, NULL, has_panic ? panic_expression : NULL) ||
        !emit_direct_call_outcome(buffer, function, instruction, signature->parameter_count,
                                  function_id, instruction_id, has_panic))
        return false;
    if (instruction->result_id == XR_PROGRAM_LOCATION_NONE ||
        signature->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE)
        return true;
    const char *field = outcome_field(signature->result_type_id);
    return field && append_format(buffer, "        v%u = call_%u.%s;\n", instruction->result_id,
                                  instruction_id, field);
}

static const XrValidatedSignature *callable_signature(const XrBackendIR *ir,
                                                      const XrValidatedFunction *function,
                                                      const XrValidatedInstruction *instruction) {
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
    for (uint32_t function = 0; function < ir->program->function_count; ++function) {
        const XrValidatedFunction *row = &ir->program->functions[function];
        for (uint32_t block = 0; block < row->block_count; ++block) {
            for (uint32_t instruction = 0; instruction < row->blocks[block].instruction_count;
                 ++instruction) {
                const XrValidatedInstruction *candidate =
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

#include "xr_backend_ir_emit_copy.inc.c"

static bool emit_callable_call_cases(CBuffer *buffer, const XrBackendIR *ir,
                                     const XrValidatedFunction *caller,
                                     const XrValidatedInstruction *instruction,
                                     const XrValidatedSignature *signature, uint32_t instruction_id,
                                     const char *result_expression, const char *error_expression,
                                     const char *panic_expression) {
    uint32_t callable_value = instruction->operands[0];
    uint16_t callable_type = caller->value_types[callable_value];
    for (uint32_t target_id = 0; target_id < ir->program->function_count; ++target_id) {
        if (!callable_type_can_target(ir, callable_type, target_id))
            continue;
        const XrValidatedFunction *target = &ir->program->functions[target_id];
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
                               const XrValidatedFunction *function,
                               const XrValidatedInstruction *instruction, uint32_t function_id,
                               uint32_t instruction_id) {
    const XrValidatedSignature *signature = callable_signature(ir, function, instruction);
    if (!signature)
        return false;
    bool has_panic = signature->panic_type_id != XR_CORE_TYPE_VOID;
    char panic_expression[64];
    (void) snprintf(panic_expression, sizeof(panic_expression), "&call_panic_%u", instruction_id);
    if (has_panic &&
        !append_format(buffer, "        XrAotPanicInfo call_panic_%u = {0};\n", instruction_id))
        return false;
    if (!append_format(buffer,
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
                                  result, NULL, has_panic ? panic_expression : NULL) ||
        !emit_direct_call_outcome(buffer, function, instruction, signature->parameter_count + 1u,
                                  function_id, instruction_id, has_panic))
        return false;
    if (instruction->result_id == XR_PROGRAM_LOCATION_NONE ||
        signature->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE)
        return true;
    const char *field = outcome_field(signature->result_type_id);
    return field && append_format(buffer, "        v%u = call_%u.%s;\n", instruction->result_id,
                                  instruction_id, field);
}

static bool emit_invoke_edge(CBuffer *buffer, const XrValidatedFunction *function,
                             const XrValidatedInstruction *instruction, uint32_t successor_index,
                             uint32_t target_start, uint32_t operand_start, uint32_t function_id,
                             const char *implicit_expression) {
    const XrValidatedBlock *target = &function->blocks[instruction->successors[successor_index]];
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

static bool emit_call_failure_outputs(CBuffer *buffer, const XrValidatedSignature *signature,
                                      uint32_t id, bool declarations) {
    if (signature->error_type_id != XR_CORE_TYPE_VOID) {
        char storage[32];
        const char *type = type_c_name(signature->error_type_id, storage);
        if (!type)
            return false;
        if (declarations) {
            const char *initializer =
                signature->error_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE ? "{0}" : "0";
            if (!append_format(buffer, "        %s invoke_error_%u = %s;\n", type, id, initializer))
                return false;
        } else if (!append_format(buffer, ", &invoke_error_%u", id)) {
            return false;
        }
    }
    if (signature->panic_type_id != XR_CORE_TYPE_VOID &&
        !append_format(
            buffer, declarations ? "        XrAotPanicInfo invoke_panic_%u = {0};\n" : ", &invoke_panic_%u",
            id))
        return false;
    return true;
}

static const XrValidatedSignature *
coroutine_call_signature(const XrBackendIR *ir, const XrValidatedFunction *function,
                         const XrValidatedInstruction *instruction) {
    if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT)
        return callable_signature(ir, function, instruction);
    if (instruction->operation_id != XR_CORE_OP_CORE_COROUTINE_CALL_SEALED)
        return NULL;
    const XrValidatedFunction *callee =
        &ir->program->functions[instruction->immediate.coroutine_call.function_id];
    return &ir->program->signatures[callee->signature_id];
}

/* The calling convention selects a target, but does not define another result
 * protocol. Every call uses the same typed outcome and parallel edge transfer. */
static bool emit_call_completion(CBuffer *buffer, const XrValidatedFunction *function,
                                 const XrValidatedInstruction *instruction,
                                 const XrValidatedSignature *signature, uint32_t function_id,
                                 uint32_t instruction_id) {
    bool coroutine = instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED ||
                     instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT;
    bool indirect = instruction->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE ||
                    instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT;
    const char *outcome = coroutine ? "child" : "call";
    const XrValidatedBlock *normal = &function->blocks[instruction->successors[0]];
    uint32_t implicit = signature->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
    uint32_t operand = signature->parameter_count + (indirect ? 1u : 0u);
    char expression[64];
    const char *result = NULL;
    if (implicit != 0u) {
        if (signature->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
            if (coroutine)
                (void) snprintf(expression, sizeof(expression), "child_result_%u", instruction_id);
            else
                (void) snprintf(expression, sizeof(expression), "v%u", normal->argument_ids[0]);
        } else {
            const char *field = outcome_field(signature->result_type_id);
            if (!field)
                return false;
            (void) snprintf(expression, sizeof(expression), "%s_%u.%s", outcome, instruction_id,
                            field);
        }
        result = expression;
    }
    if (!append_format(buffer, "        if (%s_%u.kind == 0) ", outcome, instruction_id) ||
        !emit_invoke_edge(buffer, function, instruction, 0u, implicit, operand, function_id,
                          result))
        return false;
    operand += normal->argument_count - implicit;
    uint32_t successor = 1u;
    if (coroutine) {
        operand += function->blocks[instruction->successors[1]].argument_count;
        ++successor;
    }
    if (signature->error_type_id != XR_CORE_TYPE_VOID) {
        (void) snprintf(expression, sizeof(expression), "invoke_error_%u", instruction_id);
        if (!append_format(buffer, "        if (%s_%u.kind == 2) ", outcome, instruction_id) ||
            !emit_invoke_edge(buffer, function, instruction, successor, 1u, operand, function_id,
                              expression))
            return false;
        operand += function->blocks[instruction->successors[successor]].argument_count - 1u;
        ++successor;
    }
    if (signature->panic_type_id != XR_CORE_TYPE_VOID) {
        (void) snprintf(expression, sizeof(expression), "invoke_panic_%u", instruction_id);
        if (!append_format(buffer, "        if (%s_%u.kind == 3) ", outcome, instruction_id) ||
            !emit_invoke_edge(buffer, function, instruction, successor, 1u, operand, function_id,
                              expression))
            return false;
        operand += function->blocks[instruction->successors[successor]].argument_count - 1u;
        ++successor;
    }
    if (instruction->successor_count == successor + 1u &&
        (!append_format(buffer, "        if (%s_%u.kind == 1 && %s_%u.trap == 7) ", outcome,
                        instruction_id, outcome, instruction_id) ||
         !emit_invoke_edge(buffer, function, instruction, successor, 0u, operand, function_id,
                           NULL)))
        return false;
    if (coroutine && !append_text(buffer, "        frame->state = UINT32_MAX;\n"))
        return false;
    return append_format(buffer, "        XR_AOT_FAIL(%s_%u);\n", outcome, instruction_id);
}

static bool emit_invoke(CBuffer *buffer, const XrBackendIR *ir, const XrValidatedFunction *function,
                        const XrValidatedInstruction *instruction, uint32_t function_id,
                        uint32_t instruction_id) {
    const XrValidatedFunction *callee = &ir->program->functions[instruction->immediate.function_id];
    const XrValidatedBlock *normal = &function->blocks[instruction->successors[0]];
    bool has_error = callee->error_type_id != XR_CORE_TYPE_VOID;
    bool has_panic = callee->panic_type_id != XR_CORE_TYPE_VOID;
    if (!emit_call_failure_outputs(buffer, &ir->program->signatures[callee->signature_id],
                                   instruction_id, true))
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

    return emit_call_completion(buffer, function, instruction,
                                &ir->program->signatures[callee->signature_id], function_id,
                                instruction_id);
}

static bool emit_witness_invoke(CBuffer *buffer, const XrBackendIR *ir,
                                const XrValidatedFunction *function,
                                const XrValidatedInstruction *instruction, uint32_t function_id,
                                uint32_t instruction_id) {
    uint32_t interface_id = 0u;
    const XrValidatedSignature *signature =
        witness_signature(ir, function, instruction, &interface_id);
    if (!signature)
        return false;
    const XrValidatedBlock *normal = &function->blocks[instruction->successors[0]];
    bool has_error = signature->error_type_id != XR_CORE_TYPE_VOID;
    bool has_panic = signature->panic_type_id != XR_CORE_TYPE_VOID;
    if (!emit_call_failure_outputs(buffer, signature, instruction_id, true))
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

    return emit_call_completion(buffer, function, instruction, signature, function_id,
                                instruction_id);
}

static bool emit_callable_invoke(CBuffer *buffer, const XrBackendIR *ir,
                                 const XrValidatedFunction *function,
                                 const XrValidatedInstruction *instruction, uint32_t function_id,
                                 uint32_t instruction_id) {
    const XrValidatedSignature *signature = callable_signature(ir, function, instruction);
    if (!signature)
        return false;
    const XrValidatedBlock *normal = &function->blocks[instruction->successors[0]];
    bool has_error = signature->error_type_id != XR_CORE_TYPE_VOID;
    bool has_panic = signature->panic_type_id != XR_CORE_TYPE_VOID;
    if (!emit_call_failure_outputs(buffer, signature, instruction_id, true))
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

    return emit_call_completion(buffer, function, instruction, signature, function_id,
                                instruction_id);
}

static bool emit_coroutine_call(CBuffer *buffer, const XrBackendIR *ir,
                                const XrValidatedFunction *function,
                                const XrValidatedInstruction *instruction, uint32_t function_id,
                                uint32_t instruction_id) {
    uint32_t callee_id = instruction->immediate.coroutine_call.function_id;
    uint32_t safepoint_id = instruction->immediate.coroutine_call.safepoint_id;
    const XrValidatedFunction *callee = &ir->program->functions[callee_id];
    const XrValidatedSignature *signature = &ir->program->signatures[callee->signature_id];
    if (!emit_call_failure_outputs(buffer, signature, instruction_id, true))
        return false;
    const XrValidatedCoroutineSafepoint *point = &function->coroutine_safepoints[safepoint_id];
    if (!emit_coroutine_ref_frame_transfers(buffer, ir, function, callee, instruction, false) ||
        !emit_coroutine_ref_bindings(buffer, ir, function, callee, instruction))
        return false;
    if (!append_format(buffer,
                       "        if (!frame->child_active_%u) {\n"
                       "            XrAotCoroutineFrame%u child_zero_%u = {0};\n"
                       "            frame->child_%u = child_zero_%u;\n",
                       safepoint_id, callee_id, instruction_id, safepoint_id, instruction_id))
        return false;
    for (uint32_t parameter = 0u; parameter < callee->parameter_count; ++parameter) {
        if (!append_format(buffer, "            frame->child_%u.parameter_%u = v%u;\n",
                           safepoint_id, parameter, instruction->operands[parameter]))
            return false;
    }
    if (!append_format(buffer, "            frame->child_active_%u = UINT8_C(1);\n        }\n",
                       safepoint_id))
        return false;
    if (callee->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
        char storage[32];
        const char *type = type_c_name(callee->result_type_id, storage);
        if (!type ||
            !append_format(buffer, "        %s child_result_%u = {0};\n", type, instruction_id))
            return false;
    }
    if (!append_format(buffer,
                       "        XrAotOutcome child_%u = xr_aot_fn_%u_step(xr_ctx, "
                       "&frame->child_%u, UINT8_C(0)",
                       instruction_id, callee_id, safepoint_id))
        return false;
    for (uint32_t parameter = 0u; parameter < callee->parameter_count; ++parameter) {
        if (!append_format(buffer, ", frame->child_%u.parameter_%u", safepoint_id, parameter))
            return false;
    }
    if (callee->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
        if (!append_format(buffer, ", &child_result_%u", instruction_id))
            return false;
    }
    if (!emit_call_failure_outputs(buffer, signature, instruction_id, false) ||
        !append_text(buffer, ");\n"))
        return false;
    if (!emit_coroutine_ref_frame_transfers(buffer, ir, function, callee, instruction, true))
        return false;
    if (!append_format(buffer, "        if (child_%u.kind == UINT32_C(5)) {\n", instruction_id))
        return false;
    for (uint32_t live = 0u; live < point->live_value_count; ++live) {
        if (!append_format(buffer, "            frame->live_%u_%u = v%u;\n", safepoint_id, live,
                           instruction->operands[callee->parameter_count + live]))
            return false;
    }
    if (!append_format(buffer,
                       "            frame->state = UINT32_C(%u);\n"
                       "            child_%u.safepoint_id = UINT32_C(%u);\n"
                       "            return child_%u;\n"
                       "        }\n"
                       "        frame->child_active_%u = UINT8_C(0);\n",
                       point->resume_state_id, instruction_id, safepoint_id, instruction_id,
                       safepoint_id))
        return false;
    return emit_call_completion(buffer, function, instruction,
                                &ir->program->signatures[callee->signature_id], function_id,
                                instruction_id);
}

static bool emit_indirect_coroutine_call(CBuffer *buffer, const XrBackendIR *ir,
                                         const XrValidatedFunction *function,
                                         const XrValidatedInstruction *instruction,
                                         uint32_t function_id, uint32_t instruction_id) {
    const XrValidatedSignature *signature = callable_signature(ir, function, instruction);
    uint32_t safepoint_id = instruction->immediate.u32;
    uint32_t callable_value = instruction->operands[0];
    uint16_t callable_type = function->value_types[callable_value];
    uint32_t parameter_prefix = signature ? signature->parameter_count + 1u : 0u;
    const XrValidatedCoroutineSafepoint *point = safepoint_id < function->coroutine_safepoint_count
                                                     ? &function->coroutine_safepoints[safepoint_id]
                                                     : NULL;
    if (!signature || !point ||
        !append_format(buffer,
                       "        XrAotOutcome child_%u = xr_aot_make(4, 0, 0);\n"
                       "        uint8_t child_sync_%u = UINT8_C(0);\n",
                       instruction_id, instruction_id))
        return false;
    if (!emit_call_failure_outputs(buffer, signature, instruction_id, true))
        return false;
    if (signature->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
        char storage[32];
        const char *type = type_c_name(signature->result_type_id, storage);
        if (!type ||
            !append_format(buffer, "        %s child_result_%u = {0};\n", type, instruction_id))
            return false;
    }
    if (!append_format(buffer,
                       "        if (!frame->child_active_%u) {\n"
                       "            frame->child_function_%u = v%u.function_id;\n"
                       "            switch (frame->child_function_%u) {\n",
                       safepoint_id, safepoint_id, callable_value, safepoint_id))
        return false;
    for (uint32_t target_id = 0u; target_id < ir->program->function_count; ++target_id) {
        if (!callable_type_can_target(ir, callable_type, target_id))
            continue;
        const XrValidatedFunction *target = &ir->program->functions[target_id];
        const XrValidatedFunction *semantic_target = &ir->program->functions[target_id];
        uint32_t receiver_count = semantic_target->has_receiver ? 1u : 0u;
        if (target->parameter_count != signature->parameter_count + receiver_count ||
            !append_format(buffer, "                case UINT32_C(%u):\n", target_id))
            return false;
        if (target->coroutine_safepoint_count == 0u) {
            if (!append_format(buffer, "                    child_%u = xr_aot_fn_%u(xr_ctx",
                               instruction_id, target_id))
                return false;
            if (receiver_count != 0u) {
                char storage[32];
                const char *capture_type = type_c_name(target->parameter_types[0], storage);
                if (!capture_type || !append_format(buffer, ", *(const %s *)v%u.capture",
                                                    capture_type, callable_value))
                    return false;
            }
            for (uint32_t parameter = 0u; parameter < signature->parameter_count; ++parameter)
                if (!append_format(buffer, ", v%u", instruction->operands[parameter + 1u]))
                    return false;
            if (signature->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE &&
                !append_format(buffer, ", &child_result_%u", instruction_id))
                return false;
            if (!emit_call_failure_outputs(buffer, signature, instruction_id, false))
                return false;
            if (!append_format(buffer,
                               ");\n"
                               "                    child_sync_%u = UINT8_C(1);\n"
                               "                    break;\n",
                               instruction_id))
                return false;
            continue;
        }
        if (!append_format(buffer,
                           "                    { XrAotCoroutineFrame%u child_zero = {0};\n"
                           "                      frame->child_%u.target_%u = child_zero; }\n",
                           target_id, safepoint_id, target_id))
            return false;
        for (uint32_t parameter = 0u; parameter < target->parameter_count; ++parameter) {
            if (parameter < receiver_count) {
                char storage[32];
                const char *capture_type = type_c_name(target->parameter_types[0], storage);
                if (!capture_type ||
                    !append_format(buffer,
                                   "                    frame->child_%u.target_%u.parameter_0 = "
                                   "*(const %s *)v%u.capture;\n",
                                   safepoint_id, target_id, capture_type, callable_value))
                    return false;
            } else if (!append_format(
                           buffer,
                           "                    frame->child_%u.target_%u.parameter_%u = v%u;\n",
                           safepoint_id, target_id, parameter,
                           instruction->operands[1u + parameter - receiver_count])) {
                return false;
            }
        }
        if (!append_format(buffer,
                           "                    frame->child_active_%u = UINT8_C(1);\n"
                           "                    break;\n",
                           safepoint_id))
            return false;
    }
    if (!append_text(buffer, "                default:\n"
                             "                    break;\n"
                             "            }\n"
                             "        }\n") ||
        !append_format(buffer,
                       "        if (frame->child_active_%u) {\n"
                       "            switch (frame->child_function_%u) {\n",
                       safepoint_id, safepoint_id))
        return false;
    for (uint32_t target_id = 0u; target_id < ir->program->function_count; ++target_id) {
        if (!callable_type_can_target(ir, callable_type, target_id))
            continue;
        const XrValidatedFunction *target = &ir->program->functions[target_id];
        if (target->coroutine_safepoint_count == 0u)
            continue;
        if (!append_format(buffer,
                           "                case UINT32_C(%u):\n"
                           "                    child_%u = xr_aot_fn_%u_step("
                           "xr_ctx, &frame->child_%u.target_%u, UINT8_C(0)",
                           target_id, instruction_id, target_id, safepoint_id, target_id))
            return false;
        for (uint32_t parameter = 0u; parameter < target->parameter_count; ++parameter)
            if (!append_format(buffer, ", frame->child_%u.target_%u.parameter_%u", safepoint_id,
                               target_id, parameter))
                return false;
        if (signature->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE &&
            !append_format(buffer, ", &child_result_%u", instruction_id))
            return false;
        if (!emit_call_failure_outputs(buffer, signature, instruction_id, false) ||
            !append_text(buffer, ");\n                    break;\n"))
            return false;
    }
    if (!append_text(buffer, "                default:\n"
                             "                    break;\n"
                             "            }\n"
                             "        }\n") ||
        !append_format(buffer,
                       "        if (!frame->child_active_%u && !child_sync_%u) "
                       "XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n"
                       "        if (child_%u.kind == UINT32_C(5)) {\n",
                       safepoint_id, instruction_id, instruction_id))
        return false;
    for (uint32_t live = 0u; live < point->live_value_count; ++live)
        if (!append_format(buffer, "            frame->live_%u_%u = v%u;\n", safepoint_id, live,
                           instruction->operands[parameter_prefix + live]))
            return false;
    if (!append_format(buffer,
                       "            frame->state = UINT32_C(%u);\n"
                       "            child_%u.safepoint_id = UINT32_C(%u);\n"
                       "            return child_%u;\n"
                       "        }\n"
                       "        frame->child_active_%u = UINT8_C(0);\n",
                       point->resume_state_id, instruction_id, safepoint_id, instruction_id,
                       safepoint_id))
        return false;
    return emit_call_completion(buffer, function, instruction, signature, function_id,
                                instruction_id);
}

/* One immutable byte table per string constant; the constant operation copies
 * it into a fresh arena owner so every produced string has one release site. */
static bool emit_string_constant_tables(CBuffer *buffer, const XrBackendIR *ir) {
    for (uint32_t constant = 0; constant < ir->program->constant_count; ++constant) {
        const XrValidatedConstant *value = &ir->program->constants[constant];
        if (value->kind != XR_CORE_IR_CONSTANT_STRING)
            continue;
        if (!append_format(buffer, "static const uint8_t xr_aot_text_%u[%u] = {", constant,
                           value->value.string.size != 0u ? value->value.string.size : 1u))
            return false;
        if (value->value.string.size == 0u && !append_text(buffer, "0"))
            return false;
        for (uint32_t index = 0; index < value->value.string.size; ++index) {
            if (!append_format(buffer, "%s%u", index != 0u ? "," : "",
                               (unsigned) value->value.string.bytes[index]))
                return false;
        }
        if (!append_text(buffer, "};\n"))
            return false;
    }
    return true;
}

static const char *display_operand_kind(uint16_t type_id) {
    const XrCoreIntegerType *integer = xr_core_spec_integer_type(type_id);
    if (integer)
        return integer->is_signed ? "XR_TEXT_DISPLAY_I64" : "XR_TEXT_DISPLAY_U64";
    switch (type_id) {
        case XR_CORE_TYPE_F64:
            return "XR_TEXT_DISPLAY_F64";
        case XR_CORE_TYPE_BOOL:
            return "XR_TEXT_DISPLAY_BOOL";
        case XR_CORE_TYPE_STRING:
            return "XR_TEXT_DISPLAY_STRING";
        case XR_CORE_TYPE_RUNE:
            return "XR_TEXT_DISPLAY_RUNE";
        default:
            return NULL;
    }
}

#include "xr_backend_ir_emit_string_builder.inc.c"

static bool emit_output_group(CBuffer *buffer, const XrValidatedFunction *function,
                              const XrValidatedInstruction *instruction, uint32_t function_id) {
    uint32_t count = instruction->operand_count;
    if (instruction->successor_count != 0u)
        count -= function->blocks[instruction->successors[0]].argument_count;
    if (!append_format(buffer,
                       "        {\n"
                       "            XrTextDisplayOperand xr_operands[%u];\n"
                       "            int xr_ok = 0;\n"
                       "            size_t xr_size;\n"
                       "            uint8_t *xr_line;\n"
                       "            memset(xr_operands, 0, sizeof(xr_operands));\n",
                       count != 0u ? count : 1u))
        return false;
    for (uint32_t operand = 0; operand < count; ++operand) {
        uint16_t type_id = function->value_types[instruction->operands[operand]];
        const char *kind = display_operand_kind(type_id);
        if (!kind ||
            !append_format(buffer, "            xr_operands[%u].kind = %s;\n", operand, kind))
            return false;
        const XrCoreIntegerType *integer = xr_core_spec_integer_type(type_id);
        if (integer) {
            if (!append_format(buffer, "            xr_operands[%u].%s = v%u;\n", operand,
                                integer->is_signed ? "i64" : "u64", instruction->operands[operand]))
                return false;
            continue;
        }
        switch (type_id) {
            case XR_CORE_TYPE_F64:
                if (!append_format(buffer, "            xr_operands[%u].f64 = v%u;\n",
                                   operand, instruction->operands[operand])) return false;
                break;
            case XR_CORE_TYPE_BOOL:
                if (!append_format(buffer, "            xr_operands[%u].boolean = v%u != 0;\n",
                                   operand, instruction->operands[operand]))
                    return false;
                break;
            case XR_CORE_TYPE_STRING:
                if (!append_format(buffer,
                                   "            xr_operands[%u].bytes = v%u->bytes;\n"
                                   "            xr_operands[%u].size = v%u->size;\n",
                                   operand, instruction->operands[operand], operand,
                                   instruction->operands[operand]))
                    return false;
                break;
            default:
                if (!append_format(buffer, "            xr_operands[%u].rune = v%u;\n", operand,
                                   instruction->operands[operand]))
                    return false;
                break;
        }
    }
    return append_format(buffer,
                         "            xr_size = xr_text_group_size(xr_operands, %u, &xr_ok);\n"
                         "            xr_line = xr_ok ? (uint8_t *)malloc(xr_size) : NULL;\n"
                         "            if (!xr_line) XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n"
                         "            (void)xr_text_group_render(xr_operands, %u, xr_line);\n"
                         "            if (!xr_ctx->provider_output_write || "
                         "xr_ctx->provider_output_write(xr_ctx->provider_context, UINT32_C(%u), "
                         "UINT32_C(%u), xr_line, xr_size) != 0) { free(xr_line);\n",
                         count, count, instruction->immediate.provider_operation.requirement_index,
                         instruction->immediate.provider_operation.operation_index) &&
           emit_provider_failure(buffer, function, instruction, count, function_id) &&
           append_text(buffer, "            }\n"
                               "            free(xr_line);\n"
                               "        }\n");
}

static bool emit_operation_panic_value(CBuffer *buffer, const XrValidatedFunction *function,
                                       const XrValidatedInstruction *instruction,
                                       uint32_t function_id, const char *panic_expression) {
    if (instruction->successor_count == 1u)
        return emit_invoke_edge(buffer, function, instruction, 0u, 1u,
                                xr_core_spec_panic_value_prefix(instruction->operation_id,
                                                               instruction->immediate.u32),
                                function_id, panic_expression);
    return append_format(buffer,
        "{ *out_panic = %s; XR_AOT_FAIL(xr_aot_make(3, 0, 0)); }\n", panic_expression);
}

static bool emit_operation_panic(CBuffer *buffer, const XrValidatedFunction *function,
                                 const XrValidatedInstruction *instruction, uint32_t function_id,
                                 uint32_t panic_code) {
    char panic_expression[96];
    (void)snprintf(panic_expression, sizeof(panic_expression),
                   "(XrAotPanicInfo){UINT32_C(%u), 0, 0, 0, NULL}", panic_code);
    return emit_operation_panic_value(buffer, function, instruction, function_id, panic_expression);
}

#include "xr_backend_ir_emit_provider_typed.inc.c"

static bool emit_instruction(CBuffer *buffer, const XrBackendIR *ir,
                             const XrValidatedFunction *function,
                             const XrValidatedInstruction *instruction, uint32_t function_id,
                             uint32_t instruction_id) {
    switch (instruction->operation_id) {
        case XR_CORE_OP_CORE_STRING_SLICE:
            if (!append_format(buffer,
                "        { size_t offset = 0, length = 0;\n"
                "          if (!xr_text_scalar_range(v%u->bytes, v%u->size, v%u, v%u, &offset, &length)) ",
                instruction->operands[0], instruction->operands[0], instruction->operands[1], instruction->operands[2]) ||
                !emit_operation_panic(buffer, function, instruction, function_id, 430u)) return false;
            return append_format(buffer,
                "          v%u = xr_aot_string_from_bytes(xr_ctx, length ? v%u->bytes + offset : NULL, length);\n"
                "          if (!v%u) XR_AOT_FAIL(xr_aot_make(4, 0, 0)); }\n",
                instruction->result_id, instruction->operands[0], instruction->result_id);
        case XR_CORE_OP_CORE_INTEGER_BITWISE: {
            const XrCoreIntegerType *integer = xr_core_spec_integer_type(instruction->result_type_id);
            char storage[32], right[48];
            const char *type = type_c_name(instruction->result_type_id, storage);
            if (!integer || !type) return false;
            if (instruction->operand_count == 2u)
                (void)snprintf(right, sizeof(right), "(uint64_t)v%u", instruction->operands[1]);
            else
                (void)snprintf(right, sizeof(right), "UINT64_C(0)");
            return append_format(buffer,
                "        v%u = (%s)%sxr_integer_bitwise_bits((uint64_t)v%u, %s, %uu, %s, %uu)%s;\n",
                instruction->result_id, type, integer->is_signed ? "xr_integer_signed_from_bits(" : "",
                instruction->operands[0], right, integer->width, integer->is_signed ? "true" : "false",
                instruction->immediate.u32, integer->is_signed ? ")" : "");
        }
        case XR_CORE_OP_CORE_INTEGER_DIVMOD: {
            const XrCoreIntegerType *integer =
                xr_core_spec_integer_type(instruction->result_type_id);
            char storage[32];
            const char *type = type_c_name(instruction->result_type_id, storage);
            if (!integer || !type ||
                !append_format(
                    buffer,
                    "        { XrIntegerDivModResult integer_division = xr_integer_divmod_eval("
                    "(uint64_t)v%u, (uint64_t)v%u, %uu, %s, %s);\n"
                    "          if (integer_division.divisor_is_zero) ",
                    instruction->operands[0], instruction->operands[1], integer->width,
                    integer->is_signed ? "true" : "false",
                    instruction->immediate.u32 ? "true" : "false") ||
                !emit_operation_panic(buffer, function, instruction, function_id,
                                      instruction->immediate.u32 ? 421u : 420u))
                return false;
            return append_format(
                buffer, "          else v%u = (%s)%s;\n        }\n", instruction->result_id, type,
                integer->is_signed ? "xr_integer_signed_from_bits(integer_division.bits)"
                                   : "integer_division.bits");
        }
        case XR_CORE_OP_CORE_SCALAR_BITCAST64:
            return append_format(buffer, "        memcpy(&v%u, &v%u, sizeof(v%u));\n",
                                 instruction->result_id, instruction->operands[0], instruction->result_id);
        case XR_CORE_OP_CORE_INTEGER_CONVERT: {
            const XrCoreIntegerType *source =
                xr_core_spec_integer_type(function->value_types[instruction->operands[0]]);
            const XrCoreIntegerType *target =
                xr_core_spec_integer_type(instruction->result_type_id);
            char storage[32];
            const char *type = type_c_name(instruction->result_type_id, storage);
            return source && target && type &&
                   append_format(buffer,
                                 "        v%u = (%s)%sxr_integer_convert_bits((uint64_t)v%u, "
                                 "%u, %s, %u, %s)%s;\n",
                                 instruction->result_id, type,
                                 target->is_signed ? "xr_integer_signed_from_bits(" : "",
                                 instruction->operands[0], source->width,
                                 source->is_signed ? "true" : "false", target->width,
                                 target->is_signed ? "true" : "false",
                                 target->is_signed ? ")" : "");
        }
        case XR_CORE_OP_CORE_CONSTANT_STRING: {
            const XrValidatedConstant *constant =
                &ir->program->constants[instruction->immediate.constant_id];
            return append_format(buffer,
                                 "        v%u = xr_aot_string_from_bytes(xr_ctx, xr_aot_text_%u, "
                                 "%u);\n"
                                 "        if (!v%u) XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n",
                                 instruction->result_id, instruction->immediate.constant_id,
                                 constant->value.string.size, instruction->result_id);
        }
        case XR_CORE_OP_CORE_CONSTANT_RUNE: {
            const XrValidatedConstant *constant =
                &ir->program->constants[instruction->immediate.constant_id];
            return append_format(buffer, "        v%u = UINT32_C(%u);\n", instruction->result_id,
                                 constant->value.rune);
        }
        case XR_CORE_OP_CORE_STRING_FROM_SCALAR: {
            uint16_t source = function->value_types[instruction->operands[0]];
            const XrCoreIntegerType *integer = xr_core_spec_integer_type(source);
            const char *field = integer ? (integer->is_signed ? "i64" : "u64") :
                source == XR_CORE_TYPE_F64 ? "f64" : source == XR_CORE_TYPE_BOOL ? "boolean" : "rune";
            const char *kind = display_operand_kind(source);
            return kind && append_format(buffer,
                "        { XrTextDisplayOperand operand = {0}; operand.kind = %s; operand.%s = v%u;\n"
                "          v%u = xr_aot_string_from_scalar(xr_ctx, operand);\n"
                "          if (!v%u) XR_AOT_FAIL(xr_aot_make(4, 0, 0)); }\n",
                kind, field, instruction->operands[0], instruction->result_id, instruction->result_id);
        }
        case XR_CORE_OP_CORE_SEQUENCE_LENGTH:
            return append_format(buffer, "        v%u = (int64_t)v%u%s;\n",
                                  instruction->result_id, instruction->operands[0],
                                  function->value_types[instruction->operands[0]] == XR_CORE_TYPE_STRING
                                      ? "->scalar_count" : ".storage->length");
        case XR_CORE_OP_CORE_STRING_CONCAT:
            return append_format(buffer,
                                 "        v%u = xr_aot_string_concat(xr_ctx, v%u, v%u);\n"
                                 "        if (!v%u) XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n",
                                 instruction->result_id, instruction->operands[0],
                                 instruction->operands[1], instruction->result_id);
        case XR_CORE_OP_CORE_COMPARE_STRING:
            return append_format(buffer,
                                 "        v%u = (uint8_t)xr_text_predicate(xr_text_compare("
                                 "v%u->bytes, v%u->size, v%u->bytes, v%u->size), "
                                 "UINT32_C(%u));\n",
                                 instruction->result_id, instruction->operands[0],
                                 instruction->operands[0], instruction->operands[1],
                                 instruction->operands[1], instruction->immediate.u32);
        case XR_CORE_OP_CORE_COMPARE_RUNE: {
            static const char *operators[] = {"==", "!=", "<", "<=", ">", ">="};
            return append_format(buffer, "        v%u = (uint8_t)(v%u %s v%u);\n",
                                 instruction->result_id, instruction->operands[0],
                                 operators[instruction->immediate.u32], instruction->operands[1]);
        }
        case XR_CORE_OP_CORE_OUTPUT_GROUP:
            return emit_output_group(buffer, function, instruction, function_id);
        case XR_CORE_OP_CORE_CONSTANT_F64: {
            const XrValidatedConstant *constant =
                &ir->program->constants[instruction->immediate.constant_id];
            return append_format(buffer,
                "        { uint64_t bits = UINT64_C(%llu); memcpy(&v%u, &bits, sizeof(bits)); }\n",
                (unsigned long long) constant->value.f64_bits, instruction->result_id);
        }
        case XR_CORE_OP_CORE_CONSTANT_I64: {
            const XrValidatedConstant *constant =
                &ir->program->constants[instruction->immediate.constant_id];
            if (!append_format(buffer, "        v%u = ", instruction->result_id) ||
                !emit_i64_literal(buffer, constant->value.i64))
                return false;
            return append_text(buffer, ";\n");
        }
        case XR_CORE_OP_CORE_CONSTANT_BOOL: {
            const XrValidatedConstant *constant =
                &ir->program->constants[instruction->immediate.constant_id];
            return append_format(buffer, "        v%u = UINT8_C(%u);\n", instruction->result_id,
                                 constant->value.boolean ? 1u : 0u);
        }
        case XR_CORE_OP_CORE_CONSTANT_TARGET_ENUM:
            return append_format(buffer, "        v%u = UINT16_C(%u);\n", instruction->result_id,
                                 instruction->immediate.u32);
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
                                     "        if (!xr_aot_checked_%s(v%u, v%u, &v%u)) "
                                     "XR_AOT_FAIL(xr_aot_make(1, 0, 1));\n",
                                     name, instruction->operands[0], instruction->operands[1],
                                     instruction->result_id);
            return append_format(buffer,
                                 "        v%u = xr_aot_i64_from_bits((uint64_t)v%u %s "
                                 "(uint64_t)v%u);\n",
                                 instruction->result_id, instruction->operands[0], symbol,
                                 instruction->operands[1]);
        }
        case XR_CORE_OP_CORE_DIV_I64:
            return append_format(
                buffer,
                "        if (v%u == 0) XR_AOT_FAIL(xr_aot_make(1, 0, 2));\n"
                "        if (v%u == INT64_MIN && v%u == -1) XR_AOT_FAIL(xr_aot_make(1, 0, 3));\n"
                "        v%u = v%u / v%u;\n",
                instruction->operands[1], instruction->operands[0], instruction->operands[1],
                instruction->result_id, instruction->operands[0], instruction->operands[1]);
        case XR_CORE_OP_CORE_LOGICAL_NOT:
            return append_format(buffer, "        v%u = (uint8_t)!v%u;\n", instruction->result_id,
                                 instruction->operands[0]);
        case XR_CORE_OP_CORE_LOGICAL_AND:
        case XR_CORE_OP_CORE_LOGICAL_OR:
            return append_format(buffer, "        v%u = (uint8_t)(v%u %s v%u);\n",
                                 instruction->result_id, instruction->operands[0],
                                 instruction->operation_id == XR_CORE_OP_CORE_LOGICAL_AND ? "&&"
                                                                                          : "||",
                                 instruction->operands[1]);
        case XR_CORE_OP_CORE_COMPARE_F64:
        case XR_CORE_OP_CORE_COMPARE_I64: {
            static const char *operators[] = {"==", "!=", "<", "<=", ">", ">="};
            return append_format(buffer, "        v%u = (uint8_t)(v%u %s v%u);\n",
                                 instruction->result_id, instruction->operands[0],
                                 operators[instruction->immediate.u32], instruction->operands[1]);
        }
        case XR_CORE_OP_CORE_COMPARE_TARGET_ENUM: {
            static const char *operators[] = {"==", "!="};
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
        case XR_CORE_OP_CORE_ASSERT_CONDITION:
            if (!(instruction->immediate.u32 & XR_CORE_ASSERT_MESSAGE_PRESENT))
                return append_format(buffer, "        if (!v%u) ", instruction->operands[0]) &&
                       emit_operation_panic(buffer, function, instruction, function_id,
                                            instruction->immediate.u32);
            return append_format(buffer,
                       "        if (!v%u) {\n"
                       "            XrAotPanicInfo panic = {UINT32_C(%u), 0, 0, 0, NULL};\n"
                       "            panic.message = xr_aot_string_from_bytes(xr_ctx, v%u->bytes, v%u->size);\n"
                       "            if (!panic.message) XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n",
                       instruction->operands[0],
                       instruction->immediate.u32 & ~XR_CORE_ASSERT_MESSAGE_PRESENT,
                       instruction->operands[1], instruction->operands[1]) &&
                   emit_operation_panic_value(buffer, function, instruction, function_id, "panic") &&
                   append_text(buffer, "        }\n");
        case XR_CORE_OP_CORE_RETURN:
            if (function->coroutine_safepoint_count != 0u &&
                !append_text(buffer, "        frame->state = UINT32_MAX;\n"))
                return false;
            return emit_return(buffer, function, instruction);
        case XR_CORE_OP_CORE_COROUTINE_YIELD: {
            uint32_t safepoint_id = instruction->immediate.u32;
            const XrValidatedCoroutineSafepoint *safepoint =
                &function->coroutine_safepoints[safepoint_id];
            if (instruction->operand_count < safepoint->live_value_count)
                return false;
            for (uint32_t live = 0; live < safepoint->live_value_count; ++live) {
                if (!append_format(buffer, "        frame->live_%u_%u = v%u;\n", safepoint_id, live,
                                   instruction->operands[live]))
                    return false;
            }
            return append_format(buffer,
                                 "        frame->state = UINT32_C(%u);\n"
                                 "        return xr_aot_suspend(UINT32_C(%u), UINT32_C(1), "
                                 "UINT32_C(0), INT64_C(0));\n",
                                 safepoint->resume_state_id, safepoint_id);
        }
        case XR_CORE_OP_CORE_COROUTINE_SUSPEND: {
            uint32_t safepoint_id = instruction->immediate.coroutine_suspend.safepoint_id;
            uint32_t request_count = instruction->immediate.coroutine_suspend.request_operand_count;
            const XrValidatedCoroutineSafepoint *safepoint =
                &function->coroutine_safepoints[safepoint_id];
            if (request_count != 1u ||
                instruction->operand_count < request_count + safepoint->live_value_count)
                return false;
            for (uint32_t live = 0u; live < safepoint->live_value_count; ++live) {
                if (!append_format(buffer, "        frame->live_%u_%u = v%u;\n", safepoint_id, live,
                                   instruction->operands[request_count + live]))
                    return false;
            }
            return append_format(
                buffer,
                "        frame->state = UINT32_C(%u);\n"
                "        return xr_aot_suspend(UINT32_C(%u), UINT32_C(2), UINT32_C(1), "
                "v%u <= INT64_C(0) ? INT64_C(0) : "
                "(v%u > INT64_C(86400000) ? INT64_C(86400000) : v%u));\n",
                safepoint->resume_state_id, safepoint_id, instruction->operands[0],
                instruction->operands[0], instruction->operands[0]);
        }
        case XR_CORE_OP_CORE_COROUTINE_CALL_SEALED:
            return emit_coroutine_call(buffer, ir, function, instruction, function_id,
                                       instruction_id);
        case XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT:
            return emit_indirect_coroutine_call(buffer, ir, function, instruction, function_id,
                                                instruction_id);
        case XR_CORE_OP_CORE_CALL_SEALED_DIRECT:
            return emit_call(buffer, ir, function, instruction, function_id, instruction_id);
        case XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT:
            return emit_callable_call(buffer, ir, function, instruction, function_id,
                                      instruction_id);
        case XR_CORE_OP_CORE_CALL_SEALED_INVOKE:
            return emit_invoke(buffer, ir, function, instruction, function_id, instruction_id);
        case XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE:
            return emit_callable_invoke(buffer, ir, function, instruction, function_id,
                                        instruction_id);
        case XR_CORE_OP_CORE_CALL_WITNESS_DIRECT:
            return emit_witness_call(buffer, ir, function, instruction, function_id,
                                     instruction_id);
        case XR_CORE_OP_CORE_CALL_WITNESS_INVOKE:
            return emit_witness_invoke(buffer, ir, function, instruction, function_id,
                                       instruction_id);
        case XR_CORE_OP_CORE_TRAP:
            return append_format(buffer, "        XR_AOT_FAIL(xr_aot_make(1, 0, %u));\n",
                                 instruction->immediate.u32);
        case XR_CORE_OP_CORE_CANCEL_PUBLISH:
            return append_text(buffer, "        frame->state = UINT32_MAX;\n"
                                       "        return xr_aot_make(6, 0, 0);\n");
        case XR_CORE_OP_CORE_ERROR_PUBLISH:
        case XR_CORE_OP_CORE_PANIC_PUBLISH:
            if (function->coroutine_safepoint_count != 0u &&
                !append_text(buffer, "        frame->state = UINT32_MAX;\n"))
                return false;
            return append_format(
                buffer,
                "        *out_%s = v%u;\n"
                "        return xr_aot_make(%u, 0, 0);\n",
                instruction->operation_id == XR_CORE_OP_CORE_ERROR_PUBLISH ? "error" : "panic",
                instruction->operands[0],
                instruction->operation_id == XR_CORE_OP_CORE_ERROR_PUBLISH ? 2u : 3u);
        case XR_CORE_OP_CORE_TARGET_POINTER_WIDTH:
            return append_format(buffer, "        v%u = UINT16_C(%u);\n", instruction->result_id,
                                 ir->pointer_width);
        case XR_CORE_OP_CORE_TARGET_OPERATING_SYSTEM:
            return append_format(buffer, "        v%u = UINT16_C(%u);\n", instruction->result_id,
                                 ir->operating_system);
        case XR_CORE_OP_CORE_TARGET_ARCHITECTURE:
            return append_format(buffer, "        v%u = UINT16_C(%u);\n", instruction->result_id,
                                 ir->architecture);
        case XR_CORE_OP_CORE_TARGET_NATIVE_ABI:
            return append_format(buffer, "        v%u = UINT16_C(%u);\n", instruction->result_id,
                                 ir->native_abi);
        case XR_CORE_OP_CORE_TARGET_ENDIANNESS:
            return append_format(buffer, "        v%u = UINT16_C(%u);\n", instruction->result_id,
                                 ir->endianness);
        case XR_CORE_OP_CORE_PROVIDER_CALL: {
            uint32_t provider_operand_count = instruction->operand_count;
            if (instruction->successor_count == 1u) {
                const XrValidatedBlock *target = &function->blocks[instruction->successors[0]];
                provider_operand_count -= target->argument_count;
            }
            return emit_typed_provider_call(buffer, ir, function, instruction,
                                            provider_operand_count, function_id);
        }
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
            if (!name || !emit_allocation_alignment(buffer, name, "        ") ||
                !append_format(buffer,
                               "        %s *callable_capture_%u = "
                               "(%s *)xr_aot_alloc(xr_ctx, sizeof(%s));\n",
                               name, instruction->result_id, name, name) ||
                !append_format(
                    buffer,
                    "        if (!callable_capture_%u) XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n"
                    "        *callable_capture_%u = v%u;\n",
                    instruction->result_id, instruction->result_id, instruction->operands[0]))
                return false;
            return append_format(buffer,
                                 "        v%u = (XrAotType%u){.function_id = UINT32_C(%u), "
                                 ".capture = (void *)callable_capture_%u};\n",
                                 instruction->result_id, instruction->result_type_id, target_id,
                                 instruction->result_id);
        }
        case XR_CORE_OP_CORE_CLASS_CONSTRUCT:
            return emit_class_construct(buffer, instruction);
        case XR_CORE_OP_CORE_OWNER_ALIAS:
            return emit_owner_alias(buffer, ir, instruction);
        case XR_CORE_OP_CORE_CLASS_FIELD_LOAD:
            return emit_class_field_load(buffer, ir, function, instruction);
        case XR_CORE_OP_CORE_CLASS_FIELD_PLACE:
            return emit_class_field_place(buffer, ir, function, instruction);
        case XR_CORE_OP_CORE_PLACE_EXCHANGE:
            if (!emit_module_place_check(buffer, ir, instruction->operands[0]) ||
                !emit_module_place_promotion(buffer, ir, instruction->operands[0],
                                             instruction->operands[1], instruction->result_type_id))
                return false;
            return emit_place_exchange(buffer, ir, function, instruction);
        case XR_CORE_OP_CORE_OWNER_COPY:
            return emit_owner_copy(buffer, ir, instruction);
        case XR_CORE_OP_CORE_OWNER_MOVE:
            return append_format(buffer, "        v%u = v%u;\n", instruction->result_id,
                                 instruction->operands[0]);
        case XR_CORE_OP_CORE_OWNER_DROP:
            if (instruction_drops_string_owner(function, instruction))
                return append_format(buffer,
                                     "        xr_aot_free(xr_ctx, v%u);\n        v%u = NULL;\n",
                                     instruction->operands[0], instruction->operands[0]);
            {
                char value[32];
                (void) snprintf(value, sizeof(value), "v%u", instruction->operands[0]);
                return emit_owned_value_drop(buffer, ir,
                                             function->value_types[instruction->operands[0]], value,
                                             "UINT32_C(1)");
            }
        case XR_CORE_OP_CORE_PLACE_LOCAL:
            return append_format(buffer, "        v%u = &v%u;\n", instruction->result_id,
                                 instruction->operands[0]);
        case XR_CORE_OP_CORE_PLACE_MODULE: {
            uint32_t module = instruction->immediate.module_slot.module_index;
            uint32_t ordinal = instruction->immediate.module_slot.slot_index;
            for (uint32_t index = 0u; index < module; ++index)
                ordinal += ir->program->modules[index].slot_count;
            return append_format(
                buffer,
                "        if (!xr_ctx->modules) XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n"
                "        v%u = &xr_ctx->modules->slot_%u;\n",
                instruction->result_id, ordinal);
        }
        case XR_CORE_OP_CORE_PLACE_INITIALIZE: {
            uint32_t place = instruction->operands[0], value = instruction->operands[1];
            if (!append_format(
                    buffer,
                    "        { uint32_t slot = xr_aot_module_slot(xr_ctx, v%u);\n"
                    "          if (slot == UINT32_MAX) XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n"
                    "          if (xr_ctx->modules->initialized[slot]) XR_AOT_FAIL(xr_aot_make(1, "
                    "0, 9));\n",
                    place) ||
                !emit_module_place_promotion(buffer, ir, place, value,
                                             function->value_types[value]) ||
                !append_format(buffer,
                               "          *v%u = v%u;\n"
                               "          xr_ctx->modules->initialized[slot] = 1;\n"
                               "          "
                               "xr_ctx->modules->publication_order[xr_ctx->modules->publication_"
                               "count++] = slot;\n"
                               "        }\n",
                               place, value))
                return false;
            return true;
        }
        case XR_CORE_OP_CORE_PLACE_LOAD:
            if (!emit_module_place_check(buffer, ir, instruction->operands[0]))
                return false;
            return append_format(buffer, "        v%u = *v%u;\n", instruction->result_id,
                                 instruction->operands[0]);
        case XR_CORE_OP_CORE_PLACE_STORE:
            if (!emit_module_place_check(buffer, ir, instruction->operands[0]) ||
                !emit_module_place_promotion(buffer, ir, instruction->operands[0],
                                             instruction->operands[1],
                                             function->value_types[instruction->operands[1]]))
                return false;
            return append_format(buffer, "        *v%u = v%u;\n", instruction->operands[0],
                                 instruction->operands[1]);
        case XR_CORE_OP_CORE_SEQUENCE_ELEMENT_PLACE: {
            uint32_t source = instruction->operands[0], index = instruction->operands[1];
            if (!append_format(buffer,
                "        if (v%u < 0 || (uint64_t)v%u >= v%u.storage->length) {\n"
                "            XrAotPanicInfo bounds = {UINT32_C(430), UINT32_C(1), v%u, v%u.storage->length, NULL};\n",
                index, index, source, index, source))
                return false;
            if (instruction->successor_count == 1u) {
                if (!emit_invoke_edge(buffer, function, instruction, 0u, 1u, 2u,
                                      function_id, "bounds"))
                    return false;
            } else if (!append_text(buffer,
                "            *out_panic = bounds; XR_AOT_FAIL(xr_aot_make(3, 0, 0));\n"))
                return false;
            return append_format(buffer,
                "        }\n        v%u = &v%u.storage->data[(uint64_t)v%u];\n",
                instruction->result_id, source, index);
        }
        case XR_CORE_OP_CORE_PLACE_PROJECT:
            if (!emit_module_place_check(buffer, ir, instruction->operands[0]))
                return false;
            return append_format(buffer, "        v%u = &v%u->f%u;\n", instruction->result_id,
                                 instruction->operands[0], instruction->immediate.field_ordinal);
        case XR_CORE_OP_CORE_PLACE_TAKE:
            return append_format(buffer, "        v%u = *v%u;\n        v%u = NULL;\n",
                                 instruction->result_id, instruction->operands[0],
                                 instruction->operands[0]);
        case XR_CORE_OP_CORE_STRING_BUILDER_CONSTRUCT:
        case XR_CORE_OP_CORE_STRING_BUILDER_APPEND:
        case XR_CORE_OP_CORE_STRING_BUILDER_CLEAR:
        case XR_CORE_OP_CORE_STRING_BUILDER_LENGTH:
        case XR_CORE_OP_CORE_STRING_BUILDER_SNAPSHOT:
            return emit_string_builder_operation(buffer, ir, function, instruction);
        case XR_CORE_OP_CORE_BYTES_TIMING_SAFE_EQUAL:
            return append_format(buffer,
                "        v%u = xr_crypto_core_timing_safe_equal(v%u.storage->data, v%u.storage->length, 1u, "
                "v%u.storage->data, v%u.storage->length, 1u);\n",
                instruction->result_id, instruction->operands[0], instruction->operands[0],
                instruction->operands[1], instruction->operands[1]);
        case XR_CORE_OP_CORE_CHANNEL_CONSTRUCT:
            return append_format(buffer,
                "        { int64_t capacity = v%u;\n"
                "          if (capacity < 0 || (uint64_t)capacity > UINT32_MAX ||\n"
                "              (capacity && sizeof(XrChannelMessageOwner) > SIZE_MAX / (uint64_t)capacity))\n"
                "              XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n"
                "          XrChannelStorage *channel = (XrChannelStorage *)malloc(sizeof(*channel));\n"
                "          XrChannelMessageOwner *slots = channel && capacity\n"
                "              ? (XrChannelMessageOwner *)calloc((size_t)capacity, sizeof(*slots)) : NULL;\n"
                "          if (!channel || (capacity && !slots) || !xr_channel_storage_init(channel, slots, (uint32_t)capacity)) {\n"
                "              free(slots); free(channel); XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n"
                "          }\n"
                "          v%u.storage = channel; }\n", instruction->operands[0], instruction->result_id);
        case XR_CORE_OP_CORE_CHANNEL_IS_CLOSED:
            return append_format(buffer,
                "        v%u = atomic_load_explicit(&v%u.storage->closed, memory_order_acquire);\n",
                instruction->result_id, instruction->operands[0]);
        case XR_CORE_OP_CORE_ATOMIC_CONSTRUCT:
            if (function->value_types[instruction->operands[0]] == XR_CORE_TYPE_F64)
                return append_format(buffer,
                    "        { int64_t bits; memcpy(&bits, &v%u, sizeof(bits));\n"
                    "          v%u.storage = (XrAtomicStorageCore *)malloc(sizeof(XrAtomicStorageCore));\n"
                    "          if (!v%u.storage) XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n"
                    "          xr_atomic_storage_init_core(v%u.storage, bits); }\n",
                    instruction->operands[0], instruction->result_id,
                    instruction->result_id, instruction->result_id);
            return append_format(buffer,
                "        v%u.storage = (XrAtomicStorageCore *)malloc(sizeof(XrAtomicStorageCore));\n"
                "        if (!v%u.storage) { XR_AOT_FAIL(xr_aot_make(4, 0, 0)); }\n"
                "        xr_atomic_storage_init_core(v%u.storage, (int64_t)v%u);\n",
                instruction->result_id, instruction->result_id, instruction->result_id,
                instruction->operands[0]);
        case XR_CORE_OP_CORE_ATOMIC_LOAD:
            if (instruction->result_type_id == XR_CORE_TYPE_F64)
                return append_format(buffer,
                    "        { int64_t bits = xr_atomic_i64_load_core(&v%u.storage->value, %u);\n"
                    "          memcpy(&v%u, &bits, sizeof(bits)); }\n",
                    instruction->operands[0], instruction->immediate.u32, instruction->result_id);
            return append_format(buffer, "        v%u = (%s)xr_atomic_i64_load_core(&v%u.storage->value, %u);\n",
                instruction->result_id, instruction->result_type_id == XR_CORE_TYPE_BOOL ? "uint8_t" : "int64_t",
                instruction->operands[0], instruction->immediate.u32);
        case XR_CORE_OP_CORE_ATOMIC_UPDATE:
            if (instruction->result_type_id == XR_CORE_TYPE_F64)
                return append_format(buffer,
                    "        v%u = xr_atomic_f64_fetch_update_core(&v%u.storage->value, v%u, %u, %u);\n",
                    instruction->result_id, instruction->operands[0], instruction->operands[1],
                    instruction->immediate.u32 >> 3u, instruction->immediate.u32 & 7u);
            return append_format(buffer,
                "        v%u = (%s)xr_atomic_i64_fetch_%s_core(&v%u.storage->value, (int64_t)v%u, %u);\n",
                instruction->result_id, instruction->result_type_id == XR_CORE_TYPE_BOOL ? "uint8_t" : "int64_t",
                (instruction->immediate.u32 >> 3u) == 0u ? "add" : (instruction->immediate.u32 >> 3u) == 1u ? "sub" : "xor",
                instruction->operands[0], instruction->operands[1], instruction->immediate.u32 & 7u);
        case XR_CORE_OP_CORE_ATOMIC_COMPARE_EXCHANGE:
            if (instruction->result_type_id == XR_CORE_TYPE_F64)
                return append_format(buffer,
                    "        { int64_t expected, desired;\n"
                    "          memcpy(&expected, &v%u, sizeof(expected)); memcpy(&desired, &v%u, sizeof(desired));\n"
                    "          (void)xr_atomic_i64_compare_exchange_core(&v%u.storage->value, &expected, desired, %u);\n"
                    "          memcpy(&v%u, &expected, sizeof(expected)); }\n",
                    instruction->operands[1], instruction->operands[2], instruction->operands[0],
                    instruction->immediate.u32, instruction->result_id);
            return append_format(buffer,
                "        { int64_t expected = (int64_t)v%u;\n"
                "          (void)xr_atomic_i64_compare_exchange_core(&v%u.storage->value, &expected, (int64_t)v%u, %u);\n"
                "          v%u = (%s)expected; }\n",
                instruction->operands[1], instruction->operands[0], instruction->operands[2],
                instruction->immediate.u32, instruction->result_id,
                instruction->result_type_id == XR_CORE_TYPE_BOOL ? "uint8_t" : "int64_t");
        case XR_CORE_OP_CORE_ATOMIC_EXCHANGE:
            if (instruction->result_type_id == XR_CORE_TYPE_F64)
                return append_format(buffer,
                    "        { int64_t bits; memcpy(&bits, &v%u, sizeof(bits));\n"
                    "          bits = xr_atomic_i64_exchange_core(&v%u.storage->value, bits, %u);\n"
                    "          memcpy(&v%u, &bits, sizeof(bits)); }\n",
                    instruction->operands[1], instruction->operands[0], instruction->immediate.u32,
                    instruction->result_id);
            return append_format(buffer,
                "        v%u = (%s)xr_atomic_i64_exchange_core(&v%u.storage->value, (int64_t)v%u, %u);\n",
                instruction->result_id, instruction->result_type_id == XR_CORE_TYPE_BOOL ? "uint8_t" : "int64_t",
                instruction->operands[0], instruction->operands[1], instruction->immediate.u32);
        case XR_CORE_OP_CORE_ARRAY_APPEND: {
            uint32_t receiver = instruction->operands[0], value = instruction->operands[1];
            const XrValidatedType *array = xr_validated_program_type(ir->program,
                function->value_types[receiver]);
            char storage[32];
            const char *element = array ? type_c_name(array->array_element_type, storage) : NULL;
            if (!element || !emit_module_place_check(buffer, ir, receiver) ||
                !emit_allocation_alignment(buffer, element, "        ")) return false;
            return append_format(buffer,
                "        { struct XrAotArrayStorage%u *array = v%u->storage;\n"
                "          if (array->length > UINT32_MAX) XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n"
                "          XrArrayAppendStorage view = {array->data, (uint32_t)array->length, array->capacity};\n"
                "          XrArrayAppendAllocator allocator = {xr_ctx, xr_aot_array_allocate, xr_aot_array_release};\n"
                "          if (xr_array_append_storage(&view, sizeof(%s), &v%u, UINT32_MAX, SIZE_MAX, &allocator) != XR_ARRAY_APPEND_OK)\n"
                "              XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n"
                "          array->data = (%s *)view.data; array->length = view.length; array->capacity = view.capacity;\n"
                "        }\n", array->type_id, receiver, element, value, element);
        }
        case XR_CORE_OP_CORE_ARRAY_ALLOCATE_DEFAULT: {
            const XrValidatedType *array = xr_validated_program_type(ir->program, instruction->result_type_id);
            char storage[32];
            const char *element = array ? type_c_name(array->array_element_type, storage) : NULL;
            uint32_t result = instruction->result_id;
            if (!element || !emit_allocation_alignment(buffer, element, "        ") ||
                !append_format(buffer,
                    "        { XrArrayAllocationPlan plan = xr_array_allocation_plan(v%u, sizeof(%s), UINT32_MAX, SIZE_MAX);\n"
                    "          if (plan.status == XR_ARRAY_ALLOCATION_INVALID_LENGTH) {\n",
                    instruction->operands[0], element) ||
                !emit_operation_panic(buffer, function, instruction, function_id, 452u) ||
                !append_format(buffer,
                    "          }\n"
                    "          if (plan.status != XR_ARRAY_ALLOCATION_OK) XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n"
                    "          v%u.storage = (struct XrAotArrayStorage%u *)xr_aot_alloc(xr_ctx, sizeof(*v%u.storage));\n"
                    "          if (!v%u.storage) XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n"
                    "          v%u.storage->owners = 1; v%u.storage->length = plan.count; v%u.storage->data = NULL;\n"
                    "          if (plan.count) {\n"
                    "            v%u.storage->data = (%s *)xr_aot_alloc(xr_ctx, plan.bytes);\n"
                    "            if (!v%u.storage->data) { xr_aot_free(xr_ctx, v%u.storage); XR_AOT_FAIL(xr_aot_make(4, 0, 0)); }\n"
                    "            for (uint32_t i = 0; i < plan.count; ++i) v%u.storage->data[i] = (%s)0;\n"
                    "          }\n        }\n",
                    result, instruction->result_type_id, result, result, result, result, result,
                    result, element, result, result, result, element)) return false;
            return append_format(buffer, "        v%u.storage->capacity = (uint32_t)v%u.storage->length;\n", result, result);
        }
        case XR_CORE_OP_CORE_ARRAY_CONSTRUCT: {
            const XrValidatedType *array = xr_validated_program_type(ir->program, instruction->result_type_id);
            char storage[32];
            const char *element = array ? type_c_name(array->array_element_type, storage) : NULL;
            uint32_t result = instruction->result_id;
            if (!element || !append_format(buffer,
                    "        v%u.storage = (struct XrAotArrayStorage%u *)xr_aot_alloc(xr_ctx, sizeof(*v%u.storage));\n"
                    "        if (!v%u.storage) XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n"
                    "        v%u.storage->owners = 1u;\n"
                    "        v%u.storage->length = UINT32_C(%u);\n"
                    "        v%u.storage->data = NULL;\n",
                    result, instruction->result_type_id, result, result, result, result,
                    instruction->operand_count, result))
                return false;
            if (instruction->operand_count == 0u)
                return append_format(buffer, "        v%u.storage->capacity = (uint32_t)v%u.storage->length;\n", result, result);
            if (!emit_allocation_alignment(buffer, element, "        ") ||
                !append_format(buffer,
                    "        if (v%u.storage->length > SIZE_MAX / sizeof(%s)) {\n"
                    "            xr_aot_free(xr_ctx, v%u.storage); XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n        }\n"
                    "        v%u.storage->data = (%s *)xr_aot_alloc(xr_ctx, v%u.storage->length * sizeof(%s));\n"
                    "        if (!v%u.storage->data) {\n"
                    "            xr_aot_free(xr_ctx, v%u.storage); XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n        }\n",
                    result, element, result, result, element, result, element, result, result))
                return false;
            for (uint32_t index = 0u; index < instruction->operand_count; ++index)
                if (!append_format(buffer, "        v%u.storage->data[%u] = v%u;\n",
                                    result, index, instruction->operands[index]))
                    return false;
            return append_format(buffer, "        v%u.storage->capacity = (uint32_t)v%u.storage->length;\n", result, result);
        }
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
                "        if (v%u.tag != UINT32_C(%u)) XR_AOT_FAIL(xr_aot_make(1, 0, 6));\n"
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
            if (!name || !emit_allocation_alignment(buffer, name, "        ") ||
                !append_format(buffer,
                               "        %s *existential_payload_%u = "
                               "(%s *)xr_aot_alloc(xr_ctx, sizeof(%s));\n",
                               name, instruction->result_id, name, name) ||
                !append_format(
                    buffer,
                    "        if (!existential_payload_%u) XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n",
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
        case XR_CORE_OP_CORE_EXISTENTIAL_REBORROW_READ:
            return append_format(
                buffer,
                "        v%u = (XrAotType%u){.concrete_type_id = v%u.concrete_type_id, "
                ".conformance_id = v%u.conformance_id, .data = v%u.data};\n",
                instruction->result_id, instruction->result_type_id, instruction->operands[0],
                instruction->operands[0], instruction->operands[0]);
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

static bool emit_coroutine_cancel_dispatch(CBuffer *buffer, const XrBackendIR *ir,
                                           const XrValidatedFunction *function,
                                           uint32_t function_id) {
    if (!append_text(buffer, "    if (xr_cancel) {\n        switch (frame->state) {\n"))
        return false;
    for (uint32_t state = 1u; state < function->coroutine_state_count; ++state) {
        uint32_t safepoint_id = 0u;
        const XrValidatedCoroutineSafepoint *point = NULL;
        for (; safepoint_id < function->coroutine_safepoint_count; ++safepoint_id) {
            if (function->coroutine_safepoints[safepoint_id].resume_state_id == state) {
                point = &function->coroutine_safepoints[safepoint_id];
                break;
            }
        }
        const XrValidatedInstruction *suspension =
            point ? coroutine_suspension_instruction(function, safepoint_id, NULL) : NULL;
        if (!suspension ||
            (suspension->successor_count != 2u &&
             !((suspension->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED ||
                suspension->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT) &&
               suspension->successor_count >= 3u)) ||
            !append_format(buffer, "            case UINT32_C(%u):\n", state))
            return false;
        const XrValidatedSignature *call_signature =
            coroutine_call_signature(ir, function, suspension);
        if (call_signature &&
            !emit_call_failure_outputs(buffer, call_signature, safepoint_id, true))
            return false;
        if (suspension->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED) {
            uint32_t callee_id = suspension->immediate.coroutine_call.function_id;
            const XrValidatedFunction *callee = &ir->program->functions[callee_id];
            if (!append_format(buffer,
                               "                if (!frame->child_active_%u) "
                               "return xr_aot_make(4, 0, 0);\n"
                               "                XrAotOutcome cancel_%u = "
                               "xr_aot_fn_%u_step(xr_ctx, &frame->child_%u, UINT8_C(1)",
                               safepoint_id, safepoint_id, callee_id, safepoint_id))
                return false;
            for (uint32_t parameter = 0u; parameter < callee->parameter_count; ++parameter) {
                if (!append_format(buffer, ", frame->child_%u.parameter_%u", safepoint_id,
                                   parameter))
                    return false;
            }
            if (callee->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
                const XrValidatedBlock *normal = &function->blocks[suspension->successors[0]];
                if (normal->argument_count == 0u ||
                    !append_format(buffer, ", &v%u", normal->argument_ids[0]))
                    return false;
            }
            if (!emit_call_failure_outputs(buffer, call_signature, safepoint_id, false))
                return false;
            if (!append_format(buffer,
                               ");\n"
                               "                frame->child_active_%u = UINT8_C(0);\n",
                               safepoint_id))
                return false;
        } else if (suspension->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT) {
            const XrValidatedSignature *signature = callable_signature(ir, function, suspension);
            uint16_t callable_type = function->value_types[suspension->operands[0]];
            if (!signature || !append_format(buffer,
                                             "                if (!frame->child_active_%u) "
                                             "return xr_aot_make(4, 0, 0);\n"
                                             "                XrAotOutcome cancel_%u = "
                                             "xr_aot_make(4, 0, 0);\n",
                                             safepoint_id, safepoint_id))
                return false;
            if (signature->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
                char storage[32];
                const char *type = type_c_name(signature->result_type_id, storage);
                if (!type || !append_format(buffer, "                %s cancel_result_%u = {0};\n",
                                            type, safepoint_id))
                    return false;
            }
            if (!append_format(buffer, "                switch (frame->child_function_%u) {\n",
                               safepoint_id))
                return false;
            for (uint32_t target_id = 0u; target_id < ir->program->function_count; ++target_id) {
                if (!callable_type_can_target(ir, callable_type, target_id) ||
                    ir->program->functions[target_id].coroutine_safepoint_count == 0u)
                    continue;
                const XrValidatedFunction *target = &ir->program->functions[target_id];
                if (!append_format(buffer,
                                   "                    case UINT32_C(%u):\n"
                                   "                        cancel_%u = xr_aot_fn_%u_step("
                                   "xr_ctx, &frame->child_%u.target_%u, UINT8_C(1)",
                                   target_id, safepoint_id, target_id, safepoint_id, target_id))
                    return false;
                for (uint32_t parameter = 0u; parameter < target->parameter_count; ++parameter)
                    if (!append_format(buffer, ", frame->child_%u.target_%u.parameter_%u",
                                       safepoint_id, target_id, parameter))
                        return false;
                if (signature->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE &&
                    !append_format(buffer, ", &cancel_result_%u", safepoint_id))
                    return false;
                if (!emit_call_failure_outputs(buffer, call_signature, safepoint_id, false) ||
                    !append_text(buffer, ");\n                        break;\n"))
                    return false;
            }
            if (!append_format(buffer,
                               "                    default:\n"
                               "                        break;\n"
                               "                }\n"
                               "                frame->child_active_%u = UINT8_C(0);\n",
                               safepoint_id))
                return false;
        }
        const XrValidatedBlock *cancel = &function->blocks[suspension->successors[1]];
        uint32_t live_operand_start = 0u;
        if (!coroutine_live_operand_start(ir, function, suspension, point, &live_operand_start))
            return false;
        uint32_t cancel_operand_start = live_operand_start + point->live_value_count;
        if (cancel_operand_start > suspension->operand_count ||
            cancel->argument_count > suspension->operand_count - cancel_operand_start)
            return false;
        for (uint32_t live = 0u; live < point->live_value_count; ++live) {
            if (!append_format(buffer, "                v%u = frame->live_%u_%u;\n",
                               suspension->operands[live_operand_start + live], safepoint_id, live))
                return false;
        }
        if (suspension->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED ||
            suspension->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT) {
            uint32_t trap = 2u + (call_signature->error_type_id != XR_CORE_TYPE_VOID ? 1u : 0u) +
                            (call_signature->panic_type_id != XR_CORE_TYPE_VOID ? 1u : 0u);
            uint32_t trap_operand = cancel_operand_start + cancel->argument_count;
            for (uint32_t edge = 2u; edge < trap; ++edge)
                trap_operand += function->blocks[suspension->successors[edge]].argument_count - 1u;
            if (suspension->successor_count == trap + 1u &&
                (!append_format(buffer,
                                "                if (cancel_%u.kind == UINT32_C(1) && "
                                "cancel_%u.trap == 7) ",
                                safepoint_id, safepoint_id) ||
                 !emit_parallel_edge(buffer, function, suspension, trap, trap_operand,
                                     function_id)))
                return false;
            if (!append_format(buffer,
                               "                if (cancel_%u.kind != UINT32_C(6)) {\n"
                               "                    frame->state = UINT32_MAX;\n"
                               "                    return cancel_%u;\n"
                               "                }\n",
                               safepoint_id, safepoint_id))
                return false;
        }
        if (!emit_parallel_edge(buffer, function, suspension, 1u, cancel_operand_start,
                                function_id))
            return false;
    }
    return append_text(buffer, "            default: return xr_aot_make(4, 0, 0);\n"
                               "        }\n"
                               "    }\n");
}

static void cleanup_consume(uint32_t *live, uint32_t *count, uint32_t value) {
    for (uint32_t index = 0u; index < *count; ++index) {
        if (live[index] != value)
            continue;
        memmove(&live[index], &live[index + 1u], (*count - index - 1u) * sizeof(*live));
        --*count;
        return;
    }
}

/* The validated block arguments define each ownership join. Keep only the
 * current emission cursor's owners, in acquisition order; no second CFG or
 * runtime ownership bitmap is needed for exceptional exits. */
static bool emit_failure_cleanup(CBuffer *buffer, const XrBackendIR *ir,
                                 const XrValidatedFunction *function, const uint32_t *live,
                                 uint32_t count) {
    if (function->coroutine_safepoint_count != 0u &&
        !append_text(buffer, "    frame->state = UINT32_MAX;\n"))
        return false;
    while (count) {
        uint32_t value = live[--count];
        char expression[32];
        (void) snprintf(expression, sizeof(expression), "v%u", value);
        if (!emit_owned_value_drop(buffer, ir, function->value_types[value], expression,
                                   "UINT32_C(4)"))
            return false;
    }
    return append_text(buffer, "    return xr_failure;\n");
}

static bool emit_function_blocks(CBuffer *buffer, const XrBackendIR *ir, uint32_t function_id) {
    const XrValidatedFunction *function = &ir->program->functions[function_id];
    const XrValidatedFunction *source = &ir->program->functions[function_id];
    uint32_t *live = xr_calloc(function->value_count ? function->value_count : 1u, sizeof(*live));
    if (!live) {
        buffer->failed = true;
        return false;
    }
    uint32_t serial = 0u;
    CBuffer failures = {0};
    bool emitted = true;
    for (uint32_t block_id = 0u; emitted && block_id < function->block_count; ++block_id) {
        const XrValidatedBlock *block = &function->blocks[block_id];
        uint32_t count = 0u;
        for (uint32_t argument = 0u; argument < block->argument_count; ++argument)
            if (block->argument_ownerships[argument] == XR_CORE_IR_OWNER)
                live[count++] = block->argument_ids[argument];
        emitted = append_format(buffer, "xr_f%u_b%u:\n    ;\n", function_id, block_id);
        for (uint32_t index = 0u; emitted && index < block->instruction_count; ++index, ++serial) {
            const XrValidatedInstruction *op = &block->instructions[index];
            const XrValidatedInstruction *canonical = &source->blocks[block_id].instructions[index];
            if (op->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED ||
                op->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT) {
                uint32_t point = op->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED
                                     ? op->immediate.coroutine_call.safepoint_id
                                     : op->immediate.u32;
                emitted = append_format(buffer, "xr_f%u_s%u:\n    ;\n", function_id, point);
                if (!emitted)
                    break;
            }
            /* Once a callee starts, its MOVE parameters are its responsibility,
             * including allocation failure and propagated traps. */
            for (uint32_t operand = 0u; operand < canonical->operand_count; ++operand)
                cleanup_consume(live, &count,
                                xr_validated_instruction_consumed_owner(
                                    ir->program, source, canonical, operand, block_id, true));
            CBuffer body = {0};
            emitted = emit_instruction(&body, ir, function, op, function_id, serial);
            bool can_fail = emitted && body.bytes && strstr(body.bytes, "XR_AOT_FAIL(");
            if (can_fail)
                emitted = append_format(buffer,
                                        "#define XR_AOT_FAIL(outcome_) do { xr_failure = "
                                        "(outcome_); goto xr_unwind_%u; } while (0)\n",
                                        serial) &&
                          append_format(&failures, "xr_unwind_%u:\n", serial) &&
                          emit_failure_cleanup(&failures, ir, function, live, count);
            emitted = emitted && (!body.bytes || append_text(buffer, body.bytes));
            if (emitted && can_fail)
                emitted = append_text(buffer, "#undef XR_AOT_FAIL\n");
            buffer->failed |= body.failed || failures.failed;
            xr_free(body.bytes);
            for (uint32_t operand = 0u; operand < canonical->operand_count; ++operand)
                cleanup_consume(live, &count,
                                xr_validated_instruction_consumed_owner(
                                    ir->program, source, canonical, operand, block_id, false));
            if (op->result_id != XR_PROGRAM_LOCATION_NONE &&
                op->result_ownership == XR_CORE_IR_OWNER)
                live[count++] = op->result_id;
        }
    }
    emitted = emitted && (!failures.bytes || append_text(buffer, failures.bytes));
    xr_free(failures.bytes);
    xr_free(live);
    return emitted;
}

static bool emit_function(CBuffer *buffer, const XrBackendIR *ir, uint32_t function_id) {
    const XrValidatedFunction *function = &ir->program->functions[function_id];
    if (!emit_function_signature(buffer, function, ir, function_id, false))
        return false;
    if (!append_text(buffer, "    (void)xr_ctx;\n"
                             "    XrAotOutcome xr_failure = {0}; (void)xr_failure;\n"))
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
            function->value_types[value] >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE ||
            function->value_types[value] == XR_CORE_TYPE_PANIC_INFO ? "{0}" : "0";
        const char *pointer = function->value_categories[value] == XR_CORE_IR_PLACE ? " *" : "";
        const char *value_initializer =
            function->value_categories[value] == XR_CORE_IR_PLACE ? "0" : initializer;
        if (!type ||
            !append_format(buffer, "    %s%s v%u = %s;\n", type, pointer, value,
                           value_initializer) ||
            !append_format(buffer, "    (void)v%u;\n", value))
            return false;
    }
    const XrValidatedBlock *entry = &function->blocks[function->entry_block];
    if (entry->argument_count != function->parameter_count)
        return false;
    for (uint32_t parameter = 0; parameter < function->parameter_count; ++parameter) {
        if (!append_format(buffer, "    v%u = p%u;\n", entry->argument_ids[parameter], parameter))
            return false;
    }
    if (function->coroutine_safepoint_count != 0u) {
        if (!append_text(buffer, "    if (!frame) return xr_aot_make(4, 0, 0);\n") ||
            !emit_coroutine_cancel_dispatch(buffer, ir, function, function_id) ||
            !append_text(buffer, "    switch (frame->state) {\n") ||
            !append_format(buffer, "        case UINT32_C(0): goto xr_f%u_b%u;\n", function_id,
                           function->entry_block))
            return false;
        for (uint32_t state = 1u; state < function->coroutine_state_count; ++state) {
            uint32_t resume_block = function->coroutine_states[state].continuation_block;
            const XrValidatedCoroutineSafepoint *point = NULL;
            uint32_t safepoint_id = 0u;
            for (; safepoint_id < function->coroutine_safepoint_count; ++safepoint_id) {
                if (function->coroutine_safepoints[safepoint_id].resume_state_id == state) {
                    point = &function->coroutine_safepoints[safepoint_id];
                    break;
                }
            }
            const XrValidatedInstruction *suspension =
                point ? coroutine_suspension_instruction(function, safepoint_id, NULL) : NULL;
            if (!point || !suspension ||
                !append_format(buffer, "        case UINT32_C(%u):\n", state))
                return false;
            uint32_t live_operand_start = 0u;
            if (!coroutine_live_operand_start(ir, function, suspension, point, &live_operand_start))
                return false;
            for (uint32_t live = 0; live < point->live_value_count; ++live) {
                uint32_t target =
                    suspension->operation_id != XR_CORE_OP_CORE_COROUTINE_CALL_SEALED &&
                            suspension->operation_id != XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT
                        ? function->blocks[resume_block].argument_ids[live]
                        : suspension->operands[live_operand_start + live];
                if (!append_format(buffer, "            v%u = frame->live_%u_%u;\n", target,
                                   safepoint_id, live))
                    return false;
            }
            bool child_call = suspension->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED ||
                              suspension->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT;
            if (!append_format(buffer, "            goto xr_f%u_%c%u;\n", function_id,
                               child_call ? 's' : 'b', child_call ? safepoint_id : resume_block))
                return false;
        }
        if (!append_text(buffer, "        default: return xr_aot_make(4, 0, 0);\n    }\n"))
            return false;
    } else if (!append_format(buffer, "    goto xr_f%u_b%u;\n", function_id,
                              function->entry_block)) {
        return false;
    }
    return emit_function_blocks(buffer, ir, function_id) && append_text(buffer, "}\n\n");
}

static bool emit_coroutine_entry_adapter(CBuffer *buffer, const XrBackendIR *ir) {
    const XrValidatedFunction *entry = &ir->program->functions[ir->program->entry_function];
    bool coroutine = entry->coroutine_safepoint_count != 0u;
    bool slots = needs_module_storage(ir);
    if (entry->parameter_count != 0u ||
        (entry->result_type_id != XR_CORE_TYPE_VOID && entry->result_type_id != XR_CORE_TYPE_I64))
        return !coroutine;
    bool checked = false, wrapping = false, arena = false, output = false;
    scan_helpers(ir, &checked, &wrapping, &arena, &output);
    bool entry_is_initializer = false;
    for (uint32_t module = 0u; module < ir->program->module_count; ++module)
        entry_is_initializer |=
            ir->program->modules[module].initializer == ir->program->entry_function;
    if (!append_text(buffer, "typedef struct XrAotEntryCoroutineFrame {\n"
                             "    XrAotContext context;\n"
                             "    XrAotContext *instance;\n"))
        return false;
    if (function_needs_failure_storage(ir, entry) &&
        !append_text(buffer, "    XrAotFailure failure;\n"))
        return false;
    if (coroutine) {
        if (!append_format(buffer, "    XrAotCoroutineFrame%u function;\n",
                           ir->program->entry_function))
            return false;
    } else if (!append_text(buffer, "    struct { uint32_t state; } function;\n"))
        return false;
    for (uint32_t module = 0u; module < ir->program->module_count; ++module) {
        uint32_t id = ir->program->modules[module].initializer;
        if (ir->program->functions[id].coroutine_safepoint_count != 0u &&
            !append_format(buffer, "    XrAotCoroutineFrame%u initializer_%u;\n", id, id))
            return false;
    }
    if (!append_text(buffer,
                     "} XrAotEntryCoroutineFrame;\n\n"
                     "#ifndef XR_NATIVE_DESCRIPTOR_H\n"
                     "typedef struct XrSuspensionRequest {\n"
                     "    uint32_t kind; uint16_t operand_count; uint16_t reserved16;\n"
                     "    union { int64_t timer_after_ms; } payload;\n"
                     "} XrSuspensionRequest;\n"
                     "typedef struct XrBackendNativeOutcome {\n"
                     "    uint32_t kind; int64_t value; uint32_t state_id; uint32_t safepoint_id;\n"
                     "    XrSuspensionRequest suspension;\n"
                     "    uint32_t panic_present; uint32_t panic_code;\n"
                     "    uint32_t panic_has_bounds; int64_t panic_index; uint64_t panic_length;\n"
                     "    uint32_t panic_has_message; const uint8_t *panic_message; size_t panic_message_size;\n"
                     "    uint16_t error_type_id; const void *error_value;\n"
                     "} XrBackendNativeOutcome;\n"
                     "typedef struct XrBackendNativeExecutionId { uint8_t bytes[32]; } "
                     "XrBackendNativeExecutionId;\n"
                     "typedef struct XrBackendNativeHost {\n"
                     "    void *context;\n"
                     "    XrAotProviderOutputWrite output_write;\n"
                     "    XrAotProviderCallTyped typed_call;\n"
                     "    XrAotProviderDisposeTyped typed_dispose;\n"
                     "    XrAotResourceFree resource_free;\n"
                     "} XrBackendNativeHost;\n"
                     "typedef void (*XrBackendNativeInitialize)(void *, void *, const "
                     "XrBackendNativeHost *);\n"
                     "typedef XrBackendNativeOutcome (*XrBackendNativeStep)(void *);\n"
                     "typedef XrBackendNativeOutcome (*XrBackendNativeCancel)(void *);\n"
                     "typedef void (*XrBackendNativeDrop)(void *);\n"
                     "typedef XrBackendNativeOutcome (*XrBackendNativeModuleStep)(void *, "
                     "uint32_t, uint8_t);\n"
                     "typedef struct XrBackendNativeDescriptor {\n"
                     "    uint32_t schema_version; uint32_t reserved32;\n"
                     "    XrBackendNativeExecutionId execution_id;\n"
                     "    size_t frame_size;\n"
                     "    const void *state_layout; size_t state_size;\n"
                     "    XrBackendNativeDrop state_initialize;\n"
                     "    XrBackendNativeDrop state_drop;\n"
                     "    XrBackendNativeDrop initialization_abort;\n"
                     "    XrBackendNativeInitialize initialize;\n"
                     "    XrBackendNativeModuleStep module_step;\n"
                     "    XrBackendNativeStep step; XrBackendNativeCancel cancel; "
                     "XrBackendNativeDrop drop;\n"
                     "} XrBackendNativeDescriptor;\n"
                     "#endif\n\n"
                     "static const uint8_t xr_aot_instance_layout = 0;\n"
                     "void xr_aot_instance_initialize(void *opaque) {\n"))
        return false;
    if (slots) {
        if (!append_text(buffer, "    if (!opaque) return;\n"
                                 "    XrAotModules zero = {0};\n"
                                 "    XrAotModules *modules = (XrAotModules *)opaque;\n"
                                 "    *modules = zero; modules->storage.modules = modules;\n"))
            return false;
    } else if (!append_text(buffer, "    XrAotContext zero = {0};\n"
                                    "    if (opaque) *(XrAotContext *)opaque = zero;\n"))
        return false;
    if (!append_text(buffer, "}\nvoid xr_aot_instance_abort(void *opaque) {\n"
                             "    if (!opaque) return;\n"))
        return false;
    if (slots && !append_text(buffer, "    xr_aot_modules_clear((XrAotModules *)opaque);\n"))
        return false;
    if (!slots && arena &&
        !append_text(buffer, "    xr_aot_context_destroy((XrAotContext *)opaque);\n"))
        return false;
    if (!append_text(buffer, "}\n"
                             "void xr_aot_instance_drop(void *opaque) {\n"
                             "    if (!opaque) return;\n"))
        return false;
    if (slots && !append_text(buffer, "    xr_aot_modules_destroy((XrAotModules *)opaque);\n"))
        return false;
    if (!slots && arena &&
        !append_text(buffer, "    xr_aot_context_destroy((XrAotContext *)opaque);\n"))
        return false;
    if (!append_text(
            buffer,
            "}\n"
            "size_t xr_aot_entry_coroutine_frame_size(void) { return "
            "sizeof(XrAotEntryCoroutineFrame); }\n"
            "void xr_aot_entry_coroutine_frame_initialize(void *opaque, void *state,\n"
            "                                              const XrBackendNativeHost *host) {\n"
            "    if (!opaque) return;\n"
            "    XrAotEntryCoroutineFrame zero = {0};\n"
            "    XrAotEntryCoroutineFrame *frame = (XrAotEntryCoroutineFrame *)opaque;\n"
            "    *frame = zero; frame->instance = (XrAotContext *)state;\n"))
        return false;
    if (slots && !append_text(buffer, "    frame->context.modules = (XrAotModules *)state;\n"))
        return false;
    if (function_needs_failure_storage(ir, entry) &&
        !append_text(buffer, "    frame->context.failure = &frame->failure;\n"))
        return false;
    if (!append_text(buffer,
                     "    if (host) {\n"
                     "        frame->context.provider_context = host->context;\n"
                     "        frame->context.provider_output_write = host->output_write;\n"
                     "        frame->context.provider_call_typed = host->typed_call;\n"
                     "        frame->context.provider_dispose_typed = host->typed_dispose;\n"
                     "        frame->context.resource_free = host->resource_free;\n"
                     "    }\n"
                     "}\n"
                     "void xr_aot_entry_coroutine_frame_dispose(void *opaque) {\n"
                     "    if (!opaque) return;\n"))
        return false;
    if (function_needs_failure_storage(ir, entry) &&
        !append_text(buffer,
            "    xr_aot_failure_destroy(&((XrAotEntryCoroutineFrame *)opaque)->failure);\n"))
        return false;
    if (arena &&
        !append_text(
            buffer,
            "    xr_aot_context_destroy(&((XrAotEntryCoroutineFrame *)opaque)->context);\n"))
        return false;
    if (!append_text(
            buffer,
            "    ((XrAotEntryCoroutineFrame *)opaque)->function.state = UINT32_MAX;\n"
            "}\n"
            "static XrBackendNativeOutcome xr_aot_native_outcome(XrAotOutcome native, uint32_t "
            "state) {\n"
            "    XrBackendNativeOutcome result = {0};\n"
            "    result.state_id = state;\n"
            "    result.panic_present = native.panic_present;\n"
            "    result.panic_code = native.panic_info.code;\n"
            "    result.panic_has_bounds = native.panic_info.has_bounds;\n"
            "    result.panic_index = native.panic_info.index;\n"
            "    result.panic_length = native.panic_info.length;\n"
            "    result.panic_has_message = native.panic_info.message != NULL;\n"))
        return false;
    if (has_panic_messages(ir) && !append_text(buffer,
            "    if (native.panic_info.message) {\n"
            "        result.panic_message = native.panic_info.message->bytes;\n"
            "        result.panic_message_size = native.panic_info.message->size;\n"
            "    }\n"))
        return false;
    if (!append_text(buffer,
            "    result.error_type_id = native.error_type_id;\n"
            "    result.error_value = native.error_value;\n"
            "    if (native.kind == 2) { result.kind = 6; return result; }\n"
            "    if (native.kind == 3) { result.kind = 7; return result; }\n"
            "    if (native.kind == 0) { result.kind = 0; result.value = native.i64; }\n"
            "    else if (native.kind == 5) {\n"
            "        result.kind = 1; result.safepoint_id = native.safepoint_id;\n"
            "        result.suspension.kind = native.suspension_kind;\n"
            "        result.suspension.operand_count = (uint16_t)native.suspension_operand_count;\n"
            "        result.suspension.payload.timer_after_ms = native.suspension_timer_after_ms;\n"
            "    } else if (native.kind == 6) result.kind = 3;\n"
            "    else { result.kind = 2; result.safepoint_id = native.kind == 1 ? native.trap : 4; "
            "}\n"
            "    return result;\n"
            "}\n"
            "XrBackendNativeOutcome xr_aot_module_step(void *opaque, uint32_t function, uint8_t "
            "cancel) {\n"
            "    XrBackendNativeOutcome invalid = {0}; invalid.kind = 4;\n"
            "    XrAotEntryCoroutineFrame *frame = (XrAotEntryCoroutineFrame *)opaque;\n"
            "    if (!frame || !frame->instance) return invalid;\n"
            "    (void)cancel;\n"
            "    XrAotContext *xr_ctx = &frame->context;\n"
            "    (void)xr_ctx;\n"
            "    switch (function) {\n"))
        return false;
    for (uint32_t module = 0u; module < ir->program->module_count; ++module) {
        uint32_t id = ir->program->modules[module].initializer;
        const XrValidatedFunction *fn = &ir->program->functions[id];
        if (!append_format(buffer, "        case UINT32_C(%u): {\n", id) ||
            !emit_boundary_outputs(buffer, ir, fn, true))
            return false;
        if (fn->coroutine_safepoint_count != 0u) {
            if (!append_format(buffer,
                               "            uint32_t state = frame->initializer_%u.state;\n"
                               "            XrAotOutcome result = xr_aot_fn_%u_step(xr_ctx, "
                               "&frame->initializer_%u, cancel",
                               id, id, id) ||
                !emit_boundary_outputs(buffer, ir, fn, false) || !append_text(buffer, ");\n") ||
                !emit_boundary_output_cleanup(buffer, ir, fn, true) ||
                !append_format(buffer,
                               "            return xr_aot_native_outcome(result, "
                               "cancel ? state : frame->initializer_%u.state);\n"
                               "        }\n",
                               id))
                return false;
            continue;
        }
        if (!append_text(buffer, "            if (cancel) return invalid;\n"))
            return false;
        if (!append_format(buffer, "            XrAotOutcome result = xr_aot_fn_%u(xr_ctx", id) ||
            !emit_boundary_outputs(buffer, ir, fn, false) || !append_text(buffer, ");\n") ||
            !emit_boundary_output_cleanup(buffer, ir, fn, true))
            return false;
        if (!append_text(buffer,
                         "            return xr_aot_native_outcome(result, 0);\n        }\n"))
            return false;
    }
    if (!append_text(buffer,
                     "        default: return invalid;\n"
                     "    }\n"
                     "}\n"
                     "XrBackendNativeOutcome xr_aot_entry_coroutine_step(void *opaque) {\n"
                     "    XrBackendNativeOutcome invalid = {0}; invalid.kind = 4;\n"
                     "    if (!opaque) return invalid;\n"
                     "    XrAotEntryCoroutineFrame *frame = (XrAotEntryCoroutineFrame *)opaque;\n"
                     "    if (frame->function.state == UINT32_MAX) return invalid;\n"))
        return false;
    if (!append_text(buffer, "    XrAotContext *xr_ctx = &frame->context;\n    (void)xr_ctx;\n"))
        return false;
    if (entry_is_initializer) {
        if (!append_text(buffer, "    XrAotOutcome result = xr_aot_make(0, 0, 0);\n    "
                                 "frame->function.state = UINT32_MAX;\n"))
            return false;
    } else {
        if (!emit_boundary_outputs(buffer, ir, entry, true))
            return false;
        if (coroutine) {
            if (!append_format(buffer,
                               "    XrAotOutcome result = xr_aot_fn_%u_step(xr_ctx, "
                               "&frame->function, 0",
                               ir->program->entry_function))
                return false;
        } else if (!append_format(buffer, "    XrAotOutcome result = xr_aot_fn_%u(xr_ctx",
                                  ir->program->entry_function)) {
            return false;
        }
        if (!emit_boundary_outputs(buffer, ir, entry, false) || !append_text(buffer, ");\n") ||
            !emit_boundary_output_cleanup(buffer, ir, entry, false))
            return false;
        if (!coroutine && !append_text(buffer, "    frame->function.state = UINT32_MAX;\n"))
            return false;
    }
    if (!append_text(buffer,
                     "    return xr_aot_native_outcome(result, frame->function.state);\n"
                     "}\n"
                     "XrBackendNativeOutcome xr_aot_entry_coroutine_cancel(void *opaque) {\n"
                     "    XrBackendNativeOutcome invalid = {0}; invalid.kind = 4;\n"
                     "    if (!opaque) return invalid;\n"))
        return false;
    if (coroutine) {
        if (!append_text(
                buffer,
                "    XrAotEntryCoroutineFrame *frame = (XrAotEntryCoroutineFrame *)opaque;\n"
                "    XrAotContext *xr_ctx = &frame->context;\n"
                "    uint32_t state = frame->function.state;\n") ||
            !emit_boundary_outputs(buffer, ir, entry, true) ||
            !append_format(buffer,
                           "    XrAotOutcome result = xr_aot_fn_%u_step(xr_ctx, "
                           "&frame->function, 1",
                           ir->program->entry_function) ||
            !emit_boundary_outputs(buffer, ir, entry, false) || !append_text(buffer, ");\n") ||
            !emit_boundary_output_cleanup(buffer, ir, entry, false) ||
            !append_text(buffer, "    return xr_aot_native_outcome(result, state);\n"))
            return false;
    } else if (!append_text(buffer, "    return invalid;\n")) {
        return false;
    }
    if (!append_text(buffer,
                     "}\n"
                     "const XrBackendNativeDescriptor xr_aot_entry_coroutine_descriptor = {\n"
                     "    UINT32_C(9), UINT32_C(0), {{"))
        return false;
    for (size_t index = 0u; index < sizeof(ir->execution_id.bytes); ++index)
        if (!append_format(buffer, "%sUINT8_C(%u)", index == 0u ? "" : ", ",
                           (unsigned) ir->execution_id.bytes[index]))
            return false;
    if (!append_format(
            buffer,
            "}},\n"
            "    sizeof(XrAotEntryCoroutineFrame), &xr_aot_instance_layout, sizeof(%s),\n",
            slots ? "XrAotModules" : "XrAotContext"))
        return false;
    return append_text(
        buffer, "    xr_aot_instance_initialize, xr_aot_instance_drop, xr_aot_instance_abort,\n"
                "    xr_aot_entry_coroutine_frame_initialize, xr_aot_module_step,\n"
                "    xr_aot_entry_coroutine_step, xr_aot_entry_coroutine_cancel, "
                "xr_aot_entry_coroutine_frame_dispose\n"
                "};\n\n");
}

static bool emit_module_initializers(CBuffer *buffer, const XrBackendIR *ir, bool standalone) {
    if (!standalone || ir->program->module_count == 0u)
        return true;
    if (!append_text(buffer,
                     "static XrAotOutcome xr_aot_initialization_failure(XrAotContext *xr_ctx) {\n"
                     "    XrAotOutcome result = xr_aot_make(xr_ctx->initialization_failed, 0,\n"
                     "        xr_ctx->initialization_failure_trap);\n"
                     "    result.panic_present = xr_ctx->initialization_panic_present;\n"
                     "    result.panic_info = xr_ctx->initialization_panic_info;\n"
                     "    result.error_type_id = xr_ctx->initialization_error_type;\n"
                     "    result.error_value = xr_ctx->initialization_error_value;\n"
                     "    return result;\n}\n"
                     "XrAotOutcome xr_aot_initialize_modules(XrAotContext *xr_ctx) {\n"
                     "    if (!xr_ctx) return xr_aot_make(4, 0, 0);\n"
                     "    if (xr_ctx->initialization_failed) "
                     "return xr_aot_initialization_failure(xr_ctx);\n"))
        return false;
    for (uint32_t module = 0u; module < ir->program->module_count; ++module) {
        uint32_t function_id = ir->program->modules[module].initializer;
        const XrValidatedFunction *function = &ir->program->functions[function_id];
        if (!append_format(buffer, "    if (xr_ctx->initialized_modules == UINT32_C(%u)) {\n",
                           module))
            return false;
        if (!emit_boundary_outputs(buffer, ir, function, true))
            return false;
        if (function->coroutine_safepoint_count != 0u) {
            if (!append_format(buffer,
                               "        XrAotCoroutineFrame%u frame = {0};\n"
                               "        XrAotOutcome result;\n"
                               "        for (;;) {\n"
                               "            result = xr_aot_fn_%u_step(xr_ctx, &frame, 0",
                               function_id, function_id) ||
                !emit_boundary_outputs(buffer, ir, function, false) ||
                !append_text(buffer, ");\n"
                                     "            if (result.kind != 5) break;\n"
                                     "            if (result.suspension_kind == 1 && "
                                     "result.suspension_operand_count == 0) continue;\n"))
                return false;
            if (has_timer_suspension(ir) &&
                !append_text(
                    buffer,
                    "            if (result.suspension_kind == 2 && "
                    "result.suspension_operand_count == 1) {\n"
                    "                xr_aot_host_wait_timer(result.suspension_timer_after_ms);\n"
                    "                continue;\n"
                    "            }\n"))
                return false;
            if (!append_format(buffer, "            result = xr_aot_fn_%u_step(xr_ctx, &frame, 1",
                               function_id) ||
                !emit_boundary_outputs(buffer, ir, function, false) ||
                !append_text(buffer, ");\n") ||
                !emit_boundary_output_cleanup(buffer, ir, function, true) ||
                !append_text(buffer, "            result = xr_aot_make(1, 0, 4);\n"
                                     "            break;\n"
                                     "        }\n"))
                return false;
        } else {
            if (!append_format(buffer, "        XrAotOutcome result = xr_aot_fn_%u(xr_ctx",
                               function_id))
                return false;
            if (!emit_boundary_outputs(buffer, ir, function, false) || !append_text(buffer, ");\n"))
                return false;
        }
        if (!emit_boundary_output_cleanup(buffer, ir, function, true) ||
            !append_text(
                buffer, "        if (result.kind != UINT32_C(0)) {\n"
                        "            xr_ctx->initialization_failed = "
                        "result.kind;\n"
                        "            xr_ctx->initialization_failure_trap = result.trap;\n"
                        "            xr_ctx->initialization_error_type = result.error_type_id;\n"
                        "            xr_ctx->initialization_error_value = result.error_value;\n"
                        "            xr_ctx->initialization_panic_present = result.panic_present;\n"
                        "            xr_ctx->initialization_panic_info = result.panic_info;\n") ||
            (needs_module_storage(ir) &&
             !append_text(buffer, "            xr_aot_modules_clear(xr_ctx->modules);\n")) ||
            !append_text(buffer,
                        "            return xr_aot_initialization_failure(xr_ctx);\n"
                        "        }\n"
                        "        ++xr_ctx->initialized_modules;\n"
                        "    }\n"))
            return false;
    }
    return append_text(buffer, "    return xr_aot_make(0, 0, 0);\n}\n\n");
}

static bool emit_main(CBuffer *buffer, const XrBackendIR *ir) {
    const XrValidatedFunction *entry = &ir->program->functions[ir->program->entry_function];
    bool entry_is_initializer = false;
    for (uint32_t module = 0u; module < ir->program->module_count; ++module)
        entry_is_initializer |=
            ir->program->modules[module].initializer == ir->program->entry_function;
    bool checked = false;
    bool wrapping = false;
    bool arena = false;
    bool output = false;
    scan_helpers(ir, &checked, &wrapping, &arena, &output);
    if (entry->parameter_count != 0u)
        return false;
    if (!append_text(buffer, "int main(void) {\n    XrAotContext context = {0};\n    XrAotContext *xr_ctx = &context;\n"))
        return false;
    if (needs_module_storage(ir) &&
        !append_text(buffer,
                     "    XrAotModules modules = {0};\n"
                     "    modules.storage.modules = &modules; xr_ctx->modules = &modules;\n"))
        return false;
    if (!entry_is_initializer && function_needs_failure_storage(ir, entry) &&
        !append_text(buffer,
            "    XrAotFailure entry_failure = {0}; xr_ctx->failure = &entry_failure;\n"))
        return false;
    if (output &&
        !append_text(buffer, "    xr_ctx->provider_output_write = xr_aot_host_output_write;\n"))
        return false;
    if (!emit_native_provider_main_bindings(buffer, ir))
        return false;
    if (!entry_is_initializer && !emit_boundary_outputs(buffer, ir, entry, true))
        return false;
    if (!append_text(buffer, ir->program->module_count != 0u
                                 ? "    XrAotOutcome result = xr_aot_initialize_modules(xr_ctx);\n"
                                 : "    XrAotOutcome result = xr_aot_make(0, 0, 0);\n"))
        return false;
    if (ir->program->module_count != 0u &&
        !append_text(buffer, "    if (result.kind != UINT32_C(0)) goto xr_entry_done;\n"))
        return false;
    if (!entry_is_initializer) {
        if (entry->coroutine_safepoint_count != 0u) {
            if (!append_format(buffer,
                               "    XrAotCoroutineFrame%u frame = {0};\n"
                               "    for (;;) {\n"
                               "        result = xr_aot_fn_%u_step(xr_ctx, &frame, UINT8_C(0)",
                               ir->program->entry_function, ir->program->entry_function) ||
                !emit_boundary_outputs(buffer, ir, entry, false) ||
                !append_text(buffer, ");\n"
                                     "        if (result.kind != UINT32_C(5)) break;\n"
                                     "        if (result.suspension_kind == UINT32_C(1) && "
                                     "result.suspension_operand_count == UINT32_C(0)) continue;\n"))
                return false;
            if (has_timer_suspension(ir) &&
                !append_text(
                    buffer,
                    "        if (result.suspension_kind == UINT32_C(2) && "
                    "result.suspension_operand_count == UINT32_C(1)) {\n"
                    "            xr_aot_host_wait_timer(result.suspension_timer_after_ms);\n"
                    "            continue;\n        }\n"))
                return false;
            if (!append_text(buffer, "        result = xr_aot_make(UINT32_C(4), 0, 0);\n"
                                     "        break;\n    }\n"))
                return false;
        } else if (!append_format(buffer, "    result = xr_aot_fn_%u(xr_ctx",
                                  ir->program->entry_function) ||
                   !emit_boundary_outputs(buffer, ir, entry, false) ||
                   !append_text(buffer, ");\n")) {
            return false;
        }
        if (entry->error_type_id != XR_CORE_TYPE_VOID ||
            entry->panic_type_id != XR_CORE_TYPE_VOID) {
            if (!emit_boundary_output_cleanup(buffer, ir, entry, false))
                return false;
        }
    }
    if (ir->program->module_count != 0u && !append_text(buffer, "xr_entry_done:\n    ;\n"))
        return false;
    if (!append_text(buffer, "    int exit_code = (int)(200u + result.kind * 10u + result.trap);\n"
                             "    if (result.kind == 0) {\n"))
        return false;
    switch (entry->result_type_id) {
        case XR_CORE_TYPE_I8:
            if (!append_text(buffer,
                             "        exit_code = (int)((uint64_t)result.i8 & UINT64_C(255));\n"))
                return false;
            break;
        case XR_CORE_TYPE_U8:
            if (!append_text(buffer,
                             "        exit_code = (int)((uint64_t)result.u8 & UINT64_C(255));\n"))
                return false;
            break;
        case XR_CORE_TYPE_I16:
            if (!append_text(buffer,
                             "        exit_code = (int)((uint64_t)result.i16 & UINT64_C(255));\n"))
                return false;
            break;
        case XR_CORE_TYPE_I32:
            if (!append_text(buffer,
                             "        exit_code = (int)((uint64_t)result.i32 & UINT64_C(255));\n"))
                return false;
            break;
        case XR_CORE_TYPE_U64:
            if (!append_text(buffer,
                             "        exit_code = (int)((uint64_t)result.u64 & UINT64_C(255));\n"))
                return false;
            break;

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
        case XR_CORE_TYPE_TARGET_OS:
        case XR_CORE_TYPE_TARGET_ARCH:
        case XR_CORE_TYPE_TARGET_ABI:
        case XR_CORE_TYPE_TARGET_ENDIAN:
            if (!append_text(buffer, "        exit_code = (int)(result.u16 & UINT16_C(255));\n"))
                return false;
            break;
        case XR_CORE_TYPE_ERROR:
            if (!append_text(buffer, "        exit_code = (int)(result.error & UINT32_C(255));\n"))
                return false;
            break;
        case XR_CORE_TYPE_RUNE:
            if (!append_text(buffer, "        exit_code = (int)(result.u32 & UINT32_C(255));\n"))
                return false;
            break;
        default:
            return false;
    }
    if (!append_text(buffer, "    }\n"))
        return false;
    if (has_boundary_error(ir) && !append_text(buffer,
        "    if (result.kind == UINT32_C(2)) {\n"
        "#if defined(_WIN32)\n"
        "        if (_setmode(_fileno(stderr), _O_BINARY) == -1) { exit_code = 1; }\n"
        "#endif\n"
        "        XrValueFormatReader reader = {NULL, xr_aot_format_read, xr_aot_format_child};\n"
        "        XrValueFormatNode value = {result.error_value, result.error_type_id};\n"
        "        XrValueFormatSink sink = {stderr, xr_value_format_file_write};\n"
        "        if (!xr_value_format_uncaught(reader, value, sink, 0))\n"
        "            fputs(\"XR_RUN_6005: cannot render uncaught typed error\\n\", stderr);\n"
        "        exit_code = 1;\n"
        "    }\n"))
        return false;
    if (has_boundary_panic(ir)) {
        if (!append_text(buffer,
            "    if (result.kind == UINT32_C(3)) {\n"
            "#if defined(_WIN32)\n"
            "        (void)_setmode(_fileno(stderr), _O_BINARY);\n"
            "#endif\n"
            "        (void)xr_value_format_panic((XrValueFormatSink){stderr, xr_value_format_file_write},\n"
            "            result.panic_info.code, (int)result.panic_info.has_bounds,\n"
            "            result.panic_info.index, result.panic_info.length,\n"))
            return false;
        if (!(has_panic_messages(ir) ? append_text(buffer,
            "            result.panic_info.message != NULL,\n"
            "            result.panic_info.message ? result.panic_info.message->bytes : NULL,\n"
            "            result.panic_info.message ? result.panic_info.message->size : 0);\n")
            : append_text(buffer, "            0, NULL, 0);\n")) ||
            !append_text(buffer, "        exit_code = 1;\n    }\n"))
            return false;
    }
    if (arena && !append_text(buffer, "    xr_aot_context_destroy(xr_ctx);\n"))
        return false;
    if (!entry_is_initializer && function_needs_failure_storage(ir, entry) &&
        !append_text(buffer, "    xr_aot_failure_destroy(&entry_failure);\n"))
        return false;
    if (needs_module_storage(ir) &&
        !append_text(buffer, "    xr_aot_modules_destroy(&modules);\n"))
        return false;
    return append_text(buffer, "    return exit_code;\n}\n");
}

#include "xr_backend_ir_emit_exports.inc.c"

XrBackendStatus xr_backend_ir_emit_c(const XrBackendIR *ir, bool standalone_main,
                                     XrGeneratedC *generated_out,
                                     XrBackendDiagnostic *diagnostic_out) {
    return xr_backend_ir_emit_c_exports(ir, standalone_main, NULL, 0u, generated_out, diagnostic_out);
}

XrBackendStatus xr_backend_ir_emit_c_exports(const XrBackendIR *ir, bool standalone_main,
                                             const XrBackendCExport *exports, uint32_t export_count,
                                             XrGeneratedC *generated_out,
                                             XrBackendDiagnostic *diagnostic_out) {
    if (generated_out)
        memset(generated_out, 0, sizeof(*generated_out));
    xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_OK, 0u, 0u, 0u, 0u);
    if (!ir || !ir->verified || !generated_out || !xr_backend_ir_verify(ir, diagnostic_out)) {
        if (!diagnostic_out || diagnostic_out->status == XR_BACKEND_OK)
            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVALID_INPUT, 0u, 0u, 0u, 0u);
        return diagnostic_out ? diagnostic_out->status : XR_BACKEND_INVALID_INPUT;
    }
    XrBackendStatus binding = validate_c_exports(ir, exports, export_count, diagnostic_out);
    if (binding != XR_BACKEND_OK) {
        if (!diagnostic_out || diagnostic_out->status == XR_BACKEND_OK)
            xr_backend_set_diagnostic(diagnostic_out, binding, 0u, 0u, 0u, 0u);
        return binding;
    }
    CBuffer buffer = {0};
    CBuffer header = {0};
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(ir->profile);
    if (standalone_main && machine &&
        machine->runtime_profile != XR_TARGET_RUNTIME_PROFILE_HOSTED) {
        bool checked = false;
        bool wrapping = false;
        bool arena = false;
        bool output = false;
        scan_helpers(ir, &checked, &wrapping, &arena, &output);
        if (output) {
            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_EMISSION_REJECTED, 0u, 0u, 0u, 0u);
            return XR_BACKEND_EMISSION_REJECTED;
        }
    }
    bool emitted = emit_prelude(&buffer, ir, standalone_main);
    emitted = emitted && emit_string_constant_tables(&buffer, ir);
    if (emitted && has_affine_owner_drops(ir))
        emitted = emit_owned_drop_helpers(&buffer, ir);
    emitted = emitted && emit_module_storage(&buffer, ir);
    if (emitted && standalone_main)
        emitted = emit_format_reader(&buffer, ir);
    emitted = emitted && emit_copy_helpers(&buffer, ir);
    emitted = emitted && emit_coroutine_frames(&buffer, ir);
    for (uint32_t function = 0; emitted && function < ir->program->function_count; ++function)
        emitted =
            emit_function_signature(&buffer, &ir->program->functions[function], ir, function, true);
    emitted = emitted && append_text(&buffer, "\n");
    for (uint32_t function = 0; emitted && function < ir->program->function_count; ++function)
        emitted = emit_function(&buffer, ir, function);
    emitted = emitted && emit_module_initializers(&buffer, ir, standalone_main);
    emitted = emitted && emit_coroutine_entry_adapter(&buffer, ir);
    if (emitted && standalone_main)
        emitted = emit_main(&buffer, ir);
    emitted = emitted && emit_c_exports(&buffer, &header, ir, exports, export_count);
    XiCgenVerifyResult verify = {0};
    if (!emitted || buffer.failed || !xi_cgen_verify_output(buffer.bytes, buffer.size, &verify)) {
        xr_free(buffer.bytes);
        xr_free(header.bytes);
        buffer.failed = buffer.failed || header.failed;
        xr_backend_set_diagnostic(
            diagnostic_out, buffer.failed ? XR_BACKEND_OUT_OF_MEMORY : XR_BACKEND_EMISSION_REJECTED,
            0u, 0u, 0u, 0u);
        return buffer.failed ? XR_BACKEND_OUT_OF_MEMORY : XR_BACKEND_EMISSION_REJECTED;
    }
    generated_out->bytes = buffer.bytes;
    generated_out->size = buffer.size;
    generated_out->header_bytes = header.bytes;
    generated_out->header_size = header.size;
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
    xr_free(generated->header_bytes);
    memset(generated, 0, sizeof(*generated));
}
