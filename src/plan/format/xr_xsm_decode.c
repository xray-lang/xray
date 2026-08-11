/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xsm_decode.c - Checked exact-version SemanticPlan artifact decoder
 */

#include "xr_xsm_schema.h"
#include "xr_xsm_io.h"
#include "../semantic/xr_semantic_plan_internal.h"
#include "../semantic/xr_semantic_ops.h"
#include "../semantic/xr_semantic_verify.h"
#include "../ownership/xr_ownership_certificate_internal.h"
#include "../../base/xsha256.h"
#include <stdio.h>
#include <string.h>

static const uint8_t xr_xsm_magic[8] = {'X', 'R', 'A', 'Y', 'X', 'S', 'M', 0};

typedef struct XrXsmCounts {
    uint32_t types;
    uint32_t functions;
    uint32_t blocks;
    uint32_t operations;
    uint32_t edges;
    uint32_t constants;
    uint32_t type_children;
    uint32_t parameters;
    uint32_t captures;
    uint32_t predecessors;
    uint32_t operands;
    uint32_t metadata;
    uint32_t owners;
    uint32_t events;
    uint32_t edge_states;
} XrXsmCounts;

static bool report(char *error, size_t size, const char *code, const char *detail) {
    if (error && size)
        snprintf(error, size, "%s: %s", code, detail);
    return false;
}

static char *take_string(XrXsmReader *reader, uint32_t max_length) {
    uint32_t length = xr_xsm_take_u32(reader);
    if (reader->failed || length > max_length || length > XR_XSM_MAX_STRING_SIZE ||
        reader->offset > reader->size || length > reader->size - reader->offset ||
        reader->string_bytes > XR_XSM_MAX_STRING_STORAGE ||
        length > XR_XSM_MAX_STRING_STORAGE - reader->string_bytes) {
        reader->failed = true;
        return NULL;
    }
    char *text = (char *) xr_malloc((size_t) length + 1);
    if (!text) {
        reader->failed = true;
        return NULL;
    }
    if (!xr_xsm_take_bytes(reader, text, length)) {
        xr_free(text);
        return NULL;
    }
    if (memchr(text, '\0', length) != NULL) {
        xr_free(text);
        reader->failed = true;
        return NULL;
    }
    reader->string_bytes += length;
    text[length] = '\0';
    return text;
}

static const char *take_plan_string(XrXsmReader *reader, XrSemanticPlan *plan, bool optional) {
    char *temporary = take_string(reader, 1048576u);
    if (!temporary)
        return NULL;
    if (optional && temporary[0] == '\0') {
        xr_free(temporary);
        return NULL;
    }
    char *copy = xr_semantic_plan_copy_string(plan, temporary);
    xr_free(temporary);
    if (!copy)
        reader->failed = true;
    return copy;
}

static bool checked_storage_add(size_t *total, uint32_t count, size_t element_size) {
    if (count != 0 && element_size > SIZE_MAX / count)
        return false;
    size_t bytes = (size_t) count * element_size;
    if (*total > XR_XSM_MAX_TABLE_STORAGE || bytes > XR_XSM_MAX_TABLE_STORAGE - *total)
        return false;
    *total += bytes;
    return true;
}

static bool counts_fit_storage_budget(XrXsmCounts count) {
    size_t total = 0;
#define XR_COUNT_STORAGE(field, type)                                                              \
    do {                                                                                           \
        if (!checked_storage_add(&total, count.field, sizeof(type)))                               \
            return false;                                                                          \
    } while (0)
    XR_COUNT_STORAGE(types, XrSemanticTypeRecord);
    XR_COUNT_STORAGE(functions, XrSemanticFunctionRecord);
    XR_COUNT_STORAGE(blocks, XrSemanticBlockRecord);
    XR_COUNT_STORAGE(operations, XrSemanticOperationRecord);
    XR_COUNT_STORAGE(edges, XrSemanticEdgeRecord);
    XR_COUNT_STORAGE(constants, XrSemanticConstantRecord);
    XR_COUNT_STORAGE(type_children, uint32_t);
    XR_COUNT_STORAGE(parameters, XrSemanticParameterRecord);
    XR_COUNT_STORAGE(captures, XrSemanticCaptureRecord);
    XR_COUNT_STORAGE(predecessors, uint32_t);
    XR_COUNT_STORAGE(operands, XrSemanticOperandRecord);
    XR_COUNT_STORAGE(metadata, const char *);
    XR_COUNT_STORAGE(owners, XrOwnershipOwnerRecord);
    XR_COUNT_STORAGE(events, XrOwnershipEventRecord);
    XR_COUNT_STORAGE(edge_states, XrOwnershipEdgeStateRecord);
#undef XR_COUNT_STORAGE
    return true;
}

static bool allocate_tables(XrSemanticPlan *plan, XrOwnershipCertificate *certificate,
                            XrXsmCounts count) {
    if (!counts_fit_storage_budget(count))
        return false;
#define XR_ALLOC_TABLE(field, count_value, type)                                                   \
    do {                                                                                           \
        if ((count_value) != 0) {                                                                  \
            plan->field = (type *) xr_calloc((count_value), sizeof(type));                         \
            if (!plan->field)                                                                      \
                return false;                                                                      \
        }                                                                                          \
    } while (0)
    XR_ALLOC_TABLE(types, count.types, XrSemanticTypeRecord);
    XR_ALLOC_TABLE(functions, count.functions, XrSemanticFunctionRecord);
    XR_ALLOC_TABLE(blocks, count.blocks, XrSemanticBlockRecord);
    XR_ALLOC_TABLE(operations, count.operations, XrSemanticOperationRecord);
    XR_ALLOC_TABLE(edges, count.edges, XrSemanticEdgeRecord);
    XR_ALLOC_TABLE(constants, count.constants, XrSemanticConstantRecord);
    XR_ALLOC_TABLE(type_children, count.type_children, uint32_t);
    XR_ALLOC_TABLE(parameters, count.parameters, XrSemanticParameterRecord);
    XR_ALLOC_TABLE(captures, count.captures, XrSemanticCaptureRecord);
    XR_ALLOC_TABLE(predecessors, count.predecessors, uint32_t);
    XR_ALLOC_TABLE(operands, count.operands, XrSemanticOperandRecord);
    XR_ALLOC_TABLE(metadata, count.metadata, const char *);
#undef XR_ALLOC_TABLE
#define XR_ALLOC_CERT(field, count_value, type)                                                    \
    do {                                                                                           \
        if ((count_value) != 0) {                                                                  \
            certificate->field = (type *) xr_calloc((count_value), sizeof(type));                  \
            if (!certificate->field)                                                               \
                return false;                                                                      \
        }                                                                                          \
    } while (0)
    XR_ALLOC_CERT(owners, count.owners, XrOwnershipOwnerRecord);
    XR_ALLOC_CERT(events, count.events, XrOwnershipEventRecord);
    XR_ALLOC_CERT(edge_states, count.edge_states, XrOwnershipEdgeStateRecord);
#undef XR_ALLOC_CERT
    plan->type_count = plan->type_capacity = count.types;
    plan->function_count = plan->function_capacity = count.functions;
    plan->block_count = plan->block_capacity = count.blocks;
    plan->operation_count = plan->operation_capacity = count.operations;
    plan->edge_count = plan->edge_capacity = count.edges;
    plan->constant_count = plan->constant_capacity = count.constants;
    plan->type_child_count = plan->type_child_capacity = count.type_children;
    plan->parameter_count = plan->parameter_capacity = count.parameters;
    plan->capture_count = plan->capture_capacity = count.captures;
    plan->predecessor_count = plan->predecessor_capacity = count.predecessors;
    plan->operand_count = plan->operand_capacity = count.operands;
    plan->metadata_count = plan->metadata_capacity = count.metadata;
    certificate->owner_count = certificate->owner_capacity = count.owners;
    certificate->event_count = certificate->event_capacity = count.events;
    certificate->edge_state_count = certificate->edge_state_capacity = count.edge_states;
    return true;
}

static bool take_counts(XrXsmReader *reader, XrXsmCounts *count) {
    count->types = xr_xsm_take_u32(reader);
    count->functions = xr_xsm_take_u32(reader);
    count->blocks = xr_xsm_take_u32(reader);
    count->operations = xr_xsm_take_u32(reader);
    count->edges = xr_xsm_take_u32(reader);
    count->constants = xr_xsm_take_u32(reader);
    count->type_children = xr_xsm_take_u32(reader);
    count->parameters = xr_xsm_take_u32(reader);
    count->captures = xr_xsm_take_u32(reader);
    count->predecessors = xr_xsm_take_u32(reader);
    count->operands = xr_xsm_take_u32(reader);
    count->metadata = xr_xsm_take_u32(reader);
    count->owners = xr_xsm_take_u32(reader);
    count->events = xr_xsm_take_u32(reader);
    count->edge_states = xr_xsm_take_u32(reader);
    return !reader->failed && count->types <= 1000000u && count->functions <= 100000u &&
           count->blocks <= 2000000u && count->operations <= 10000000u &&
           count->edges <= 40000000u && count->constants <= 10000000u &&
           count->type_children <= 8000000u && count->parameters <= 25600000u &&
           count->captures <= 6400000u && count->predecessors <= 16000000u &&
           count->operands <= 40000000u && count->metadata <= 80000000u &&
           count->owners <= 2000000u && count->events <= 20000000u &&
           count->edge_states <= 40000000u;
}

static void decode_types(XrXsmReader *reader, XrSemanticPlan *plan) {
    for (uint32_t i = 0; i < plan->type_count; i++) {
        XrSemanticTypeRecord *record = &plan->types[i];
        xr_xsm_take_bytes(reader, record->id.bytes, sizeof(record->id.bytes));
        record->canonical_key = take_plan_string(reader, plan, false);
        record->kind = xr_xsm_take_u32(reader);
        record->child_begin = xr_xsm_take_u32(reader);
        record->child_count = xr_xsm_take_u16(reader);
        record->scalar_rep = xr_xsm_take_u8(reader);
        record->flags = xr_xsm_take_u8(reader);
    }
    for (uint32_t i = 0; i < plan->type_child_count; i++)
        plan->type_children[i] = xr_xsm_take_u32(reader);
}

static void decode_functions(XrXsmReader *reader, XrSemanticPlan *plan) {
    for (uint32_t i = 0; i < plan->function_count; i++) {
        XrSemanticFunctionRecord *record = &plan->functions[i];
        xr_xsm_take_bytes(reader, record->id.bytes, sizeof(record->id.bytes));
        record->canonical_key = take_plan_string(reader, plan, false);
        record->name = take_plan_string(reader, plan, false);
        record->return_type = xr_xsm_take_u32(reader);
        record->parent = xr_xsm_take_u32(reader);
        record->parameter_begin = xr_xsm_take_u32(reader);
        record->parameter_count = xr_xsm_take_u16(reader);
        record->child_count = xr_xsm_take_u16(reader);
        record->capture_begin = xr_xsm_take_u32(reader);
        record->capture_count = xr_xsm_take_u16(reader);
        record->reserved = xr_xsm_take_u16(reader);
        record->block_begin = xr_xsm_take_u32(reader);
        record->block_count = xr_xsm_take_u32(reader);
        record->value_begin = xr_xsm_take_u32(reader);
        record->value_count = xr_xsm_take_u32(reader);
        record->semantic_effects = xr_xsm_take_u32(reader);
        record->capability_mask = xr_xsm_take_u32(reader);
        record->return_parameter = (int16_t) xr_xsm_take_u16(reader);
        record->return_provenance = xr_xsm_take_u8(reader);
        record->flags = xr_xsm_take_u8(reader);
    }
    for (uint32_t i = 0; i < plan->parameter_count; i++) {
        XrSemanticParameterRecord *record = &plan->parameters[i];
        xr_xsm_take_bytes(reader, record->id.bytes, sizeof(record->id.bytes));
        record->canonical_key = take_plan_string(reader, plan, false);
        record->function = xr_xsm_take_u32(reader);
        record->type = xr_xsm_take_u32(reader);
        record->value = xr_xsm_take_u32(reader);
        record->ordinal = xr_xsm_take_u16(reader);
        record->mode = xr_xsm_take_u8(reader);
        record->ownership = xr_xsm_take_u8(reader);
        record->transfer_mode = xr_xsm_take_u8(reader);
        record->flags = xr_xsm_take_u8(reader);
        record->reserved = xr_xsm_take_u16(reader);
    }
    for (uint32_t i = 0; i < plan->capture_count; i++) {
        XrSemanticCaptureRecord *record = &plan->captures[i];
        xr_xsm_take_bytes(reader, record->id.bytes, sizeof(record->id.bytes));
        record->canonical_key = take_plan_string(reader, plan, false);
        record->name = take_plan_string(reader, plan, false);
        record->function = xr_xsm_take_u32(reader);
        record->source_function = xr_xsm_take_u32(reader);
        record->source_value = xr_xsm_take_u32(reader);
        record->source_capture = xr_xsm_take_u32(reader);
        record->type = xr_xsm_take_u32(reader);
        record->source_type = xr_xsm_take_u32(reader);
        record->source_index = xr_xsm_take_u32(reader);
        record->ordinal = xr_xsm_take_u16(reader);
        record->source = xr_xsm_take_u8(reader);
        record->kind = xr_xsm_take_u8(reader);
        record->storage_domain = xr_xsm_take_u8(reader);
        record->value_capability = xr_xsm_take_u8(reader);
        record->flags = xr_xsm_take_u8(reader);
        record->reserved[0] = xr_xsm_take_u8(reader);
    }
}

static void decode_blocks(XrXsmReader *reader, XrSemanticPlan *plan) {
    for (uint32_t i = 0; i < plan->block_count; i++) {
        XrSemanticBlockRecord *record = &plan->blocks[i];
        xr_xsm_take_bytes(reader, record->id.bytes, sizeof(record->id.bytes));
        record->canonical_key = take_plan_string(reader, plan, false);
        record->function = xr_xsm_take_u32(reader);
        record->operation_begin = xr_xsm_take_u32(reader);
        record->operation_count = xr_xsm_take_u32(reader);
        record->predecessor_begin = xr_xsm_take_u32(reader);
        record->predecessor_count = xr_xsm_take_u16(reader);
        record->kind = xr_xsm_take_u16(reader);
        record->successors[0] = xr_xsm_take_u32(reader);
        record->successors[1] = xr_xsm_take_u32(reader);
        record->control_value = xr_xsm_take_u32(reader);
        record->source_line = xr_xsm_take_u32(reader);
    }
    for (uint32_t i = 0; i < plan->predecessor_count; i++)
        plan->predecessors[i] = xr_xsm_take_u32(reader);
}

static void decode_operations(XrXsmReader *reader, XrSemanticPlan *plan) {
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        XrSemanticOperationRecord *record = &plan->operations[i];
        xr_xsm_take_bytes(reader, record->id.bytes, sizeof(record->id.bytes));
        xr_xsm_take_bytes(reader, record->allocation_id.bytes, sizeof(record->allocation_id.bytes));
        record->canonical_key = take_plan_string(reader, plan, false);
        record->allocation_key = take_plan_string(reader, plan, true);
        record->function = xr_xsm_take_u32(reader);
        record->block = xr_xsm_take_u32(reader);
        record->result_value = xr_xsm_take_u32(reader);
        record->result_type = xr_xsm_take_u32(reader);
        record->operand_begin = xr_xsm_take_u32(reader);
        record->operand_count = xr_xsm_take_u16(reader);
        record->opcode = xr_xsm_take_u16(reader);
        record->metadata_begin = xr_xsm_take_u32(reader);
        record->metadata_count = xr_xsm_take_u16(reader);
        record->auxiliary_kind = xr_xsm_take_u8(reader);
        record->reserved = xr_xsm_take_u8(reader);
        record->effects = xr_xsm_take_u32(reader);
        record->source_line = xr_xsm_take_u32(reader);
        record->semantic_immediate = (int64_t) xr_xsm_take_u64(reader);
        record->constant = xr_xsm_take_u32(reader);
        for (unsigned e = 0; e < 8; e++)
            record->evidence[e] = xr_xsm_take_u32(reader);
        record->ownership_use = xr_xsm_take_u8(reader);
        record->result_ownership = xr_xsm_take_u8(reader);
        record->transfer_mode = xr_xsm_take_u8(reader);
        record->parameter_mode = xr_xsm_take_u8(reader);
        record->parameter_ownership = xr_xsm_take_u8(reader);
        record->flags = xr_xsm_take_u8(reader);
        record->result_alias_operand = (int16_t) xr_xsm_take_u16(reader);
        record->return_parameter = (int16_t) xr_xsm_take_u16(reader);
        record->return_provenance = xr_xsm_take_u8(reader);
        record->return_complete = xr_xsm_take_u8(reader);
    }
    for (uint32_t i = 0; i < plan->operand_count; i++) {
        XrSemanticOperandRecord *record = &plan->operands[i];
        record->value = xr_xsm_take_u32(reader);
        record->type = xr_xsm_take_u32(reader);
        record->parameter = (int16_t) xr_xsm_take_u16(reader);
        record->role = xr_xsm_take_u8(reader);
        record->transfer_mode = xr_xsm_take_u8(reader);
        record->ownership_action = xr_xsm_take_u8(reader);
        record->parameter_mode = xr_xsm_take_u8(reader);
        record->access = xr_xsm_take_u8(reader);
        record->origin = xr_xsm_take_u8(reader);
        record->lifetime = xr_xsm_take_u8(reader);
        record->escape = xr_xsm_take_u8(reader);
        record->flags = xr_xsm_take_u8(reader);
    }
    for (uint32_t i = 0; i < plan->metadata_count; i++)
        plan->metadata[i] = take_plan_string(reader, plan, false);
}

static void decode_constants(XrXsmReader *reader, XrSemanticPlan *plan) {
    for (uint32_t i = 0; i < plan->constant_count; i++) {
        XrSemanticConstantRecord *record = &plan->constants[i];
        record->type = xr_xsm_take_u32(reader);
        record->kind = xr_xsm_take_u8(reader);
        record->integer = (int64_t) xr_xsm_take_u64(reader);
        record->float_bits = xr_xsm_take_u64(reader);
        record->string = take_plan_string(reader, plan,
                                          record->kind != XR_SEM_CONST_STRING &&
                                              record->kind != XR_SEM_CONST_ENUM_NAMESPACE);
    }
}

static void decode_edges(XrXsmReader *reader, XrSemanticPlan *plan) {
    for (uint32_t i = 0; i < plan->edge_count; i++) {
        XrSemanticEdgeRecord *record = &plan->edges[i];
        xr_xsm_take_bytes(reader, record->id.bytes, sizeof(record->id.bytes));
        record->canonical_key = take_plan_string(reader, plan, false);
        record->function = xr_xsm_take_u32(reader);
        record->from_block = xr_xsm_take_u32(reader);
        record->to_block = xr_xsm_take_u32(reader);
        record->operation = xr_xsm_take_u32(reader);
        record->kind = xr_xsm_take_u8(reader);
        record->flags = xr_xsm_take_u8(reader);
        record->reserved = xr_xsm_take_u16(reader);
    }
}

static void decode_ownership(XrXsmReader *reader, XrOwnershipCertificate *certificate) {
    for (uint32_t i = 0; i < certificate->owner_count; i++) {
        XrOwnershipOwnerRecord *record = &certificate->owners[i];
        xr_xsm_take_bytes(reader, record->id.bytes, sizeof(record->id.bytes));
        record->canonical_key = take_string(reader, 1048576u);
        record->function = xr_xsm_take_u32(reader);
        record->origin_value = xr_xsm_take_u32(reader);
        record->initial_state = xr_xsm_take_u8(reader);
        record->exit_state = xr_xsm_take_u8(reader);
        record->return_provenance = xr_xsm_take_u8(reader);
        record->flags = xr_xsm_take_u8(reader);
    }
    for (uint32_t i = 0; i < certificate->event_count; i++) {
        XrOwnershipEventRecord *record = &certificate->events[i];
        xr_xsm_take_bytes(reader, record->id.bytes, sizeof(record->id.bytes));
        record->canonical_key = take_string(reader, 1048576u);
        record->owner = xr_xsm_take_u32(reader);
        record->operation = xr_xsm_take_u32(reader);
        record->block = xr_xsm_take_u32(reader);
        record->successor = xr_xsm_take_u32(reader);
        record->logical_delta = (int16_t) xr_xsm_take_u16(reader);
        record->kind = xr_xsm_take_u8(reader);
        record->state_after = xr_xsm_take_u8(reader);
    }
    for (uint32_t i = 0; i < certificate->edge_state_count; i++) {
        XrOwnershipEdgeStateRecord *record = &certificate->edge_states[i];
        record->owner = xr_xsm_take_u32(reader);
        record->block = xr_xsm_take_u32(reader);
        record->successor = xr_xsm_take_u32(reader);
        record->entry_balance = (int32_t) xr_xsm_take_u32(reader);
        record->exit_balance = (int32_t) xr_xsm_take_u32(reader);
        record->entry_state = xr_xsm_take_u8(reader);
        record->exit_state = xr_xsm_take_u8(reader);
        record->flags = xr_xsm_take_u16(reader);
    }
}

bool xr_xsm_decode(const uint8_t *bytes, size_t size, XrSemanticPlan **out, char *error,
                   size_t error_size) {
    if (out)
        *out = NULL;
    if (!bytes || !out || size < XR_XSM_HEADER_SIZE)
        return report(error, error_size, "XR_ARTIFACT_2001", "XSM header is truncated");
    if (size > XR_XSM_MAX_ARTIFACT_SIZE)
        return report(error, error_size, "XR_EXEC_5003", "XSM artifact exceeds its hard budget");
    XrXsmReader header = {.data = bytes, .size = size};
    uint8_t magic[8], digest[32];
    XrFingerprint expected_registry_fingerprint, expected_fingerprint;
    xr_xsm_take_bytes(&header, magic, sizeof(magic));
    uint32_t schema = xr_xsm_take_u32(&header);
    uint32_t header_size = xr_xsm_take_u32(&header);
    uint64_t payload_size = xr_xsm_take_u64(&header);
    xr_xsm_take_bytes(&header, digest, sizeof(digest));
    xr_xsm_take_bytes(&header, expected_registry_fingerprint.bytes,
                      sizeof(expected_registry_fingerprint.bytes));
    xr_xsm_take_bytes(&header, expected_fingerprint.bytes, sizeof(expected_fingerprint.bytes));
    if (header.failed || memcmp(magic, xr_xsm_magic, sizeof(magic)) != 0 ||
        schema != XR_SEMANTIC_SCHEMA_VERSION || header_size != XR_XSM_HEADER_SIZE)
        return report(error, error_size, "XR_ARTIFACT_2000", "XSM schema is not exactly supported");
    if (payload_size != size - XR_XSM_HEADER_SIZE)
        return report(error, error_size, "XR_ARTIFACT_2001", "XSM payload bounds are invalid");
    XrFingerprint current_registry_fingerprint;
    xr_semantic_op_registry_fingerprint(&current_registry_fingerprint);
    if (!xr_fingerprint_equal(expected_registry_fingerprint, current_registry_fingerprint))
        return report(error, error_size, "XR_ARTIFACT_2003",
                      "XSM operation registry fingerprint is incompatible");
    uint8_t actual_digest[32];
    xr_sha256(bytes + XR_XSM_HEADER_SIZE, (size_t) payload_size, actual_digest);
    if (memcmp(digest, actual_digest, sizeof(digest)) != 0)
        return report(error, error_size, "XR_ARTIFACT_2002", "XSM payload digest is invalid");

    XrSemanticPlan *plan = xr_semantic_plan_create();
    XrOwnershipCertificate *certificate =
        (XrOwnershipCertificate *) xr_calloc(1, sizeof(*certificate));
    if (!plan || !certificate) {
        xr_semantic_plan_free(plan);
        xr_ownership_certificate_free(certificate);
        return report(error, error_size, "XR_EXEC_5003", "XSM decoder allocation failed");
    }
    certificate->schema = schema;
    XrXsmReader reader = {
        .data = bytes + XR_XSM_HEADER_SIZE,
        .size = (size_t) payload_size,
    };
    XrXsmCounts count;
    if (!take_counts(&reader, &count) || !allocate_tables(plan, certificate, count)) {
        xr_semantic_plan_free(plan);
        xr_ownership_certificate_free(certificate);
        return report(error, error_size, "XR_EXEC_5003", "XSM table budget is invalid");
    }
    decode_types(&reader, plan);
    decode_functions(&reader, plan);
    decode_blocks(&reader, plan);
    decode_operations(&reader, plan);
    decode_edges(&reader, plan);
    decode_constants(&reader, plan);
    decode_ownership(&reader, certificate);
    if (reader.failed || reader.offset != reader.size) {
        xr_semantic_plan_free(plan);
        xr_ownership_certificate_free(certificate);
        return report(error, error_size, "XR_ARTIFACT_2001", "XSM record bounds are invalid");
    }
    xr_semantic_plan_set_ownership(plan, certificate);
    if (!xr_semantic_plan_freeze(plan, error, error_size) ||
        !xr_fingerprint_equal(plan->fingerprint, expected_fingerprint) ||
        !xr_semantic_plan_verify(plan, error, error_size)) {
        if (error && error_size && error[0] == '\0')
            snprintf(error, error_size, "XR_ARTIFACT_2002: XSM semantic fingerprint is invalid");
        xr_semantic_plan_free(plan);
        return false;
    }
    plan->verified = true;
    *out = plan;
    return true;
}
