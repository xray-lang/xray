/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_encode.c - Canonical compact XrProgram writer
 */

#include "xr_program_internal.h"

#include "../base/xmalloc.h"
#include "../core/xr_core_spec_gen.h"
#include "xr_program_schema_gen.h"

#include <stdlib.h>
#include <string.h>

typedef struct ByteBuffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    XrProgramBuildStatus status;
} ByteBuffer;

typedef struct FunctionRef {
    const XrCoreIrFunction *function;
} FunctionRef;

typedef struct ConstantRef {
    const XrCoreIrConstantInput *constant;
} ConstantRef;

static bool buffer_reserve(ByteBuffer *buffer, size_t extra) {
    if (buffer->status != XR_PROGRAM_BUILD_OK)
        return false;
    if (extra > XR_PROGRAM_LIMIT_ARTIFACT_BYTES ||
        buffer->size > XR_PROGRAM_LIMIT_ARTIFACT_BYTES - extra) {
        buffer->status = XR_PROGRAM_BUILD_RESOURCE_LIMIT;
        return false;
    }
    size_t required = buffer->size + extra;
    if (required <= buffer->capacity)
        return true;
    size_t capacity = buffer->capacity ? buffer->capacity : 256u;
    while (capacity < required) {
        if (capacity > XR_PROGRAM_LIMIT_ARTIFACT_BYTES / 2u) {
            capacity = XR_PROGRAM_LIMIT_ARTIFACT_BYTES;
            break;
        }
        capacity *= 2u;
    }
    uint8_t *grown = xr_realloc(buffer->data, capacity);
    if (!grown) {
        buffer->status = XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        return false;
    }
    buffer->data = grown;
    buffer->capacity = capacity;
    return true;
}

static void buffer_put_bytes(ByteBuffer *buffer, const void *bytes, size_t size) {
    if (!buffer_reserve(buffer, size))
        return;
    memcpy(buffer->data + buffer->size, bytes, size);
    buffer->size += size;
}

static void buffer_put_u16(ByteBuffer *buffer, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t) value, (uint8_t) (value >> 8u)};
    buffer_put_bytes(buffer, bytes, sizeof(bytes));
}

static void buffer_put_uvar(ByteBuffer *buffer, uint64_t value) {
    do {
        uint8_t byte = (uint8_t) (value & UINT64_C(0x7f));
        value >>= 7u;
        if (value != 0)
            byte |= UINT8_C(0x80);
        buffer_put_bytes(buffer, &byte, 1u);
    } while (value != 0 && buffer->status == XR_PROGRAM_BUILD_OK);
}

static void buffer_put_svar(ByteBuffer *buffer, int64_t value) {
    uint64_t zigzag = value >= 0 ? (uint64_t) value * UINT64_C(2)
                                 : (uint64_t) (-(value + 1)) * UINT64_C(2) + UINT64_C(1);
    buffer_put_uvar(buffer, zigzag);
}

static void buffer_destroy(ByteBuffer *buffer) {
    xr_free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static int function_ref_compare(const void *left, const void *right) {
    const FunctionRef *a = left;
    const FunctionRef *b = right;
    return memcmp(a->function->key.bytes, b->function->key.bytes, XR_CORE_IR_KEY_SIZE);
}

static int constant_value_compare(const XrCoreIrConstantInput *left,
                                  const XrCoreIrConstantInput *right) {
    if (left->type_id != right->type_id)
        return left->type_id < right->type_id ? -1 : 1;
    if (left->kind != right->kind)
        return left->kind < right->kind ? -1 : 1;
    if (left->kind == XR_CORE_IR_CONSTANT_I64 && left->value.i64 != right->value.i64)
        return left->value.i64 < right->value.i64 ? -1 : 1;
    if (left->kind == XR_CORE_IR_CONSTANT_BOOL && left->value.boolean != right->value.boolean)
        return left->value.boolean ? 1 : -1;
    return 0;
}

static int constant_ref_compare(const void *left, const void *right) {
    const ConstantRef *a = left;
    const ConstantRef *b = right;
    int value_order = constant_value_compare(a->constant, b->constant);
    if (value_order != 0)
        return value_order;
    return memcmp(a->constant->key.bytes, b->constant->key.bytes, XR_CORE_IR_KEY_SIZE);
}

static FunctionRef *collect_functions(const XrCoreIrProgram *program, uint32_t *count_out) {
    uint32_t count = 0;
    for (uint32_t module = 0; module < program->module_count; ++module)
        count += program->modules[module].function_count;
    FunctionRef *refs = xr_calloc(count, sizeof(FunctionRef));
    if (!refs)
        return NULL;
    uint32_t cursor = 0;
    for (uint32_t module = 0; module < program->module_count; ++module) {
        for (uint32_t function = 0; function < program->modules[module].function_count; ++function)
            refs[cursor++].function = &program->modules[module].functions[function];
    }
    qsort(refs, count, sizeof(FunctionRef), function_ref_compare);
    *count_out = count;
    return refs;
}

static ConstantRef *collect_constants(const XrCoreIrProgram *program, uint32_t *count_out) {
    uint32_t count = 0;
    for (uint32_t module = 0; module < program->module_count; ++module)
        count += program->modules[module].constant_count;
    if (count == 0) {
        *count_out = 0;
        return (ConstantRef *) xr_calloc(1u, sizeof(ConstantRef));
    }
    ConstantRef *refs = xr_calloc(count, sizeof(ConstantRef));
    if (!refs)
        return NULL;
    uint32_t cursor = 0;
    for (uint32_t module = 0; module < program->module_count; ++module) {
        for (uint32_t constant = 0; constant < program->modules[module].constant_count; ++constant)
            refs[cursor++].constant = &program->modules[module].constants[constant];
    }
    qsort(refs, count, sizeof(ConstantRef), constant_ref_compare);
    *count_out = count;
    return refs;
}

static uint32_t canonical_constant_count(const ConstantRef *constants, uint32_t count) {
    uint32_t unique = 0;
    for (uint32_t index = 0; index < count; ++index) {
        if (index == 0 ||
            constant_value_compare(constants[index - 1].constant, constants[index].constant) != 0)
            ++unique;
    }
    return unique;
}

static bool constant_id(const ConstantRef *constants, uint32_t count, XrCoreIrKey key,
                        uint32_t *id_out) {
    uint32_t id = 0;
    for (uint32_t index = 0; index < count; ++index) {
        if (index != 0 &&
            constant_value_compare(constants[index - 1].constant, constants[index].constant) != 0)
            ++id;
        if (xr_core_ir_key_equal(constants[index].constant->key, key)) {
            *id_out = id;
            return true;
        }
    }
    return false;
}

static bool function_id(const FunctionRef *functions, uint32_t count, XrCoreIrKey key,
                        uint32_t *id_out) {
    for (uint32_t index = 0; index < count; ++index) {
        if (xr_core_ir_key_equal(functions[index].function->key, key)) {
            *id_out = index;
            return true;
        }
    }
    return false;
}

static bool block_id(const XrCoreIrFunction *function, XrCoreIrKey key, uint32_t *id_out) {
    for (uint32_t index = 0; index < function->block_count; ++index) {
        if (xr_core_ir_key_equal(function->blocks[index].key, key)) {
            *id_out = index;
            return true;
        }
    }
    return false;
}

static bool value_id(const XrCoreIrFunction *function, XrCoreIrKey key, uint32_t *id_out) {
    uint32_t value = 0;
    for (uint32_t block_index = 0; block_index < function->block_count; ++block_index) {
        const XrCoreIrBlock *block = &function->blocks[block_index];
        for (uint32_t index = 0; index < block->argument_count; ++index, ++value) {
            if (xr_core_ir_key_equal(block->arguments[index].key, key)) {
                *id_out = value;
                return true;
            }
        }
        for (uint32_t index = 0; index < block->instruction_count; ++index) {
            if (xr_core_ir_key_is_zero(block->instructions[index].result))
                continue;
            if (xr_core_ir_key_equal(block->instructions[index].result, key)) {
                *id_out = value;
                return true;
            }
            ++value;
        }
    }
    return false;
}

static uint32_t function_value_count(const XrCoreIrFunction *function) {
    uint32_t count = 0;
    for (uint32_t block_index = 0; block_index < function->block_count; ++block_index) {
        const XrCoreIrBlock *block = &function->blocks[block_index];
        count += block->argument_count;
        for (uint32_t index = 0; index < block->instruction_count; ++index)
            count += !xr_core_ir_key_is_zero(block->instructions[index].result);
    }
    return count;
}

static void encode_types(ByteBuffer *buffer, const XrCoreIrProgram *program) {
    /* type-id, kind, logical ownership, copy contract, then logical shape */
    static const uint8_t rows[][4] = {
        {XR_CORE_TYPE_VOID, 0u, XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL, XR_CORE_IR_COPY_FORBIDDEN},
        {XR_CORE_TYPE_BOOL, 1u, XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL, XR_CORE_IR_COPY_TRIVIAL},
        {XR_CORE_TYPE_I64, 2u, XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL, XR_CORE_IR_COPY_TRIVIAL},
        {XR_CORE_TYPE_U32, 3u, XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL, XR_CORE_IR_COPY_TRIVIAL},
        {XR_CORE_TYPE_ERROR, 4u, XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL, XR_CORE_IR_COPY_TRIVIAL},
        {XR_CORE_TYPE_PANIC_INFO, 5u, XR_CORE_IR_TYPE_OWNERSHIP_AFFINE, XR_CORE_IR_COPY_FORBIDDEN},
    };
    buffer_put_uvar(buffer, sizeof(rows) / sizeof(rows[0]) + program->type_count);
    for (size_t index = 0; index < sizeof(rows) / sizeof(rows[0]); ++index) {
        buffer_put_uvar(buffer, rows[index][0]);
        buffer_put_uvar(buffer, rows[index][1]);
        buffer_put_uvar(buffer, rows[index][2]);
        buffer_put_uvar(buffer, rows[index][3]);
    }
    for (uint32_t index = 0; index < program->type_count; ++index) {
        const XrCoreIrType *type = &program->types[index];
        buffer_put_uvar(buffer, type->type_id);
        buffer_put_uvar(buffer, type->kind == XR_CORE_IR_TYPE_AGGREGATE
                                    ? XR_PROGRAM_TYPE_KIND_AGGREGATE
                                    : XR_PROGRAM_TYPE_KIND_VARIANT);
        buffer_put_uvar(buffer, type->ownership);
        buffer_put_uvar(buffer, type->copy_contract);
        buffer_put_bytes(buffer, type->key.bytes, sizeof(type->key.bytes));
        if (type->kind == XR_CORE_IR_TYPE_AGGREGATE) {
            buffer_put_uvar(buffer, type->field_count);
            for (uint32_t field = 0; field < type->field_count; ++field)
                buffer_put_uvar(buffer, type->field_types[field]);
        } else {
            buffer_put_uvar(buffer, type->variant_count);
            for (uint32_t variant = 0; variant < type->variant_count; ++variant) {
                const XrCoreIrVariant *row = &type->variants[variant];
                buffer_put_uvar(buffer, row->payload_count);
                for (uint32_t field = 0; field < row->payload_count; ++field)
                    buffer_put_uvar(buffer, row->payload_types[field]);
            }
        }
    }
}

static void encode_constants(ByteBuffer *buffer, const ConstantRef *constants, uint32_t count) {
    uint32_t unique = canonical_constant_count(constants, count);
    buffer_put_uvar(buffer, unique);
    uint32_t id = 0;
    for (uint32_t index = 0; index < count; ++index) {
        if (index != 0 &&
            constant_value_compare(constants[index - 1].constant, constants[index].constant) == 0)
            continue;
        const XrCoreIrConstantInput *constant = constants[index].constant;
        buffer_put_uvar(buffer, id++);
        buffer_put_uvar(buffer, constant->type_id);
        buffer_put_uvar(buffer, constant->kind);
        if (constant->kind == XR_CORE_IR_CONSTANT_I64)
            buffer_put_svar(buffer, constant->value.i64);
        else
            buffer_put_uvar(buffer, constant->value.boolean ? 1u : 0u);
    }
}

static void encode_functions(ByteBuffer *buffer, const FunctionRef *functions, uint32_t count) {
    buffer_put_uvar(buffer, count);
    for (uint32_t id = 0; id < count; ++id) {
        const XrCoreIrFunction *function = functions[id].function;
        uint32_t entry = 0;
        (void) block_id(function, function->entry_block, &entry);
        buffer_put_uvar(buffer, id);
        buffer_put_uvar(buffer, function->parameter_count);
        for (uint32_t parameter = 0; parameter < function->parameter_count; ++parameter) {
            buffer_put_uvar(buffer, function->parameter_types[parameter]);
            buffer_put_uvar(buffer, function->parameter_modes[parameter]);
        }
        buffer_put_uvar(buffer, function->result_type_id);
        buffer_put_uvar(buffer, function->result_ownership);
        buffer_put_uvar(buffer, function->error_type_id);
        buffer_put_uvar(buffer, function->panic_type_id);
        buffer_put_uvar(buffer, function->effect_mask);
        buffer_put_uvar(buffer, function->capability_mask);
        buffer_put_uvar(buffer, entry);
        buffer_put_uvar(buffer, function->block_count);
        buffer_put_uvar(buffer, function_value_count(function));
        buffer_put_uvar(buffer, function->flags);
    }
}

static void encode_instruction(ByteBuffer *buffer, const XrCoreIrInstruction *instruction,
                               const XrCoreIrFunction *function, const FunctionRef *functions,
                               uint32_t function_count, const ConstantRef *constants,
                               uint32_t constant_count) {
    uint32_t id = 0;
    buffer_put_uvar(buffer, instruction->operation_id);
    if (xr_core_ir_key_is_zero(instruction->result)) {
        buffer_put_uvar(buffer, 0u);
    } else {
        (void) value_id(function, instruction->result, &id);
        buffer_put_uvar(buffer, (uint64_t) id + 1u);
    }
    buffer_put_uvar(buffer, instruction->result_type_id);
    buffer_put_uvar(buffer, instruction->result_category);
    buffer_put_uvar(buffer, instruction->result_ownership);
    buffer_put_uvar(buffer, instruction->operand_count);
    for (uint32_t index = 0; index < instruction->operand_count; ++index) {
        (void) value_id(function, instruction->operands[index], &id);
        buffer_put_uvar(buffer, id);
    }
    buffer_put_uvar(buffer, instruction->immediate_kind);
    switch (instruction->immediate_kind) {
        case XR_CORE_IR_IMMEDIATE_NONE:
            break;
        case XR_CORE_IR_IMMEDIATE_I64:
            buffer_put_svar(buffer, instruction->immediate.i64);
            break;
        case XR_CORE_IR_IMMEDIATE_U32:
            buffer_put_uvar(buffer, instruction->immediate.u32);
            break;
        case XR_CORE_IR_IMMEDIATE_BOOL:
            buffer_put_uvar(buffer, instruction->immediate.boolean ? 1u : 0u);
            break;
        case XR_CORE_IR_IMMEDIATE_CONSTANT:
            (void) constant_id(constants, constant_count, instruction->immediate.key, &id);
            buffer_put_uvar(buffer, id);
            break;
        case XR_CORE_IR_IMMEDIATE_FUNCTION:
            (void) function_id(functions, function_count, instruction->immediate.key, &id);
            buffer_put_uvar(buffer, id);
            break;
        case XR_CORE_IR_IMMEDIATE_FIELD:
            buffer_put_uvar(buffer, instruction->immediate.field_ordinal);
            break;
        case XR_CORE_IR_IMMEDIATE_VARIANT:
            buffer_put_uvar(buffer, instruction->immediate.variant_ordinal);
            break;
        case XR_CORE_IR_IMMEDIATE_VARIANT_FIELD:
            buffer_put_uvar(buffer, instruction->immediate.variant_field.variant_ordinal);
            buffer_put_uvar(buffer, instruction->immediate.variant_field.field_ordinal);
            break;
        default:
            buffer->status = XR_PROGRAM_BUILD_INVALID_INPUT;
            return;
    }
    buffer_put_uvar(buffer, instruction->successor_count);
    for (uint32_t index = 0; index < instruction->successor_count; ++index) {
        (void) block_id(function, instruction->successors[index], &id);
        buffer_put_uvar(buffer, id);
    }
}

static void encode_code(ByteBuffer *buffer, const FunctionRef *functions, uint32_t function_count,
                        const ConstantRef *constants, uint32_t constant_count) {
    buffer_put_uvar(buffer, function_count);
    for (uint32_t function_id_value = 0; function_id_value < function_count; ++function_id_value) {
        const XrCoreIrFunction *function = functions[function_id_value].function;
        buffer_put_uvar(buffer, function_id_value);
        buffer_put_uvar(buffer, function->block_count);
        for (uint32_t block_id_value = 0; block_id_value < function->block_count;
             ++block_id_value) {
            const XrCoreIrBlock *block = &function->blocks[block_id_value];
            buffer_put_uvar(buffer, block_id_value);
            buffer_put_uvar(buffer, block->argument_count);
            for (uint32_t argument = 0; argument < block->argument_count; ++argument) {
                uint32_t value = 0;
                (void) value_id(function, block->arguments[argument].key, &value);
                buffer_put_uvar(buffer, value);
                buffer_put_uvar(buffer, block->arguments[argument].type_id);
                buffer_put_uvar(buffer, block->arguments[argument].category);
                buffer_put_uvar(buffer, block->arguments[argument].ownership);
            }
            buffer_put_uvar(buffer, block->instruction_count);
            for (uint32_t instruction = 0; instruction < block->instruction_count; ++instruction)
                encode_instruction(buffer, &block->instructions[instruction], function, functions,
                                   function_count, constants, constant_count);
        }
    }
}

static bool parse_hex_digest(const char *hex, uint8_t digest[XR_PROGRAM_DIGEST_SIZE]) {
    for (size_t index = 0; index < XR_PROGRAM_DIGEST_SIZE; ++index) {
        unsigned high = (unsigned) (hex[index * 2u] >= 'a' ? hex[index * 2u] - 'a' + 10
                                                           : hex[index * 2u] - '0');
        unsigned low = (unsigned) (hex[index * 2u + 1u] >= 'a' ? hex[index * 2u + 1u] - 'a' + 10
                                                               : hex[index * 2u + 1u] - '0');
        if (high > 15u || low > 15u)
            return false;
        digest[index] = (uint8_t) ((high << 4u) | low);
    }
    return hex[(size_t) XR_PROGRAM_DIGEST_SIZE * 2u] == '\0';
}

void xr_program_id_hex(XrProgramId id, char output[XR_PROGRAM_DIGEST_SIZE * 2u + 1u]) {
    static const char hex[] = "0123456789abcdef";
    if (!output)
        return;
    for (size_t index = 0; index < XR_PROGRAM_DIGEST_SIZE; ++index) {
        output[index * 2u] = hex[id.bytes[index] >> 4u];
        output[index * 2u + 1u] = hex[id.bytes[index] & UINT8_C(0x0f)];
    }
    output[(size_t) XR_PROGRAM_DIGEST_SIZE * 2u] = '\0';
}

XrProgramBuildStatus xr_program_write(const XrCoreIrProgram *program,
                                      XrProgramArtifact *artifact_out, char *diagnostic,
                                      size_t diagnostic_size) {
    static const uint8_t magic[XR_PROGRAM_MAGIC_SIZE] = XR_PROGRAM_MAGIC_BYTES;
    enum {
        SECTION_COUNT = XR_PROGRAM_REQUIRED_SECTION_COUNT
    };
    ByteBuffer sections[SECTION_COUNT] = {{0}};
    ByteBuffer artifact = {0};
    FunctionRef *functions = NULL;
    ConstantRef *constants = NULL;
    uint32_t function_count = 0;
    uint32_t constant_count = 0;
    uint8_t core_spec_fingerprint[XR_PROGRAM_DIGEST_SIZE] = {0};
    XrProgramBuildStatus status = XR_PROGRAM_BUILD_OK;

    if (artifact_out)
        memset(artifact_out, 0, sizeof(*artifact_out));
    if (diagnostic && diagnostic_size != 0)
        diagnostic[0] = '\0';
    if (!program || !artifact_out ||
        !parse_hex_digest(XR_CORE_SPEC_SEMANTIC_SHA256, core_spec_fingerprint)) {
        xr_program_set_diagnostic(diagnostic, diagnostic_size, "program writer input is invalid");
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    }
    functions = collect_functions(program, &function_count);
    constants = collect_constants(program, &constant_count);
    if (!functions || !constants) {
        status = XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        goto cleanup;
    }

    encode_types(&sections[0], program);
    encode_constants(&sections[1], constants, constant_count);
    encode_functions(&sections[2], functions, function_count);
    encode_code(&sections[3], functions, function_count, constants, constant_count);
    buffer_put_uvar(&sections[4], 0u);
    buffer_put_uvar(&sections[5], 0u);
    buffer_put_uvar(&sections[6], 0u);
    for (size_t index = 0; index < SECTION_COUNT; ++index) {
        if (sections[index].status != XR_PROGRAM_BUILD_OK) {
            status = sections[index].status;
            goto cleanup;
        }
    }

    buffer_put_bytes(&artifact, magic, sizeof(magic));
    buffer_put_u16(&artifact, XR_PROGRAM_FORMAT_MAJOR);
    buffer_put_u16(&artifact, XR_PROGRAM_FORMAT_MINOR);
    buffer_put_uvar(&artifact, XR_CORE_SPEC_EPOCH);
    buffer_put_bytes(&artifact, core_spec_fingerprint, sizeof(core_spec_fingerprint));
    buffer_put_bytes(&artifact, program->semantic_profile_fingerprint,
                     sizeof(program->semantic_profile_fingerprint));
    buffer_put_uvar(&artifact, program->required_feature_count);
    for (uint32_t index = 0; index < program->required_feature_count; ++index)
        buffer_put_uvar(&artifact, program->required_features[index]);
    buffer_put_uvar(&artifact, SECTION_COUNT);
    uint64_t payload_offset = 0;
    for (uint32_t index = 0; index < SECTION_COUNT; ++index) {
        buffer_put_uvar(&artifact, index + 1u);
        buffer_put_uvar(&artifact, payload_offset);
        buffer_put_uvar(&artifact, sections[index].size);
        payload_offset += sections[index].size;
    }
    for (size_t index = 0; index < SECTION_COUNT; ++index)
        buffer_put_bytes(&artifact, sections[index].data, sections[index].size);
    if (artifact.status != XR_PROGRAM_BUILD_OK) {
        status = artifact.status;
        goto cleanup;
    }
    artifact_out->bytes = artifact.data;
    artifact_out->size = artifact.size;
    xr_program_compute_id(artifact_out->bytes, artifact_out->size, &artifact_out->id);
    artifact.data = NULL;

cleanup:
    if (status != XR_PROGRAM_BUILD_OK)
        xr_program_set_diagnostic(diagnostic, diagnostic_size, "program write failed: %s",
                                  xr_program_build_status_name(status));
    xr_free(constants);
    xr_free(functions);
    for (size_t index = 0; index < SECTION_COUNT; ++index)
        buffer_destroy(&sections[index]);
    buffer_destroy(&artifact);
    return status;
}

void xr_program_artifact_free(XrProgramArtifact *artifact) {
    if (!artifact)
        return;
    xr_free(artifact->bytes);
    memset(artifact, 0, sizeof(*artifact));
}
