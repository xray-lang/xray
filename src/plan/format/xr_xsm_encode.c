/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xsm_encode.c - Exact-version SemanticPlan artifact encoder
 */

#include "xr_xsm_schema.h"
#include "xr_xsm_io.h"
#include "xr_artifact_kind.h"
#include "../semantic/xr_semantic_plan_internal.h"
#include "../semantic/xr_semantic_verify.h"
#include "../ownership/xr_ownership_certificate_internal.h"
#include "../../base/xsha256.h"
#include <stdio.h>
#include <string.h>

static void encode_counts(XrXsmWriter *writer, const XrSemanticPlan *plan) {
    xr_xsm_put_u32(writer, plan->type_count);
    xr_xsm_put_u32(writer, plan->source_class_count);
    xr_xsm_put_u32(writer, plan->source_method_count);
    xr_xsm_put_u32(writer, plan->function_count);
    xr_xsm_put_u32(writer, plan->block_count);
    xr_xsm_put_u32(writer, plan->operation_count);
    xr_xsm_put_u32(writer, plan->call_target_count);
    xr_xsm_put_u32(writer, plan->dependency_count);
    xr_xsm_put_u32(writer, plan->source_export_count);
    xr_xsm_put_u32(writer, plan->edge_count);
    xr_xsm_put_u32(writer, plan->constant_count);
    xr_xsm_put_u32(writer, plan->entity_count);
    xr_xsm_put_u32(writer, plan->type_child_count);
    xr_xsm_put_u32(writer, plan->parameter_count);
    xr_xsm_put_u32(writer, plan->capture_count);
    xr_xsm_put_u32(writer, plan->predecessor_count);
    xr_xsm_put_u32(writer, plan->operand_count);
    xr_xsm_put_u32(writer, plan->metadata_count);
    xr_xsm_put_u32(writer, plan->ownership->owner_count);
    xr_xsm_put_u32(writer, plan->ownership->event_count);
    xr_xsm_put_u32(writer, plan->ownership->edge_state_count);
    xr_xsm_put_u32(writer, plan->ownership->loop_invariant_count);
}

static void encode_source_methods(XrXsmWriter *writer, const XrSemanticPlan *plan) {
    for (uint32_t i = 0; i < plan->source_method_count; i++) {
        const XrSemanticSourceMethodRecord *record = &plan->source_methods[i];
        xr_xsm_put_bytes(writer, record->id.bytes, sizeof(record->id.bytes));
        xr_xsm_put_string(writer, record->canonical_key);
        xr_xsm_put_string(writer, record->name);
        xr_xsm_put_u32(writer, record->source_class);
        xr_xsm_put_u32(writer, record->function);
        xr_xsm_put_u16(writer, record->member_ordinal);
        xr_xsm_put_u16(writer, record->parameter_count);
        xr_xsm_put_u8(writer, record->flags);
        xr_xsm_put_u8(writer, 0);
        xr_xsm_put_u8(writer, 0);
        xr_xsm_put_u8(writer, 0);
    }
}

static void encode_source_classes(XrXsmWriter *writer, const XrSemanticPlan *plan) {
    for (uint32_t i = 0; i < plan->source_class_count; i++) {
        const XrSemanticSourceClassRecord *record = &plan->source_classes[i];
        xr_xsm_put_bytes(writer, record->id.bytes, sizeof(record->id.bytes));
        xr_xsm_put_string(writer, record->canonical_key);
        xr_xsm_put_bytes(writer, record->module.bytes, sizeof(record->module.bytes));
        xr_xsm_put_string(writer, record->module_path);
        xr_xsm_put_string(writer, record->name);
        xr_xsm_put_u32(writer, record->ordinal);
        xr_xsm_put_u16(writer, record->method_count);
        xr_xsm_put_u8(writer, record->flags);
        xr_xsm_put_u8(writer, 0);
    }
}

static void encode_entities(XrXsmWriter *writer, const XrSemanticPlan *plan) {
    for (uint32_t i = 0; i < plan->entity_count; i++) {
        const XrSemanticEntityRecord *record = &plan->entities[i];
        xr_xsm_put_bytes(writer, record->id.bytes, sizeof(record->id.bytes));
        xr_xsm_put_string(writer, record->canonical_key);
        xr_xsm_put_u32(writer, record->parent);
        xr_xsm_put_u32(writer, record->subject);
        xr_xsm_put_u32(writer, record->ordinal);
        xr_xsm_put_u16(writer, record->kind);
        xr_xsm_put_u8(writer, record->subject_kind);
        xr_xsm_put_u8(writer, record->flags);
    }
}

static void encode_types(XrXsmWriter *writer, const XrSemanticPlan *plan) {
    for (uint32_t i = 0; i < plan->type_count; i++) {
        const XrSemanticTypeRecord *record = &plan->types[i];
        xr_xsm_put_bytes(writer, record->id.bytes, sizeof(record->id.bytes));
        xr_xsm_put_string(writer, record->canonical_key);
        xr_xsm_put_u32(writer, record->kind);
        xr_xsm_put_u32(writer, record->builtin_type);
        xr_xsm_put_u32(writer, record->source_class);
        xr_xsm_put_bytes(writer, record->source_class_identity.bytes,
                         sizeof(record->source_class_identity.bytes));
        xr_xsm_put_u32(writer, record->child_begin);
        xr_xsm_put_u32(writer, record->aggregate_extent);
        xr_xsm_put_u32(writer, record->aggregate_align);
        xr_xsm_put_u16(writer, record->child_count);
        xr_xsm_put_u8(writer, record->scalar_rep);
        xr_xsm_put_u8(writer, record->flags);
    }
    for (uint32_t i = 0; i < plan->type_child_count; i++)
        xr_xsm_put_u32(writer, plan->type_children[i]);
}

static void encode_functions(XrXsmWriter *writer, const XrSemanticPlan *plan) {
    for (uint32_t i = 0; i < plan->function_count; i++) {
        const XrSemanticFunctionRecord *record = &plan->functions[i];
        xr_xsm_put_bytes(writer, record->id.bytes, sizeof(record->id.bytes));
        xr_xsm_put_string(writer, record->canonical_key);
        xr_xsm_put_string(writer, record->name);
        xr_xsm_put_u32(writer, record->return_type);
        xr_xsm_put_u32(writer, record->parent);
        xr_xsm_put_u32(writer, record->parameter_begin);
        xr_xsm_put_u16(writer, record->parameter_count);
        xr_xsm_put_u16(writer, record->child_count);
        xr_xsm_put_u32(writer, record->capture_begin);
        xr_xsm_put_u16(writer, record->capture_count);
        xr_xsm_put_u16(writer, 0);
        xr_xsm_put_u32(writer, record->block_begin);
        xr_xsm_put_u32(writer, record->block_count);
        xr_xsm_put_u32(writer, record->value_begin);
        xr_xsm_put_u32(writer, record->value_count);
        xr_xsm_put_u32(writer, record->semantic_effects);
        xr_xsm_put_u32(writer, record->capability_mask);
        xr_xsm_put_u32(writer, record->source_class);
        xr_xsm_put_u16(writer, record->source_member_ordinal);
        xr_xsm_put_u16(writer, (uint16_t) record->return_parameter);
        xr_xsm_put_u8(writer, record->return_provenance);
        xr_xsm_put_u8(writer, record->source_kind);
        xr_xsm_put_u8(writer, record->flags);
        xr_xsm_put_u8(writer, 0);
    }
    for (uint32_t i = 0; i < plan->parameter_count; i++) {
        const XrSemanticParameterRecord *record = &plan->parameters[i];
        xr_xsm_put_bytes(writer, record->id.bytes, sizeof(record->id.bytes));
        xr_xsm_put_string(writer, record->canonical_key);
        xr_xsm_put_u32(writer, record->function);
        xr_xsm_put_u32(writer, record->type);
        xr_xsm_put_u32(writer, record->value);
        xr_xsm_put_u16(writer, record->ordinal);
        xr_xsm_put_u8(writer, record->mode);
        xr_xsm_put_u8(writer, record->ownership);
        xr_xsm_put_u8(writer, record->transfer_mode);
        xr_xsm_put_u8(writer, record->flags);
        xr_xsm_put_u16(writer, 0);
    }
    for (uint32_t i = 0; i < plan->capture_count; i++) {
        const XrSemanticCaptureRecord *record = &plan->captures[i];
        xr_xsm_put_bytes(writer, record->id.bytes, sizeof(record->id.bytes));
        xr_xsm_put_string(writer, record->canonical_key);
        xr_xsm_put_string(writer, record->name);
        xr_xsm_put_u32(writer, record->function);
        xr_xsm_put_u32(writer, record->source_function);
        xr_xsm_put_u32(writer, record->source_value);
        xr_xsm_put_u32(writer, record->source_capture);
        xr_xsm_put_u32(writer, record->type);
        xr_xsm_put_u32(writer, record->source_type);
        xr_xsm_put_u32(writer, record->source_index);
        xr_xsm_put_u16(writer, record->ordinal);
        xr_xsm_put_u8(writer, record->source);
        xr_xsm_put_u8(writer, record->kind);
        xr_xsm_put_u8(writer, record->storage_domain);
        xr_xsm_put_u8(writer, record->value_capability);
        xr_xsm_put_u8(writer, record->flags);
        xr_xsm_put_u8(writer, 0);
    }
}

static void encode_blocks(XrXsmWriter *writer, const XrSemanticPlan *plan) {
    for (uint32_t i = 0; i < plan->block_count; i++) {
        const XrSemanticBlockRecord *record = &plan->blocks[i];
        xr_xsm_put_bytes(writer, record->id.bytes, sizeof(record->id.bytes));
        xr_xsm_put_string(writer, record->canonical_key);
        xr_xsm_put_u32(writer, record->function);
        xr_xsm_put_u32(writer, record->operation_begin);
        xr_xsm_put_u32(writer, record->operation_count);
        xr_xsm_put_u32(writer, record->predecessor_begin);
        xr_xsm_put_u16(writer, record->predecessor_count);
        xr_xsm_put_u16(writer, record->kind);
        xr_xsm_put_u32(writer, record->successors[0]);
        xr_xsm_put_u32(writer, record->successors[1]);
        xr_xsm_put_u32(writer, record->control_value);
        xr_xsm_put_u32(writer, record->source_line);
    }
    for (uint32_t i = 0; i < plan->predecessor_count; i++)
        xr_xsm_put_u32(writer, plan->predecessors[i]);
}

static void encode_operations(XrXsmWriter *writer, const XrSemanticPlan *plan) {
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        const XrSemanticOperationRecord *record = &plan->operations[i];
        xr_xsm_put_bytes(writer, record->id.bytes, sizeof(record->id.bytes));
        xr_xsm_put_bytes(writer, record->allocation_id.bytes, sizeof(record->allocation_id.bytes));
        xr_xsm_put_string(writer, record->canonical_key);
        xr_xsm_put_string(writer, record->allocation_key);
        xr_xsm_put_u32(writer, record->function);
        xr_xsm_put_u32(writer, record->block);
        xr_xsm_put_u32(writer, record->result_value);
        xr_xsm_put_u32(writer, record->result_type);
        xr_xsm_put_u32(writer, record->operand_begin);
        xr_xsm_put_u16(writer, record->operand_count);
        xr_xsm_put_u16(writer, record->opcode);
        xr_xsm_put_u32(writer, record->metadata_begin);
        xr_xsm_put_u16(writer, record->metadata_count);
        xr_xsm_put_u8(writer, record->auxiliary_kind);
        xr_xsm_put_u8(writer, record->import_resolution);
        xr_xsm_put_u32(writer, record->effects);
        xr_xsm_put_u32(writer, record->source_line);
        xr_xsm_put_string(writer, record->source_file);
        xr_xsm_put_u32(writer, record->source_start_line);
        xr_xsm_put_u32(writer, record->source_start_column);
        xr_xsm_put_u32(writer, record->source_end_line);
        xr_xsm_put_u32(writer, record->source_end_column);
        xr_xsm_put_u32(writer, record->source_discriminator);
        xr_xsm_put_u64(writer, (uint64_t) record->semantic_immediate);
        xr_xsm_put_u32(writer, record->constant);
        xr_xsm_put_u32(writer, record->callable_function);
        for (unsigned e = 0; e < 8; e++)
            xr_xsm_put_u32(writer, record->evidence[e]);
        xr_xsm_put_u8(writer, record->ownership_use);
        xr_xsm_put_u8(writer, record->result_ownership);
        xr_xsm_put_u8(writer, record->transfer_mode);
        xr_xsm_put_u8(writer, record->parameter_mode);
        xr_xsm_put_u8(writer, record->parameter_ownership);
        xr_xsm_put_u8(writer, record->flags);
        xr_xsm_put_u16(writer, (uint16_t) record->result_alias_operand);
        xr_xsm_put_u16(writer, (uint16_t) record->return_parameter);
        xr_xsm_put_u8(writer, record->return_provenance);
        xr_xsm_put_u8(writer, record->return_complete);
        xr_xsm_put_u32(writer, record->view_source_value);
        xr_xsm_put_u32(writer, record->view_element_type);
        xr_xsm_put_u16(writer, (uint16_t) record->view_source_operand);
        xr_xsm_put_u16(writer, (uint16_t) record->view_source_parameter);
        xr_xsm_put_u8(writer, record->intrinsic_kind);
        xr_xsm_put_u8(writer, record->view_origin);
        xr_xsm_put_u8(writer, record->view_capability);
        xr_xsm_put_u8(writer, record->view_lifetime);
        xr_xsm_put_u8(writer, record->view_complete);
        xr_xsm_put_bytes(writer, record->reserved_view, sizeof(record->reserved_view));
    }
    for (uint32_t i = 0; i < plan->operand_count; i++) {
        const XrSemanticOperandRecord *record = &plan->operands[i];
        xr_xsm_put_u32(writer, record->value);
        xr_xsm_put_u32(writer, record->type);
        xr_xsm_put_u16(writer, (uint16_t) record->parameter);
        xr_xsm_put_u8(writer, record->role);
        xr_xsm_put_u8(writer, record->transfer_mode);
        xr_xsm_put_u8(writer, record->ownership_action);
        xr_xsm_put_u8(writer, record->parameter_mode);
        xr_xsm_put_u8(writer, record->access);
        xr_xsm_put_u8(writer, record->origin);
        xr_xsm_put_u8(writer, record->lifetime);
        xr_xsm_put_u8(writer, record->escape);
        xr_xsm_put_u8(writer, record->flags);
    }
    for (uint32_t i = 0; i < plan->metadata_count; i++)
        xr_xsm_put_string(writer, plan->metadata[i]);
}

static void encode_call_targets(XrXsmWriter *writer, const XrSemanticPlan *plan) {
    for (uint32_t i = 0; i < plan->call_target_count; i++) {
        const XrSemanticCallTargetRecord *record = &plan->call_targets[i];
        xr_xsm_put_bytes(writer, record->id.bytes, sizeof(record->id.bytes));
        xr_xsm_put_string(writer, record->canonical_key);
        xr_xsm_put_u32(writer, record->operation);
        xr_xsm_put_u32(writer, record->function);
        xr_xsm_put_u32(writer, record->dependency);
        xr_xsm_put_u32(writer, record->source_export);
        xr_xsm_put_bytes(writer, record->export_identity.bytes,
                         sizeof(record->export_identity.bytes));
        xr_xsm_put_bytes(writer, record->callee_function.bytes,
                         sizeof(record->callee_function.bytes));
        xr_xsm_put_u32(writer, record->callable_type);
        xr_xsm_put_u8(writer, record->kind);
        xr_xsm_put_u8(writer, 0);
        xr_xsm_put_u8(writer, 0);
        xr_xsm_put_u8(writer, 0);
    }
}

static void encode_dependencies_and_exports(XrXsmWriter *writer, const XrSemanticPlan *plan) {
    for (uint32_t i = 0; i < plan->dependency_count; i++) {
        const XrSemanticDependencyRecord *record = &plan->dependencies[i];
        xr_xsm_put_bytes(writer, record->id.bytes, sizeof(record->id.bytes));
        xr_xsm_put_string(writer, record->canonical_key);
        xr_xsm_put_string(writer, record->module_path);
        xr_xsm_put_bytes(writer, record->module.bytes, sizeof(record->module.bytes));
        xr_xsm_put_bytes(writer, record->semantic_fingerprint.bytes,
                         sizeof(record->semantic_fingerprint.bytes));
    }
    for (uint32_t i = 0; i < plan->source_export_count; i++) {
        const XrSemanticSourceExportRecord *record = &plan->source_exports[i];
        xr_xsm_put_bytes(writer, record->id.bytes, sizeof(record->id.bytes));
        xr_xsm_put_string(writer, record->canonical_key);
        xr_xsm_put_string(writer, record->name);
        xr_xsm_put_u32(writer, record->function);
        xr_xsm_put_u32(writer, record->shared_slot);
    }
}

static void encode_constants(XrXsmWriter *writer, const XrSemanticPlan *plan) {
    for (uint32_t i = 0; i < plan->constant_count; i++) {
        const XrSemanticConstantRecord *record = &plan->constants[i];
        xr_xsm_put_u32(writer, record->type);
        xr_xsm_put_u8(writer, record->kind);
        xr_xsm_put_u64(writer, (uint64_t) record->integer);
        xr_xsm_put_u64(writer, record->float_bits);
        xr_xsm_put_string(writer, record->string);
    }
}

static void encode_edges(XrXsmWriter *writer, const XrSemanticPlan *plan) {
    for (uint32_t i = 0; i < plan->edge_count; i++) {
        const XrSemanticEdgeRecord *record = &plan->edges[i];
        xr_xsm_put_bytes(writer, record->id.bytes, sizeof(record->id.bytes));
        xr_xsm_put_string(writer, record->canonical_key);
        xr_xsm_put_u32(writer, record->function);
        xr_xsm_put_u32(writer, record->from_block);
        xr_xsm_put_u32(writer, record->to_block);
        xr_xsm_put_u32(writer, record->operation);
        xr_xsm_put_u8(writer, record->kind);
        xr_xsm_put_u8(writer, record->flags);
        xr_xsm_put_u16(writer, 0);
    }
}

static void encode_ownership(XrXsmWriter *writer, const XrSemanticPlan *plan) {
    const XrOwnershipCertificate *certificate = plan->ownership;
    for (uint32_t i = 0; i < certificate->owner_count; i++) {
        const XrOwnershipOwnerRecord *record = &certificate->owners[i];
        xr_xsm_put_bytes(writer, record->id.bytes, sizeof(record->id.bytes));
        xr_xsm_put_string(writer, record->canonical_key);
        xr_xsm_put_u32(writer, record->function);
        xr_xsm_put_u32(writer, record->origin_value);
        xr_xsm_put_u8(writer, record->initial_state);
        xr_xsm_put_u8(writer, record->exit_state);
        xr_xsm_put_u8(writer, record->return_provenance);
        xr_xsm_put_u8(writer, record->flags);
    }
    for (uint32_t i = 0; i < certificate->event_count; i++) {
        const XrOwnershipEventRecord *record = &certificate->events[i];
        xr_xsm_put_bytes(writer, record->id.bytes, sizeof(record->id.bytes));
        xr_xsm_put_string(writer, record->canonical_key);
        xr_xsm_put_u32(writer, record->owner);
        xr_xsm_put_u32(writer, record->operation);
        xr_xsm_put_u32(writer, record->block);
        xr_xsm_put_u32(writer, record->successor);
        xr_xsm_put_u16(writer, (uint16_t) record->logical_delta);
        xr_xsm_put_u8(writer, record->kind);
        xr_xsm_put_u8(writer, record->state_after);
        xr_xsm_put_u8(writer, record->program_point);
        xr_xsm_put_u8(writer, 0);
    }
    for (uint32_t i = 0; i < certificate->edge_state_count; i++) {
        const XrOwnershipEdgeStateRecord *record = &certificate->edge_states[i];
        xr_xsm_put_u32(writer, record->owner);
        xr_xsm_put_u32(writer, record->block);
        xr_xsm_put_u32(writer, record->successor);
        xr_xsm_put_u32(writer, (uint32_t) record->entry_balance);
        xr_xsm_put_u32(writer, (uint32_t) record->exit_balance);
        xr_xsm_put_u8(writer, record->entry_state);
        xr_xsm_put_u8(writer, record->exit_state);
        xr_xsm_put_u16(writer, record->flags);
    }
    for (uint32_t i = 0; i < certificate->loop_invariant_count; i++) {
        const XrOwnershipLoopInvariantRecord *record = &certificate->loop_invariants[i];
        xr_xsm_put_bytes(writer, record->id.bytes, sizeof(record->id.bytes));
        xr_xsm_put_string(writer, record->canonical_key);
        xr_xsm_put_u32(writer, record->owner);
        xr_xsm_put_u32(writer, record->header);
        xr_xsm_put_u32(writer, record->backedge);
        xr_xsm_put_u32(writer, (uint32_t) record->balance);
        xr_xsm_put_u8(writer, record->state);
        xr_xsm_put_u8(writer, 0);
        xr_xsm_put_u8(writer, 0);
        xr_xsm_put_u8(writer, 0);
    }
}

bool xr_xsm_encode(const XrSemanticPlan *plan, uint8_t **bytes, size_t *size, char *error,
                   size_t error_size) {
    if (bytes)
        *bytes = NULL;
    if (size)
        *size = 0;
    if (!plan || !bytes || !size || !plan->frozen || !plan->verified || !plan->ownership ||
        (plan->dependency_count != 0 && !plan->module_set_verified)) {
        if (error && error_size)
            snprintf(error, error_size,
                     "XR_ARTIFACT_2004: encoder requires a verified frozen SemanticPlan");
        return false;
    }
    if (!xr_semantic_plan_verify(plan, error, error_size))
        return false;
    XrXsmWriter payload = {.limit = XR_XSM_MAX_PAYLOAD_SIZE};
    encode_counts(&payload, plan);
    encode_entities(&payload, plan);
    encode_source_classes(&payload, plan);
    encode_source_methods(&payload, plan);
    encode_types(&payload, plan);
    encode_functions(&payload, plan);
    encode_blocks(&payload, plan);
    encode_operations(&payload, plan);
    encode_call_targets(&payload, plan);
    encode_dependencies_and_exports(&payload, plan);
    encode_edges(&payload, plan);
    encode_constants(&payload, plan);
    encode_ownership(&payload, plan);
    if (payload.failed) {
        xr_free(payload.data);
        if (error && error_size)
            snprintf(error, error_size, "XR_EXEC_5003: XSM encoding budget exhausted");
        return false;
    }
    uint8_t digest[32];
    xr_sha256(payload.data, payload.size, digest);
    XrXsmWriter artifact = {.limit = XR_XSM_MAX_ARTIFACT_SIZE};
    xr_xsm_put_bytes(&artifact, xr_xsm_artifact_magic, XR_XSM_ARTIFACT_MAGIC_SIZE);
    xr_xsm_put_u32(&artifact, XR_SEMANTIC_SCHEMA_VERSION);
    xr_xsm_put_u32(&artifact, XR_XSM_HEADER_SIZE);
    xr_xsm_put_u64(&artifact, payload.size);
    xr_xsm_put_bytes(&artifact, digest, sizeof(digest));
    xr_xsm_put_bytes(&artifact, plan->operation_registry_fingerprint.bytes,
                     sizeof(plan->operation_registry_fingerprint.bytes));
    xr_xsm_put_bytes(&artifact, plan->stdlib_registry_fingerprint.bytes,
                     sizeof(plan->stdlib_registry_fingerprint.bytes));
    xr_xsm_put_bytes(&artifact, plan->fingerprint.bytes, sizeof(plan->fingerprint.bytes));
    xr_xsm_put_bytes(&artifact, payload.data, payload.size);
    xr_free(payload.data);
    if (artifact.failed) {
        xr_free(artifact.data);
        if (error && error_size)
            snprintf(error, error_size, "XR_EXEC_5003: XSM artifact allocation failed");
        return false;
    }
    *bytes = artifact.data;
    *size = artifact.size;
    return true;
}
