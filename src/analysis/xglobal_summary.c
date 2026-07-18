/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xglobal_summary.c - Whole-program summary/evidence data model
 */

#include "xglobal_summary.h"
#include "../base/xhash.h"
#include "../base/xmalloc.h"
#include "../base/xmemstream.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

XR_FUNC uint32_t xg_name_id(const char *name) {
    if (!name || !name[0])
        return 0;
    uint32_t h = xr_hash_bytes(name, strlen(name));
    return h ? h : 1;
}

static uint64_t type_key_fold_bytes(uint64_t h, const void *data, size_t len) {
    uint64_t part = xr_hash_bytes64(data, len);
    h ^= part + UINT64_C(0x9e3779b97f4a7c15) + (h << 6) + (h >> 2);
    return h ? h : 1;
}

static uint64_t type_key_fold_u64(uint64_t h, uint64_t value) {
    return type_key_fold_bytes(h, &value, sizeof(value));
}

static uint32_t type_key_folded32(uint64_t h) {
    uint32_t v = (uint32_t) (h ^ (h >> 32));
    return v ? v : 1;
}

XR_FUNC uint32_t xg_synthetic_type_key(uint8_t tref_kind) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    h = type_key_fold_u64(h, tref_kind);
    h = type_key_fold_u64(h, 0);
    h = type_key_fold_u64(h, 0);
    h = type_key_fold_u64(h, 0);
    return type_key_folded32(h);
}

XR_FUNC uint32_t xg_synthetic_width_type_key(uint8_t tref_kind, uint8_t native_width) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    h = type_key_fold_u64(h, tref_kind);
    h = type_key_fold_u64(h, native_width);
    h = type_key_fold_u64(h, 0);
    h = type_key_fold_u64(h, 0);
    return type_key_folded32(h);
}

XR_FUNC uint64_t xg_json_shape_hash_begin(uint32_t field_count) {
    return type_key_fold_u64(XR_FNV64_OFFSET_BASIS, field_count);
}

XR_FUNC uint64_t xg_json_shape_hash_add_field(uint64_t hash, uint8_t shape_kind, uint32_t name_id,
                                              uint32_t type_key) {
    hash = type_key_fold_u64(hash, name_id);
    if (shape_kind == XG_JSON_SHAPE_RECORD_BRIDGE)
        hash = type_key_fold_u64(hash, type_key);
    return hash;
}

static bool reserve_array(void **items, uint32_t *cap, uint32_t needed, size_t elem_size) {
    uint32_t new_cap;
    void *new_items;

    if (!items || !cap || elem_size == 0)
        return false;
    if (*cap >= needed)
        return true;
    new_cap = *cap < 8 ? 8 : *cap;
    while (new_cap < needed) {
        if (new_cap > UINT32_MAX / 2)
            return false;
        new_cap *= 2;
    }
    if ((size_t) new_cap > SIZE_MAX / elem_size)
        return false;
    new_items = xr_realloc(*items, (size_t) new_cap * elem_size);
    if (!new_items)
        return false;
    *items = new_items;
    *cap = new_cap;
    return true;
}

static uint64_t hash_mix(uint64_t hash, const void *data, size_t size) {
    uint64_t part = xr_hash_bytes64(data, size);
    hash ^= part + UINT64_C(0x9e3779b97f4a7c15) + (hash << 6) + (hash >> 2);
    return hash;
}

static uint64_t hash_u8(uint64_t hash, uint8_t value) {
    return hash_mix(hash, &value, sizeof(value));
}

static uint64_t hash_u32(uint64_t hash, uint32_t value) {
    return hash_mix(hash, &value, sizeof(value));
}

static uint64_t hash_u64(uint64_t hash, uint64_t value) {
    return hash_mix(hash, &value, sizeof(value));
}

XR_FUNC uint32_t xg_stable_source_node_id(XgModuleId module_id, uint32_t ast_kind, uint32_t line,
                                          uint32_t column) {
    uint64_t hash = XR_FNV64_OFFSET_BASIS;
    if (module_id == XG_NO_ID || ast_kind == 0 || line == 0 || column == 0)
        return 0;
    hash = hash_u32(hash, module_id);
    hash = hash_u32(hash, ast_kind);
    hash = hash_u32(hash, line);
    hash = hash_u32(hash, column);
    return type_key_folded32(hash);
}

static size_t bounded_cstr_len(const char *s, size_t max_len) {
    size_t len = 0;
    if (!s)
        return 0;
    while (len < max_len && s[len])
        len++;
    return len;
}

static uint64_t hash_module_summary(uint64_t hash, const XgModuleSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->name_id);
    hash = hash_u64(hash, row->canonical_hash);
    hash = hash_u64(hash, row->source_hash);
    hash = hash_u8(hash, row->kind);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_decl_summary(uint64_t hash, const XgDeclSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->source_node_id);
    hash = hash_u32(hash, row->decl_id);
    hash = hash_u8(hash, row->kind);
    hash = hash_u32(hash, row->flags);
    hash = hash_u32(hash, row->name_id);
    hash = hash_u32(hash, row->type_key);
    hash = hash_u32(hash, row->signature_key);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u32(hash, row->derive_flags);
    hash = hash_u32(hash, row->storage_flags);
    hash = hash_u8(hash, row->storage_owner);
    hash = hash_u8(hash, row->storage_mutability);
    hash = hash_u8(hash, row->address_identity);
    return hash_u8(hash, row->materialization_kind);
}

static uint64_t hash_class_summary(uint64_t hash, const XgClassSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->decl_id);
    hash = hash_u32(hash, row->class_id);
    hash = hash_u32(hash, row->parent_class_id);
    hash = hash_u32(hash, row->name_id);
    hash = hash_u32(hash, row->flags);
    hash = hash_u32(hash, row->field_start);
    hash = hash_u32(hash, row->field_count);
    hash = hash_u32(hash, row->method_start);
    hash = hash_u32(hash, row->method_count);
    hash = hash_u32(hash, row->interface_start);
    hash = hash_u32(hash, row->interface_count);
    hash = hash_u32(hash, row->generic_origin_class_id);
    hash = hash_u32(hash, row->generic_origin_name_id);
    hash = hash_u32(hash, row->generic_type_key);
    hash = hash_u32(hash, row->generic_type_arg_key_start);
    hash = hash_u32(hash, row->generic_type_arg_count);
    return hash_u8(hash, row->decl_kind);
}

static uint64_t hash_class_field_summary(uint64_t hash, const XgClassFieldSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->field_id);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->source_node_id);
    hash = hash_u32(hash, row->owner_class_id);
    hash = hash_u32(hash, row->name_id);
    hash = hash_u32(hash, row->type_key);
    hash = hash_u32(hash, row->target_name_id);
    hash = hash_u32(hash, row->target_class_id);
    hash = hash_u32(hash, row->target_interface_id);
    hash = hash_u32(hash, row->element_type_key);
    hash = hash_u32(hash, row->key_type_key);
    hash = hash_u32(hash, row->value_type_key);
    hash = hash_u32(hash, row->fixed_length);
    hash = hash_u32(hash, row->decl_ordinal);
    hash = hash_u32(hash, row->instance_slot);
    hash = hash_u32(hash, row->flags);
    hash = hash_u8(hash, row->semantic_kind);
    return hash_u8(hash, row->native_width);
}

static uint64_t hash_method_summary(uint64_t hash, const XgMethodSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->method_id);
    hash = hash_u32(hash, row->owner_class_id);
    hash = hash_u32(hash, row->source_node_id);
    hash = hash_u32(hash, row->name_id);
    hash = hash_u32(hash, row->signature_key);
    hash = hash_u32(hash, row->override_of);
    hash = hash_u32(hash, row->root_method_id);
    hash = hash_u32(hash, row->override_depth);
    hash = hash_u32(hash, row->default_arg_contract_id);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_interface_impl_summary(uint64_t hash, const XgInterfaceImplSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->implementor_class_id);
    hash = hash_u32(hash, row->interface_id);
    hash = hash_u32(hash, row->name_id);
    hash = hash_u32(hash, row->type_key);
    hash = hash_u32(hash, row->source_span_id);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_interface_extends_summary(uint64_t hash,
                                               const XgInterfaceExtendsSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->child_interface_id);
    hash = hash_u32(hash, row->parent_interface_id);
    hash = hash_u32(hash, row->name_id);
    hash = hash_u32(hash, row->type_key);
    hash = hash_u32(hash, row->source_span_id);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_interface_method_summary(uint64_t hash, const XgInterfaceMethodSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->interface_method_id);
    hash = hash_u32(hash, row->owner_interface_id);
    hash = hash_u32(hash, row->name_id);
    hash = hash_u32(hash, row->signature_key);
    hash = hash_u32(hash, row->ordinal);
    hash = hash_u32(hash, row->source_span_id);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_interface_object_use_summary(uint64_t hash,
                                                  const XgInterfaceObjectUseSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->use_id);
    hash = hash_u32(hash, row->interface_id);
    hash = hash_u32(hash, row->owner_func_id);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u32(hash, row->body_ordinal);
    hash = hash_u32(hash, row->type_key);
    hash = hash_u32(hash, row->reason);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_body_summary(uint64_t hash, const XgBodySummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->func_id);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->source_node_id);
    hash = hash_u32(hash, row->owner_decl_id);
    hash = hash_u32(hash, row->owner_class_id);
    hash = hash_u32(hash, row->owner_method_id);
    hash = hash_u32(hash, row->name_id);
    hash = hash_u32(hash, row->signature_key);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u8(hash, row->kind);
    hash = hash_u64(hash, row->body_hash);
    hash = hash_u32(hash, row->effect_bits);
    hash = hash_u32(hash, row->escape_bits);
    hash = hash_u32(hash, row->capability_bits);
    hash = hash_u32(hash, row->param_storage_key);
    hash = hash_u32(hash, row->param_storage_start);
    hash = hash_u32(hash, row->param_storage_count);
    hash = hash_u32(hash, row->callsite_start);
    hash = hash_u32(hash, row->callsite_count);
    hash = hash_u32(hash, row->metadata_use_bits);
    return hash_u32(hash, row->static_data_use_bits);
}

static uint64_t hash_param_storage_summary(uint64_t hash, const XgParamStorageSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->requirement_id);
    hash = hash_u32(hash, row->owner_func_id);
    hash = hash_u32(hash, row->param_index);
    hash = hash_u8(hash, row->storage_owner);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_callsite_summary(uint64_t hash, const XgCallsiteSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->callsite_id);
    hash = hash_u32(hash, row->owner_func_id);
    hash = hash_u32(hash, row->source_node_id);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u32(hash, row->body_ordinal);
    hash = hash_u8(hash, row->kind);
    hash = hash_u32(hash, row->static_target_func_id);
    hash = hash_u32(hash, row->receiver_static_class_id);
    hash = hash_u32(hash, row->receiver_static_interface_id);
    hash = hash_u32(hash, row->method_id);
    hash = hash_u32(hash, row->method_name_id);
    hash = hash_u32(hash, row->method_signature_key);
    hash = hash_u32(hash, row->arg_type_key_start);
    hash = hash_u32(hash, row->arg_count);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_link_dependency_summary(uint64_t hash, const XgLinkDependencySummary *row) {
    size_t name_len;
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->link_id);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->decl_id);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u32(hash, row->name_id);
    hash = hash_u8(hash, row->kind);
    hash = hash_u32(hash, row->flags);
    name_len = bounded_cstr_len(row->name, sizeof(row->name));
    hash = hash_u32(hash, (uint32_t) name_len);
    return hash_mix(hash, row->name, name_len);
}

static uint64_t hash_generic_inst_summary(uint64_t hash, const XgGenericInstSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->generic_inst_id);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->origin_decl_id);
    hash = hash_u32(hash, row->origin_func_id);
    hash = hash_u32(hash, row->origin_method_id);
    hash = hash_u32(hash, row->origin_class_id);
    hash = hash_u32(hash, row->specialized_func_id);
    hash = hash_u32(hash, row->specialized_class_id);
    hash = hash_u32(hash, row->root_callsite_id);
    hash = hash_u32(hash, row->constraint_interface_id);
    hash = hash_u32(hash, row->name_id);
    hash = hash_u32(hash, row->type_key);
    hash = hash_u32(hash, row->type_arg_key_start);
    hash = hash_u32(hash, row->type_arg_count);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u8(hash, row->kind);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_generic_body_use_summary(uint64_t hash, const XgGenericBodyUseSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->use_id);
    hash = hash_u32(hash, row->generic_inst_id);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->owner_func_id);
    hash = hash_u32(hash, row->origin_body_func_id);
    hash = hash_u32(hash, row->specialized_body_func_id);
    hash = hash_u32(hash, row->root_callsite_id);
    hash = hash_u32(hash, row->type_key);
    hash = hash_u32(hash, row->type_arg_key_start);
    hash = hash_u32(hash, row->type_arg_count);
    hash = hash_u32(hash, row->estimated_body_size);
    hash = hash_u32(hash, row->flags);
    return hash_u64(hash, row->body_use_hash);
}

static uint64_t hash_generic_storage_summary(uint64_t hash, const XgGenericStorageSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->storage_id);
    hash = hash_u32(hash, row->generic_inst_id);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u8(hash, row->storage_kind);
    hash = hash_u32(hash, row->origin_type_key);
    hash = hash_u32(hash, row->specialized_type_key);
    hash = hash_u32(hash, row->elem_type_key);
    hash = hash_u32(hash, row->key_type_key);
    hash = hash_u32(hash, row->value_type_key);
    hash = hash_u32(hash, row->container_plan_id);
    hash = hash_u32(hash, row->flags);
    return hash_u64(hash, row->storage_hash);
}

static uint64_t hash_generic_code_size_summary(uint64_t hash, const XgGenericCodeSizeSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->code_size_id);
    hash = hash_u32(hash, row->generic_inst_id);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->body_use_id);
    hash = hash_u32(hash, row->origin_body_size_estimate);
    hash = hash_u32(hash, row->specialized_body_size_estimate);
    hash = hash_u32(hash, row->instantiation_count);
    hash = hash_u32(hash, row->threshold);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_sequence_access_summary(uint64_t hash, const XgSequenceAccessSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->access_id);
    hash = hash_u32(hash, row->owner_func_id);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u32(hash, row->body_ordinal);
    hash = hash_u8(hash, row->sequence_kind);
    hash = hash_u8(hash, row->access_kind);
    hash = hash_u32(hash, row->receiver_type_key);
    hash = hash_u32(hash, row->elem_type_key);
    hash = hash_u32(hash, row->index_expr_id);
    hash = hash_u32(hash, row->length_expr_id);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_capacity_op_summary(uint64_t hash, const XgCapacityOpSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->op_id);
    hash = hash_u32(hash, row->owner_func_id);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u32(hash, row->body_ordinal);
    hash = hash_u8(hash, row->sequence_kind);
    hash = hash_u8(hash, row->op_kind);
    hash = hash_u32(hash, row->receiver_type_key);
    hash = hash_u32(hash, row->elem_type_key);
    hash = hash_u32(hash, row->count_expr_id);
    hash = hash_u32(hash, row->loop_id);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_bulk_op_summary(uint64_t hash, const XgBulkOpSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->op_id);
    hash = hash_u32(hash, row->owner_func_id);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u32(hash, row->body_ordinal);
    hash = hash_u8(hash, row->op_kind);
    hash = hash_u32(hash, row->elem_type_key);
    hash = hash_u32(hash, row->src_type_key);
    hash = hash_u32(hash, row->dst_type_key);
    hash = hash_u32(hash, row->length_expr_id);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_encoding_op_summary(uint64_t hash, const XgEncodingOpSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->op_id);
    hash = hash_u32(hash, row->owner_func_id);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u32(hash, row->body_ordinal);
    hash = hash_u8(hash, row->op_kind);
    hash = hash_u32(hash, row->input_type_key);
    hash = hash_u32(hash, row->output_type_key);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_derive_summary(uint64_t hash, const XgDeriveSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->derive_id);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->owner_decl_id);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u32(hash, row->type_key);
    hash = hash_u8(hash, row->derive_kind);
    hash = hash_u32(hash, row->field_start);
    hash = hash_u32(hash, row->field_count);
    hash = hash_u32(hash, row->method_start);
    hash = hash_u32(hash, row->method_count);
    hash = hash_u32(hash, row->flags);
    return hash_u64(hash, row->derive_hash);
}

static uint64_t hash_derived_field_summary(uint64_t hash, const XgDerivedFieldSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->field_id);
    hash = hash_u32(hash, row->derive_id);
    hash = hash_u32(hash, row->field_ordinal);
    hash = hash_u32(hash, row->name_id);
    hash = hash_u32(hash, row->type_key);
    hash = hash_u32(hash, row->source_field_id);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_derived_method_summary(uint64_t hash, const XgDerivedMethodSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->method_id);
    hash = hash_u32(hash, row->derive_id);
    hash = hash_u8(hash, row->method_kind);
    hash = hash_u32(hash, row->generated_body_func_id);
    hash = hash_u32(hash, row->signature_key);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_json_shape_summary(uint64_t hash, const XgJsonShapeSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->json_shape_id);
    hash = hash_u32(hash, row->record_shape_id);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->owner_func_id);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u32(hash, row->type_key);
    hash = hash_u32(hash, row->field_name_start);
    hash = hash_u32(hash, row->field_count);
    hash = hash_u8(hash, row->shape_kind);
    hash = hash_u32(hash, row->flags);
    return hash_u64(hash, row->shape_hash);
}

static uint64_t hash_json_access_summary(uint64_t hash, const XgJsonAccessSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->json_access_id);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->owner_func_id);
    hash = hash_u32(hash, row->receiver_shape_id);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u32(hash, row->key_name_id);
    hash = hash_u32(hash, row->result_type_key);
    hash = hash_u32(hash, row->field_ordinal);
    hash = hash_u8(hash, row->access_kind);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_json_field_summary(uint64_t hash, const XgJsonFieldSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->field_id);
    hash = hash_u32(hash, row->shape_id);
    hash = hash_u32(hash, row->field_ordinal);
    hash = hash_u32(hash, row->name_id);
    hash = hash_u32(hash, row->type_key);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_json_codec_summary(uint64_t hash, const XgJsonCodecSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->codec_id);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->owner_func_id);
    hash = hash_u32(hash, row->source_node_id);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u8(hash, row->codec_kind);
    hash = hash_u32(hash, row->input_type_key);
    hash = hash_u32(hash, row->target_type_key);
    hash = hash_u32(hash, row->input_shape_id);
    hash = hash_u32(hash, row->output_shape_id);
    hash = hash_u32(hash, row->record_shape_id);
    hash = hash_u32(hash, row->field_count);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_record_shape_summary(uint64_t hash, const XgRecordShapeSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->record_shape_id);
    hash = hash_u32(hash, row->json_shape_id);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->owner_func_id);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u32(hash, row->type_key);
    hash = hash_u32(hash, row->field_name_start);
    hash = hash_u32(hash, row->field_count);
    hash = hash_u8(hash, row->shape_kind);
    hash = hash_u32(hash, row->flags);
    return hash_u64(hash, row->shape_hash);
}

static uint64_t hash_record_field_summary(uint64_t hash, const XgRecordFieldSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->field_id);
    hash = hash_u32(hash, row->shape_id);
    hash = hash_u32(hash, row->field_ordinal);
    hash = hash_u32(hash, row->name_id);
    hash = hash_u32(hash, row->type_key);
    hash = hash_u32(hash, row->default_value_id);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_record_access_summary(uint64_t hash, const XgRecordAccessSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->record_access_id);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->owner_func_id);
    hash = hash_u32(hash, row->receiver_shape_id);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u32(hash, row->field_name_id);
    hash = hash_u32(hash, row->result_type_key);
    hash = hash_u32(hash, row->field_ordinal);
    hash = hash_u8(hash, row->access_kind);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_record_merge_summary(uint64_t hash, const XgRecordMergeSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->merge_id);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->owner_func_id);
    hash = hash_u32(hash, row->source_node_id);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u32(hash, row->base_shape_id);
    hash = hash_u32(hash, row->patch_shape_id);
    hash = hash_u32(hash, row->result_shape_id);
    hash = hash_u32(hash, row->base_field_count);
    hash = hash_u32(hash, row->patch_field_count);
    hash = hash_u32(hash, row->result_field_count);
    hash = hash_u32(hash, row->overwrite_count);
    hash = hash_u32(hash, row->copy_table_id);
    hash = hash_u32(hash, row->flags);
    return hash_u64(hash, row->merge_hash);
}

static uint64_t hash_options_bag_summary(uint64_t hash, const XgOptionsBagSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->options_id);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->owner_func_id);
    hash = hash_u32(hash, row->callsite_id);
    hash = hash_u32(hash, row->param_shape_id);
    hash = hash_u32(hash, row->supplied_shape_id);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u32(hash, row->supplied_field_mask_id);
    hash = hash_u32(hash, row->default_field_mask_id);
    hash = hash_u32(hash, row->required_field_mask_id);
    hash = hash_u32(hash, row->supplied_count);
    hash = hash_u32(hash, row->default_count);
    hash = hash_u32(hash, row->required_count);
    hash = hash_u8(hash, row->action);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_map_shape_summary(uint64_t hash, const XgMapShapeSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->shape_id);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->owner_func_id);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u8(hash, row->container_kind);
    hash = hash_u8(hash, row->source);
    hash = hash_u32(hash, row->key_type_key);
    hash = hash_u32(hash, row->value_type_key);
    hash = hash_u32(hash, row->entry_start);
    hash = hash_u32(hash, row->entry_count);
    hash = hash_u32(hash, row->literal_count);
    hash = hash_u32(hash, row->flags);
    return hash_u64(hash, row->shape_hash);
}

static uint64_t hash_map_entry_summary(uint64_t hash, const XgMapEntrySummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->entry_id);
    hash = hash_u32(hash, row->shape_id);
    hash = hash_u32(hash, row->entry_ordinal);
    hash = hash_u32(hash, row->key_const_id);
    hash = hash_u32(hash, row->value_const_id);
    hash = hash_u64(hash, (uint64_t) row->key_i64);
    hash = hash_u64(hash, row->prehash);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_key_access_summary(uint64_t hash, const XgKeyAccessSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->access_id);
    hash = hash_u32(hash, row->owner_func_id);
    hash = hash_u32(hash, row->source_span_id);
    hash = hash_u32(hash, row->body_ordinal);
    hash = hash_u8(hash, row->container_kind);
    hash = hash_u8(hash, row->op);
    hash = hash_u32(hash, row->receiver_shape_id);
    hash = hash_u32(hash, row->receiver_type_key);
    hash = hash_u32(hash, row->key_type_key);
    hash = hash_u32(hash, row->value_type_key);
    hash = hash_u32(hash, row->key_const_id);
    hash = hash_u64(hash, row->key_prehash);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_hash_eq_summary(uint64_t hash, const XgHashEqSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->hash_eq_id);
    hash = hash_u32(hash, row->type_key);
    hash = hash_u8(hash, row->kind);
    hash = hash_u32(hash, row->eq_derive_id);
    hash = hash_u32(hash, row->hash_derive_id);
    hash = hash_u32(hash, row->eq_func_id);
    hash = hash_u32(hash, row->hash_func_id);
    return hash_u32(hash, row->flags);
}

static uint64_t hash_build_key(uint64_t hash, const XgBuildKey *key) {
    if (!key)
        return hash_u32(hash, 0);
    hash = hash_u64(hash, key->source_hash);
    hash = hash_u64(hash, key->compiler_semver_hash);
    hash = hash_u64(hash, key->profile_hash);
    hash = hash_u64(hash, key->imported_summary_hash);
    hash = hash_u32(hash, key->module_id);
    return hash_u32(hash, key->profile);
}

XR_FUNC const char *xg_build_profile_name(uint32_t profile) {
    switch ((XgBuildProfile) profile) {
        case XG_BUILD_CHECK:
            return "check";
        case XG_BUILD_DEV:
            return "dev";
        case XG_BUILD_NATIVE_RELEASE:
            return "native_release";
        case XG_BUILD_FREESTANDING:
            return "freestanding";
        case XG_BUILD_DEBUG_TOOLING:
            return "debug_tooling";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_decl_kind_name(uint8_t kind) {
    switch ((XgDeclKind) kind) {
        case XG_DECL_FUNC:
            return "func";
        case XG_DECL_CLASS:
            return "class";
        case XG_DECL_STRUCT:
            return "struct";
        case XG_DECL_UNION:
            return "union";
        case XG_DECL_ENUM:
            return "enum";
        case XG_DECL_INTERFACE:
            return "interface";
        case XG_DECL_GLOBAL:
            return "global";
        default:
            return "unknown";
    }
}

XR_FUNC bool xg_decl_kind_supports_methods(uint8_t kind) {
    switch ((XgDeclKind) kind) {
        case XG_DECL_CLASS:
        case XG_DECL_STRUCT:
        case XG_DECL_UNION:
            return true;
        default:
            return false;
    }
}

XR_FUNC bool xg_decl_kind_is_runtime_class(uint8_t kind) {
    return kind == XG_DECL_CLASS;
}

XR_FUNC bool xg_decl_kind_is_value_aggregate(uint8_t kind) {
    return kind == XG_DECL_STRUCT || kind == XG_DECL_UNION;
}

XR_FUNC const char *xg_callsite_kind_name(uint8_t kind) {
    switch ((XgCallsiteKind) kind) {
        case XG_CALL_DIRECT_FUNC:
            return "direct_func";
        case XG_CALL_METHOD:
            return "method";
        case XG_CALL_INTERFACE:
            return "interface";
        case XG_CALL_CLOSURE:
            return "closure";
        case XG_CALL_NATIVE:
            return "native";
        case XG_CALL_EXTERN:
            return "extern";
        default:
            return "unknown";
    }
}

static const char *xg_body_kind_name(uint8_t kind) {
    switch ((XgBodyKind) kind) {
        case XG_BODY_MODULE_INIT:
            return "module_init";
        case XG_BODY_FUNCTION:
            return "function";
        case XG_BODY_METHOD:
            return "method";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_link_dependency_kind_name(uint8_t kind) {
    switch ((XgLinkDependencyKind) kind) {
        case XG_LINK_DEP_EXTERN_DYLIB:
            return "extern_dylib";
        case XG_LINK_DEP_STDLIB_MODULE:
            return "stdlib_module";
        case XG_LINK_DEP_STDLIB_SYMBOL:
            return "stdlib_symbol";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_generic_inst_kind_name(uint8_t kind) {
    switch ((XgGenericInstKind) kind) {
        case XG_GENERIC_INST_FUNCTION:
            return "function";
        case XG_GENERIC_INST_METHOD:
            return "method";
        case XG_GENERIC_INST_CLASS:
            return "class";
        case XG_GENERIC_INST_CONTAINER:
            return "container";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_generic_storage_kind_name(uint8_t kind) {
    switch ((XgGenericStorageKind) kind) {
        case XG_GENERIC_STORAGE_ARRAY:
            return "array";
        case XG_GENERIC_STORAGE_MAP:
            return "map";
        case XG_GENERIC_STORAGE_SET:
            return "set";
        case XG_GENERIC_STORAGE_CLASS:
            return "class";
        case XG_GENERIC_STORAGE_STRUCT:
            return "struct";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_sequence_kind_name(uint8_t kind) {
    switch ((XgSequenceKind) kind) {
        case XG_SEQ_ARRAY:
            return "array";
        case XG_SEQ_BYTES:
            return "bytes";
        case XG_SEQ_STRING:
            return "string";
        case XG_SEQ_SPAN:
            return "span";
        case XG_SEQ_BYTE_SLICE:
            return "byte_slice";
        case XG_SEQ_STRING_BUILDER:
            return "string_builder";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_sequence_access_kind_name(uint8_t kind) {
    switch ((XgSequenceAccessKind) kind) {
        case XG_SEQ_ACCESS_INDEX_GET:
            return "index_get";
        case XG_SEQ_ACCESS_INDEX_SET:
            return "index_set";
        case XG_SEQ_ACCESS_SLICE:
            return "slice";
        case XG_SEQ_ACCESS_ITER:
            return "iter";
        case XG_SEQ_ACCESS_LENGTH:
            return "length";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_capacity_op_kind_name(uint8_t kind) {
    switch ((XgCapacityOpKind) kind) {
        case XG_CAPACITY_PUSH:
            return "push";
        case XG_CAPACITY_APPEND:
            return "append";
        case XG_CAPACITY_EXTEND:
            return "extend";
        case XG_CAPACITY_RESERVE:
            return "reserve";
        case XG_CAPACITY_CONCAT:
            return "concat";
        case XG_CAPACITY_TO_STRING:
            return "to_string";
        case XG_CAPACITY_CLEAR:
            return "clear";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_bulk_op_kind_name(uint8_t kind) {
    switch ((XgBulkOpKind) kind) {
        case XG_BULK_COPY:
            return "copy";
        case XG_BULK_FILL:
            return "fill";
        case XG_BULK_COMPARE:
            return "compare";
        case XG_BULK_REPEAT:
            return "repeat";
        case XG_BULK_COPY_WITHIN:
            return "copy_within";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_encoding_op_kind_name(uint8_t kind) {
    switch ((XgEncodingOpKind) kind) {
        case XG_ENCODING_STRING_TO_BYTES:
            return "string_to_bytes";
        case XG_ENCODING_BYTES_TO_STRING:
            return "bytes_to_string";
        case XG_ENCODING_UTF8_VALIDATE:
            return "utf8_validate";
        case XG_ENCODING_UTF8_COUNT:
            return "utf8_count";
        case XG_ENCODING_UTF16_ENCODE:
            return "utf16_encode";
        case XG_ENCODING_UTF16_DECODE:
            return "utf16_decode";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_derive_kind_name(uint8_t kind) {
    switch ((XgDeriveKind) kind) {
        case XG_DERIVE_JSON:
            return "json";
        case XG_DERIVE_INSPECT:
            return "inspect";
        case XG_DERIVE_EQ:
            return "eq";
        case XG_DERIVE_HASH:
            return "hash";
        case XG_DERIVE_CLONE:
            return "clone";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_derived_method_kind_name(uint8_t kind) {
    switch ((XgDerivedMethodKind) kind) {
        case XG_DERIVED_METHOD_JSON_ENCODE:
            return "json_encode";
        case XG_DERIVED_METHOD_INSPECT_FORMAT:
            return "inspect_format";
        case XG_DERIVED_METHOD_EQ:
            return "eq";
        case XG_DERIVED_METHOD_HASH:
            return "hash";
        case XG_DERIVED_METHOD_CLONE:
            return "clone";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_json_shape_kind_name(uint8_t kind) {
    switch ((XgJsonShapeKind) kind) {
        case XG_JSON_SHAPE_OPEN:
            return "open";
        case XG_JSON_SHAPE_SHAPED:
            return "shaped";
        case XG_JSON_SHAPE_RECORD_BRIDGE:
            return "record_bridge";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_json_access_kind_name(uint8_t kind) {
    switch ((XgJsonAccessKind) kind) {
        case XG_JSON_ACCESS_FIELD_GET:
            return "field_get";
        case XG_JSON_ACCESS_FIELD_SET:
            return "field_set";
        case XG_JSON_ACCESS_INDEX_GET:
            return "index_get";
        case XG_JSON_ACCESS_INDEX_SET:
            return "index_set";
        case XG_JSON_ACCESS_GET_DEFAULT:
            return "get_default";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_json_codec_kind_name(uint8_t kind) {
    switch ((XgJsonCodecKind) kind) {
        case XG_JSON_CODEC_PARSE:
            return "parse";
        case XG_JSON_CODEC_DECODE:
            return "decode";
        case XG_JSON_CODEC_ENCODE:
            return "encode";
        case XG_JSON_CODEC_STRINGIFY:
            return "stringify";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_record_shape_kind_name(uint8_t kind) {
    switch ((XgRecordShapeKind) kind) {
        case XG_RECORD_SHAPE_LITERAL:
            return "literal";
        case XG_RECORD_SHAPE_OPTIONS:
            return "options";
        case XG_RECORD_SHAPE_SPREAD:
            return "spread";
        case XG_RECORD_SHAPE_STATIC:
            return "static";
        case XG_RECORD_SHAPE_PATCH:
            return "patch";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_record_access_kind_name(uint8_t kind) {
    switch ((XgRecordAccessKind) kind) {
        case XG_RECORD_ACCESS_FIELD_GET:
            return "field_get";
        case XG_RECORD_ACCESS_FIELD_SET:
            return "field_set";
        case XG_RECORD_ACCESS_DESTRUCTURE:
            return "destructure";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_options_action_name(uint8_t action) {
    switch ((XgOptionsAction) action) {
        case XG_OPTIONS_DEFAULT_ELIDED:
            return "default_elided";
        case XG_OPTIONS_DEFAULT_FILL_TABLE:
            return "default_fill_table";
        case XG_OPTIONS_REQUIRED_CHECK:
            return "required_check";
        case XG_OPTIONS_CALLSITE_SPECIALIZED:
            return "callsite_specialized";
        case XG_OPTIONS_REJECT:
            return "reject";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_map_container_kind_name(uint8_t kind) {
    switch ((XgMapContainerKind) kind) {
        case XG_MAP_CONTAINER_MAP:
            return "map";
        case XG_MAP_CONTAINER_SET:
            return "set";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_map_shape_source_name(uint8_t source) {
    switch ((XgMapShapeSource) source) {
        case XG_MAP_SHAPE_SRC_LITERAL:
            return "literal";
        case XG_MAP_SHAPE_SRC_CONSTRUCTOR:
            return "constructor";
        case XG_MAP_SHAPE_SRC_FROM_ARRAY:
            return "from_array";
        case XG_MAP_SHAPE_SRC_STATIC:
            return "static";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_key_access_op_name(uint8_t op) {
    switch ((XgKeyAccessOp) op) {
        case XG_KEY_ACCESS_GET:
            return "get";
        case XG_KEY_ACCESS_INDEX_GET:
            return "index_get";
        case XG_KEY_ACCESS_SET:
            return "set";
        case XG_KEY_ACCESS_HAS:
            return "has";
        case XG_KEY_ACCESS_DELETE:
            return "delete";
        case XG_KEY_ACCESS_ADD:
            return "add";
        case XG_KEY_ACCESS_CLEAR:
            return "clear";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_hash_eq_kind_name(uint8_t kind) {
    switch ((XgHashEqKind) kind) {
        case XG_HASH_EQ_BUILTIN:
            return "builtin";
        case XG_HASH_EQ_ENUM_ORDINAL:
            return "enum_ordinal";
        case XG_HASH_EQ_DERIVE:
            return "derive";
        case XG_HASH_EQ_USER_METHOD:
            return "user_method";
        case XG_HASH_EQ_MISSING:
            return "missing";
        default:
            return "unknown";
    }
}

XR_FUNC const char *xg_body_effect_name(uint32_t effect) {
    switch (effect) {
        case XG_BODY_MAY_ERROR:
            return "error";
        case XG_BODY_MAY_PANIC:
            return "panic";
        case XG_BODY_MAY_SUSPEND:
            return "suspend";
        case XG_BODY_MAY_ALLOC:
            return "alloc";
        case XG_BODY_MAY_MUTATE:
            return "mutate";
        case XG_BODY_MAY_CALL_NATIVE:
            return "native_call";
        case XG_BODY_MAY_READ_MEM:
            return "read_mem";
        case XG_BODY_MAY_CALL:
            return "call";
        case XG_BODY_MAY_SPAWN:
            return "spawn";
        case XG_BODY_ACCESSES_MUTABLE_MODULE:
            return "mutable_module";
        case XG_BODY_OBSERVES_TASK_ID:
            return "task_identity";
        default:
            return "unknown";
    }
}

XR_FUNC const uint32_t *xg_body_effect_catalog(uint32_t *out_count) {
    static const uint32_t effects[] = {
        XG_BODY_MAY_ERROR,        XG_BODY_MAY_PANIC,
        XG_BODY_MAY_SUSPEND,      XG_BODY_MAY_ALLOC,
        XG_BODY_MAY_MUTATE,       XG_BODY_MAY_CALL_NATIVE,
        XG_BODY_MAY_READ_MEM,     XG_BODY_MAY_CALL,
        XG_BODY_MAY_SPAWN,        XG_BODY_ACCESSES_MUTABLE_MODULE,
        XG_BODY_OBSERVES_TASK_ID,
    };
    if (out_count)
        *out_count = (uint32_t) (sizeof(effects) / sizeof(effects[0]));
    return effects;
}

XR_FUNC const char *xg_body_escape_name(uint32_t escape) {
    switch (escape) {
        case XG_BODY_ESCAPE_RETURN:
            return "return";
        case XG_BODY_ESCAPE_FIELD:
            return "field";
        case XG_BODY_ESCAPE_CONTAINER:
            return "container";
        case XG_BODY_ESCAPE_CORO:
            return "coro";
        case XG_BODY_ESCAPE_NATIVE:
            return "native";
        case XG_BODY_ESCAPE_EXTERN:
            return "extern";
        case XG_BODY_ESCAPE_CAPTURE:
            return "capture";
        default:
            return "unknown";
    }
}

XR_FUNC const uint32_t *xg_body_escape_catalog(uint32_t *out_count) {
    static const uint32_t escapes[] = {
        XG_BODY_ESCAPE_RETURN, XG_BODY_ESCAPE_FIELD,  XG_BODY_ESCAPE_CONTAINER, XG_BODY_ESCAPE_CORO,
        XG_BODY_ESCAPE_NATIVE, XG_BODY_ESCAPE_EXTERN, XG_BODY_ESCAPE_CAPTURE,
    };
    if (out_count)
        *out_count = (uint32_t) (sizeof(escapes) / sizeof(escapes[0]));
    return escapes;
}

XR_FUNC const char *xg_interface_object_use_name(uint32_t reason) {
    switch (reason) {
        case XG_INTERFACE_OBJECT_USE_VALUE:
            return "value";
        case XG_INTERFACE_OBJECT_USE_ARRAY:
            return "array";
        case XG_INTERFACE_OBJECT_USE_FIELD:
            return "field";
        case XG_INTERFACE_OBJECT_USE_RETURN:
            return "return";
        case XG_INTERFACE_OBJECT_USE_CAPTURE:
            return "capture";
        case XG_INTERFACE_OBJECT_USE_PARAM:
            return "param";
        default:
            return "unknown";
    }
}

XR_FUNC const uint32_t *xg_interface_object_use_catalog(uint32_t *out_count) {
    static const uint32_t reasons[] = {
        XG_INTERFACE_OBJECT_USE_VALUE,   XG_INTERFACE_OBJECT_USE_ARRAY,
        XG_INTERFACE_OBJECT_USE_FIELD,   XG_INTERFACE_OBJECT_USE_RETURN,
        XG_INTERFACE_OBJECT_USE_CAPTURE, XG_INTERFACE_OBJECT_USE_PARAM,
    };
    if (out_count)
        *out_count = (uint32_t) (sizeof(reasons) / sizeof(reasons[0]));
    return reasons;
}

XR_FUNC const char *xg_capability_name(uint32_t capability) {
    switch (capability) {
        case XG_CAP_COROUTINE:
            return "coroutine";
        case XG_CAP_CHANNEL:
            return "channel";
        case XG_CAP_EXCEPTION:
            return "exception";
        case XG_CAP_NATIVE:
            return "native";
        case XG_CAP_EXTERN:
            return "extern";
        case XG_CAP_OBJECTS:
            return "objects";
        case XG_CAP_DEEP_COPY:
            return "deep_copy";
        case XG_CAP_INSTANCEOF:
            return "instanceof";
        case XG_CAP_SYS_THREAD:
            return "sys_thread";
        case XG_CAP_SCOPE:
            return "scope";
        case XG_CAP_TIMER:
            return "timer";
        case XG_CAP_NETPOLL:
            return "netpoll";
        case XG_CAP_TASK:
            return "task";
        case XG_CAP_ATOMIC:
            return "atomic";
        case XG_CAP_WORK_QUEUE:
            return "work_queue";
        case XG_CAP_RESULT_GROUP:
            return "result_group";
        case XG_CAP_COUNTDOWN_LATCH:
            return "countdown_latch";
        case XG_CAP_SEMAPHORE:
            return "semaphore";
        case XG_CAP_EVENT_COUNT:
            return "event_count";
        case XG_CAP_GENERATOR:
            return "generator";
        case XG_CAP_STACKTRACE:
            return "stacktrace";
        case XG_CAP_PARALLEL:
            return "parallel";
        default:
            return "unknown";
    }
}

XR_FUNC const uint32_t *xg_capability_catalog(uint32_t *out_count) {
    static const uint32_t capabilities[] = {
        XG_CAP_COROUTINE,    XG_CAP_CHANNEL,         XG_CAP_EXCEPTION,
        XG_CAP_NATIVE,       XG_CAP_EXTERN,          XG_CAP_OBJECTS,
        XG_CAP_DEEP_COPY,    XG_CAP_INSTANCEOF,      XG_CAP_SYS_THREAD,
        XG_CAP_SCOPE,        XG_CAP_TIMER,           XG_CAP_NETPOLL,
        XG_CAP_TASK,         XG_CAP_ATOMIC,          XG_CAP_WORK_QUEUE,
        XG_CAP_RESULT_GROUP, XG_CAP_COUNTDOWN_LATCH, XG_CAP_SEMAPHORE,
        XG_CAP_EVENT_COUNT,  XG_CAP_GENERATOR,       XG_CAP_STACKTRACE,
        XG_CAP_PARALLEL,
    };
    if (out_count)
        *out_count = (uint32_t) (sizeof(capabilities) / sizeof(capabilities[0]));
    return capabilities;
}

XR_FUNC const char *xg_metadata_name(uint32_t metadata) {
    switch (metadata) {
        case XG_METADATA_TYPENAME:
            return "typename";
        case XG_METADATA_DERIVE:
            return "derive";
        case XG_METADATA_DEBUG:
            return "debug";
        case XG_METADATA_TOOLING:
            return "tooling";
        default:
            return "unknown";
    }
}

XR_FUNC const uint32_t *xg_metadata_catalog(uint32_t *out_count) {
    static const uint32_t metadata[] = {
        XG_METADATA_TYPENAME,
        XG_METADATA_DERIVE,
        XG_METADATA_DEBUG,
        XG_METADATA_TOOLING,
    };
    if (out_count)
        *out_count = (uint32_t) (sizeof(metadata) / sizeof(metadata[0]));
    return metadata;
}

XR_FUNC const char *xg_static_data_name(uint32_t static_data) {
    switch (static_data) {
        case XG_STATIC_DATA_COMPTIME_VALUE:
            return "comptime_value";
        case XG_STATIC_DATA_FIXED_LAYOUT:
            return "fixed_layout";
        case XG_STATIC_DATA_RODATA:
            return "rodata";
        case XG_STATIC_DATA_FREESTANDING_SAFE:
            return "freestanding_safe";
        case XG_STATIC_DATA_RUNTIME_INIT:
            return "runtime_init";
        default:
            return "unknown";
    }
}

XR_FUNC const uint32_t *xg_static_data_catalog(uint32_t *out_count) {
    static const uint32_t static_data[] = {
        XG_STATIC_DATA_COMPTIME_VALUE,    XG_STATIC_DATA_FIXED_LAYOUT, XG_STATIC_DATA_RODATA,
        XG_STATIC_DATA_FREESTANDING_SAFE, XG_STATIC_DATA_RUNTIME_INIT,
    };
    if (out_count)
        *out_count = (uint32_t) (sizeof(static_data) / sizeof(static_data[0]));
    return static_data;
}

XR_FUNC const char *xg_evidence_cache_phase_name(uint32_t phase) {
    switch ((XgEvidenceCachePhase) phase) {
        case XG_EVIDENCE_CACHE_DECLARATIONS:
            return "declarations";
        case XG_EVIDENCE_CACHE_SEMANTIC_GRAPH:
            return "semantic_graph";
        case XG_EVIDENCE_CACHE_BODY_SUMMARY:
            return "body_summary";
        case XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE:
            return "global_evidence";
        default:
            return "unknown";
    }
}

XR_FUNC void xg_global_evidence_init(XgGlobalEvidence *evidence, XgBuildKey key) {
    if (!evidence)
        return;
    memset(evidence, 0, sizeof(*evidence));
    evidence->key = key;
}

XR_FUNC void xg_global_evidence_free(XgGlobalEvidence *evidence) {
    if (!evidence)
        return;
    xr_free(evidence->modules);
    xr_free(evidence->decls);
    xr_free(evidence->classes);
    xr_free(evidence->class_fields);
    xr_free(evidence->methods);
    xr_free(evidence->interface_impls);
    xr_free(evidence->interface_extends);
    xr_free(evidence->interface_methods);
    xr_free(evidence->interface_object_uses);
    xr_free(evidence->bodies);
    xr_free(evidence->param_storages);
    xr_free(evidence->callsites);
    xr_free(evidence->link_deps);
    xr_free(evidence->generic_insts);
    xr_free(evidence->generic_body_uses);
    xr_free(evidence->generic_storages);
    xr_free(evidence->generic_code_sizes);
    xr_free(evidence->sequence_accesses);
    xr_free(evidence->capacity_ops);
    xr_free(evidence->bulk_ops);
    xr_free(evidence->encoding_ops);
    xr_free(evidence->derives);
    xr_free(evidence->derived_fields);
    xr_free(evidence->derived_methods);
    xr_free(evidence->json_shapes);
    xr_free(evidence->json_fields);
    xr_free(evidence->json_accesses);
    xr_free(evidence->json_codecs);
    xr_free(evidence->record_shapes);
    xr_free(evidence->record_fields);
    xr_free(evidence->record_accesses);
    xr_free(evidence->record_merges);
    xr_free(evidence->options_bags);
    xr_free(evidence->map_shapes);
    xr_free(evidence->map_entries);
    xr_free(evidence->key_accesses);
    xr_free(evidence->hash_eqs);
    memset(evidence, 0, sizeof(*evidence));
}

static bool xg_global_evidence_clone(XgGlobalEvidence *out, const XgGlobalEvidence *src) {
    if (!out || !src)
        return false;
    memset(out, 0, sizeof(*out));
    out->key = src->key;

#define XG_CLONE_ARRAY(field, count_field, cap_field)                                              \
    do {                                                                                           \
        out->count_field = src->count_field;                                                       \
        if (src->count_field > 0) {                                                                \
            if (!src->field)                                                                       \
                goto fail;                                                                         \
            out->field = xr_malloc((size_t) src->count_field * sizeof(*out->field));               \
            if (!out->field)                                                                       \
                goto fail;                                                                         \
            memcpy(out->field, src->field, (size_t) src->count_field * sizeof(*out->field));       \
            out->cap_field = src->count_field;                                                     \
        }                                                                                          \
    } while (0)

    XG_CLONE_ARRAY(modules, nmodules, module_cap);
    XG_CLONE_ARRAY(decls, ndecls, decl_cap);
    XG_CLONE_ARRAY(classes, nclasses, class_cap);
    XG_CLONE_ARRAY(class_fields, nclass_fields, class_field_cap);
    XG_CLONE_ARRAY(methods, nmethods, method_cap);
    XG_CLONE_ARRAY(interface_impls, ninterface_impls, interface_impl_cap);
    XG_CLONE_ARRAY(interface_extends, ninterface_extends, interface_extend_cap);
    XG_CLONE_ARRAY(interface_methods, ninterface_methods, interface_method_cap);
    XG_CLONE_ARRAY(interface_object_uses, ninterface_object_uses, interface_object_use_cap);
    XG_CLONE_ARRAY(bodies, nbodies, body_cap);
    XG_CLONE_ARRAY(param_storages, nparam_storages, param_storage_cap);
    XG_CLONE_ARRAY(callsites, ncallsites, callsite_cap);
    XG_CLONE_ARRAY(link_deps, nlink_deps, link_dep_cap);
    XG_CLONE_ARRAY(generic_insts, ngeneric_insts, generic_inst_cap);
    XG_CLONE_ARRAY(generic_body_uses, ngeneric_body_uses, generic_body_use_cap);
    XG_CLONE_ARRAY(generic_storages, ngeneric_storages, generic_storage_cap);
    XG_CLONE_ARRAY(generic_code_sizes, ngeneric_code_sizes, generic_code_size_cap);
    XG_CLONE_ARRAY(sequence_accesses, nsequence_accesses, sequence_access_cap);
    XG_CLONE_ARRAY(capacity_ops, ncapacity_ops, capacity_op_cap);
    XG_CLONE_ARRAY(bulk_ops, nbulk_ops, bulk_op_cap);
    XG_CLONE_ARRAY(encoding_ops, nencoding_ops, encoding_op_cap);
    XG_CLONE_ARRAY(derives, nderives, derive_cap);
    XG_CLONE_ARRAY(derived_fields, nderived_fields, derived_field_cap);
    XG_CLONE_ARRAY(derived_methods, nderived_methods, derived_method_cap);
    XG_CLONE_ARRAY(json_shapes, njson_shapes, json_shape_cap);
    XG_CLONE_ARRAY(json_fields, njson_fields, json_field_cap);
    XG_CLONE_ARRAY(json_accesses, njson_accesses, json_access_cap);
    XG_CLONE_ARRAY(json_codecs, njson_codecs, json_codec_cap);
    XG_CLONE_ARRAY(record_shapes, nrecord_shapes, record_shape_cap);
    XG_CLONE_ARRAY(record_fields, nrecord_fields, record_field_cap);
    XG_CLONE_ARRAY(record_accesses, nrecord_accesses, record_access_cap);
    XG_CLONE_ARRAY(record_merges, nrecord_merges, record_merge_cap);
    XG_CLONE_ARRAY(options_bags, noptions_bags, options_bag_cap);
    XG_CLONE_ARRAY(map_shapes, nmap_shapes, map_shape_cap);
    XG_CLONE_ARRAY(map_entries, nmap_entries, map_entry_cap);
    XG_CLONE_ARRAY(key_accesses, nkey_accesses, key_access_cap);
    XG_CLONE_ARRAY(hash_eqs, nhash_eqs, hash_eq_cap);

#undef XG_CLONE_ARRAY

    return true;

fail:
    xg_global_evidence_free(out);
    return false;
}

XR_FUNC bool xg_global_evidence_reserve_modules(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->modules, &evidence->module_cap, capacity,
                                     sizeof(XgModuleSummary));
}

XR_FUNC bool xg_global_evidence_reserve_decls(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->decls, &evidence->decl_cap, capacity,
                                     sizeof(XgDeclSummary));
}

XR_FUNC bool xg_global_evidence_reserve_classes(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->classes, &evidence->class_cap, capacity,
                                     sizeof(XgClassSummary));
}

XR_FUNC bool xg_global_evidence_reserve_class_fields(XgGlobalEvidence *evidence,
                                                     uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->class_fields, &evidence->class_field_cap,
                                     capacity, sizeof(XgClassFieldSummary));
}

XR_FUNC bool xg_global_evidence_reserve_methods(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->methods, &evidence->method_cap, capacity,
                                     sizeof(XgMethodSummary));
}

XR_FUNC bool xg_global_evidence_reserve_interface_impls(XgGlobalEvidence *evidence,
                                                        uint32_t capacity) {
    return evidence &&
           reserve_array((void **) &evidence->interface_impls, &evidence->interface_impl_cap,
                         capacity, sizeof(XgInterfaceImplSummary));
}

XR_FUNC bool xg_global_evidence_reserve_interface_extends(XgGlobalEvidence *evidence,
                                                          uint32_t capacity) {
    return evidence &&
           reserve_array((void **) &evidence->interface_extends, &evidence->interface_extend_cap,
                         capacity, sizeof(XgInterfaceExtendsSummary));
}

XR_FUNC bool xg_global_evidence_reserve_interface_methods(XgGlobalEvidence *evidence,
                                                          uint32_t capacity) {
    return evidence &&
           reserve_array((void **) &evidence->interface_methods, &evidence->interface_method_cap,
                         capacity, sizeof(XgInterfaceMethodSummary));
}

XR_FUNC bool xg_global_evidence_reserve_interface_object_uses(XgGlobalEvidence *evidence,
                                                              uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->interface_object_uses,
                                     &evidence->interface_object_use_cap, capacity,
                                     sizeof(XgInterfaceObjectUseSummary));
}

XR_FUNC bool xg_global_evidence_reserve_bodies(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->bodies, &evidence->body_cap, capacity,
                                     sizeof(XgBodySummary));
}

XR_FUNC bool xg_global_evidence_reserve_param_storages(XgGlobalEvidence *evidence,
                                                       uint32_t capacity) {
    return evidence &&
           reserve_array((void **) &evidence->param_storages, &evidence->param_storage_cap,
                         capacity, sizeof(XgParamStorageSummary));
}

XR_FUNC bool xg_global_evidence_reserve_callsites(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->callsites, &evidence->callsite_cap,
                                     capacity, sizeof(XgCallsiteSummary));
}

XR_FUNC bool xg_global_evidence_reserve_link_deps(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->link_deps, &evidence->link_dep_cap,
                                     capacity, sizeof(XgLinkDependencySummary));
}

XR_FUNC bool xg_global_evidence_reserve_generic_insts(XgGlobalEvidence *evidence,
                                                      uint32_t capacity) {
    return evidence &&
           reserve_array((void **) &evidence->generic_insts, &evidence->generic_inst_cap, capacity,
                         sizeof(XgGenericInstSummary));
}

XR_FUNC bool xg_global_evidence_reserve_generic_body_uses(XgGlobalEvidence *evidence,
                                                          uint32_t capacity) {
    return evidence &&
           reserve_array((void **) &evidence->generic_body_uses, &evidence->generic_body_use_cap,
                         capacity, sizeof(XgGenericBodyUseSummary));
}

XR_FUNC bool xg_global_evidence_reserve_generic_storages(XgGlobalEvidence *evidence,
                                                         uint32_t capacity) {
    return evidence &&
           reserve_array((void **) &evidence->generic_storages, &evidence->generic_storage_cap,
                         capacity, sizeof(XgGenericStorageSummary));
}

XR_FUNC bool xg_global_evidence_reserve_generic_code_sizes(XgGlobalEvidence *evidence,
                                                           uint32_t capacity) {
    return evidence &&
           reserve_array((void **) &evidence->generic_code_sizes, &evidence->generic_code_size_cap,
                         capacity, sizeof(XgGenericCodeSizeSummary));
}

XR_FUNC bool xg_global_evidence_reserve_sequence_accesses(XgGlobalEvidence *evidence,
                                                          uint32_t capacity) {
    return evidence &&
           reserve_array((void **) &evidence->sequence_accesses, &evidence->sequence_access_cap,
                         capacity, sizeof(XgSequenceAccessSummary));
}

XR_FUNC bool xg_global_evidence_reserve_capacity_ops(XgGlobalEvidence *evidence,
                                                     uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->capacity_ops, &evidence->capacity_op_cap,
                                     capacity, sizeof(XgCapacityOpSummary));
}

XR_FUNC bool xg_global_evidence_reserve_bulk_ops(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->bulk_ops, &evidence->bulk_op_cap,
                                     capacity, sizeof(XgBulkOpSummary));
}

XR_FUNC bool xg_global_evidence_reserve_encoding_ops(XgGlobalEvidence *evidence,
                                                     uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->encoding_ops, &evidence->encoding_op_cap,
                                     capacity, sizeof(XgEncodingOpSummary));
}

XR_FUNC bool xg_global_evidence_reserve_derives(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->derives, &evidence->derive_cap, capacity,
                                     sizeof(XgDeriveSummary));
}

XR_FUNC bool xg_global_evidence_reserve_derived_fields(XgGlobalEvidence *evidence,
                                                       uint32_t capacity) {
    return evidence &&
           reserve_array((void **) &evidence->derived_fields, &evidence->derived_field_cap,
                         capacity, sizeof(XgDerivedFieldSummary));
}

XR_FUNC bool xg_global_evidence_reserve_derived_methods(XgGlobalEvidence *evidence,
                                                        uint32_t capacity) {
    return evidence &&
           reserve_array((void **) &evidence->derived_methods, &evidence->derived_method_cap,
                         capacity, sizeof(XgDerivedMethodSummary));
}

XR_FUNC bool xg_global_evidence_reserve_json_shapes(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->json_shapes, &evidence->json_shape_cap,
                                     capacity, sizeof(XgJsonShapeSummary));
}

XR_FUNC bool xg_global_evidence_reserve_json_fields(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->json_fields, &evidence->json_field_cap,
                                     capacity, sizeof(XgJsonFieldSummary));
}

XR_FUNC bool xg_global_evidence_reserve_json_accesses(XgGlobalEvidence *evidence,
                                                      uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->json_accesses, &evidence->json_access_cap,
                                     capacity, sizeof(XgJsonAccessSummary));
}

XR_FUNC bool xg_global_evidence_reserve_json_codecs(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->json_codecs, &evidence->json_codec_cap,
                                     capacity, sizeof(XgJsonCodecSummary));
}

XR_FUNC bool xg_global_evidence_reserve_record_shapes(XgGlobalEvidence *evidence,
                                                      uint32_t capacity) {
    return evidence &&
           reserve_array((void **) &evidence->record_shapes, &evidence->record_shape_cap, capacity,
                         sizeof(XgRecordShapeSummary));
}

XR_FUNC bool xg_global_evidence_reserve_record_fields(XgGlobalEvidence *evidence,
                                                      uint32_t capacity) {
    return evidence &&
           reserve_array((void **) &evidence->record_fields, &evidence->record_field_cap, capacity,
                         sizeof(XgRecordFieldSummary));
}

XR_FUNC bool xg_global_evidence_reserve_record_accesses(XgGlobalEvidence *evidence,
                                                        uint32_t capacity) {
    return evidence &&
           reserve_array((void **) &evidence->record_accesses, &evidence->record_access_cap,
                         capacity, sizeof(XgRecordAccessSummary));
}

XR_FUNC bool xg_global_evidence_reserve_record_merges(XgGlobalEvidence *evidence,
                                                      uint32_t capacity) {
    return evidence &&
           reserve_array((void **) &evidence->record_merges, &evidence->record_merge_cap, capacity,
                         sizeof(XgRecordMergeSummary));
}

XR_FUNC bool xg_global_evidence_reserve_options_bags(XgGlobalEvidence *evidence,
                                                     uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->options_bags, &evidence->options_bag_cap,
                                     capacity, sizeof(XgOptionsBagSummary));
}

XR_FUNC bool xg_global_evidence_reserve_map_shapes(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->map_shapes, &evidence->map_shape_cap,
                                     capacity, sizeof(XgMapShapeSummary));
}

XR_FUNC bool xg_global_evidence_reserve_map_entries(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->map_entries, &evidence->map_entry_cap,
                                     capacity, sizeof(XgMapEntrySummary));
}

XR_FUNC bool xg_global_evidence_reserve_key_accesses(XgGlobalEvidence *evidence,
                                                     uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->key_accesses, &evidence->key_access_cap,
                                     capacity, sizeof(XgKeyAccessSummary));
}

XR_FUNC bool xg_global_evidence_reserve_hash_eqs(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->hash_eqs, &evidence->hash_eq_cap,
                                     capacity, sizeof(XgHashEqSummary));
}

XR_FUNC XgModuleSummary *xg_global_evidence_add_module(XgGlobalEvidence *evidence,
                                                       const XgModuleSummary *summary) {
    XgModuleSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_modules(evidence, evidence->nmodules + 1))
        return NULL;
    row = &evidence->modules[evidence->nmodules++];
    *row = *summary;
    return row;
}

XR_FUNC XgDeclSummary *xg_global_evidence_add_decl(XgGlobalEvidence *evidence,
                                                   const XgDeclSummary *summary) {
    XgDeclSummary *row;
    if (!evidence || !summary || !xg_global_evidence_reserve_decls(evidence, evidence->ndecls + 1))
        return NULL;
    row = &evidence->decls[evidence->ndecls++];
    *row = *summary;
    return row;
}

XR_FUNC XgClassSummary *xg_global_evidence_add_class(XgGlobalEvidence *evidence,
                                                     const XgClassSummary *summary) {
    XgClassSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_classes(evidence, evidence->nclasses + 1))
        return NULL;
    row = &evidence->classes[evidence->nclasses++];
    *row = *summary;
    return row;
}

XR_FUNC XgClassFieldSummary *
xg_global_evidence_add_class_field(XgGlobalEvidence *evidence, const XgClassFieldSummary *summary) {
    XgClassFieldSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_class_fields(evidence, evidence->nclass_fields + 1))
        return NULL;
    row = &evidence->class_fields[evidence->nclass_fields++];
    *row = *summary;
    return row;
}

XR_FUNC XgMethodSummary *xg_global_evidence_add_method(XgGlobalEvidence *evidence,
                                                       const XgMethodSummary *summary) {
    XgMethodSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_methods(evidence, evidence->nmethods + 1))
        return NULL;
    row = &evidence->methods[evidence->nmethods++];
    *row = *summary;
    if (row->method_id != XG_NO_ID && row->root_method_id == XG_NO_ID)
        row->root_method_id = row->method_id;
    return row;
}

XR_FUNC XgInterfaceImplSummary *
xg_global_evidence_add_interface_impl(XgGlobalEvidence *evidence,
                                      const XgInterfaceImplSummary *summary) {
    XgInterfaceImplSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_interface_impls(evidence, evidence->ninterface_impls + 1))
        return NULL;
    row = &evidence->interface_impls[evidence->ninterface_impls++];
    *row = *summary;
    return row;
}

XR_FUNC XgInterfaceExtendsSummary *
xg_global_evidence_add_interface_extends(XgGlobalEvidence *evidence,
                                         const XgInterfaceExtendsSummary *summary) {
    XgInterfaceExtendsSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_interface_extends(evidence, evidence->ninterface_extends + 1))
        return NULL;
    row = &evidence->interface_extends[evidence->ninterface_extends++];
    *row = *summary;
    return row;
}

XR_FUNC XgInterfaceMethodSummary *
xg_global_evidence_add_interface_method(XgGlobalEvidence *evidence,
                                        const XgInterfaceMethodSummary *summary) {
    XgInterfaceMethodSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_interface_methods(evidence, evidence->ninterface_methods + 1))
        return NULL;
    row = &evidence->interface_methods[evidence->ninterface_methods++];
    *row = *summary;
    return row;
}

XR_FUNC XgInterfaceObjectUseSummary *
xg_global_evidence_add_interface_object_use(XgGlobalEvidence *evidence,
                                            const XgInterfaceObjectUseSummary *summary) {
    XgInterfaceObjectUseSummary *row;
    if (!evidence || !summary || summary->interface_id == XG_NO_ID || summary->reason == 0 ||
        !xg_global_evidence_reserve_interface_object_uses(evidence,
                                                          evidence->ninterface_object_uses + 1))
        return NULL;
    row = &evidence->interface_object_uses[evidence->ninterface_object_uses++];
    *row = *summary;
    if (row->use_id == XG_NO_ID)
        row->use_id = evidence->ninterface_object_uses;
    return row;
}

XR_FUNC XgBodySummary *xg_global_evidence_add_body(XgGlobalEvidence *evidence,
                                                   const XgBodySummary *summary) {
    XgBodySummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_bodies(evidence, evidence->nbodies + 1))
        return NULL;
    row = &evidence->bodies[evidence->nbodies++];
    *row = *summary;
    return row;
}

XR_FUNC XgParamStorageSummary *
xg_global_evidence_add_param_storage(XgGlobalEvidence *evidence,
                                     const XgParamStorageSummary *summary) {
    XgParamStorageSummary *row;
    if (!evidence || !summary || summary->owner_func_id == XG_NO_ID ||
        !xg_global_evidence_reserve_param_storages(evidence, evidence->nparam_storages + 1))
        return NULL;
    row = &evidence->param_storages[evidence->nparam_storages++];
    *row = *summary;
    if (row->requirement_id == XG_NO_ID)
        row->requirement_id = evidence->nparam_storages;
    return row;
}

XR_FUNC XgCallsiteSummary *xg_global_evidence_add_callsite(XgGlobalEvidence *evidence,
                                                           const XgCallsiteSummary *summary) {
    XgCallsiteSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_callsites(evidence, evidence->ncallsites + 1))
        return NULL;
    row = &evidence->callsites[evidence->ncallsites++];
    *row = *summary;
    return row;
}

XR_FUNC XgLinkDependencySummary *
xg_global_evidence_add_link_dependency(XgGlobalEvidence *evidence,
                                       const XgLinkDependencySummary *summary) {
    XgLinkDependencySummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_link_deps(evidence, evidence->nlink_deps + 1))
        return NULL;
    row = &evidence->link_deps[evidence->nlink_deps++];
    *row = *summary;
    row->name[XG_LINK_DEP_NAME_MAX - 1] = '\0';
    return row;
}

XR_FUNC XgGenericInstSummary *
xg_global_evidence_add_generic_inst(XgGlobalEvidence *evidence,
                                    const XgGenericInstSummary *summary) {
    XgGenericInstSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_generic_insts(evidence, evidence->ngeneric_insts + 1))
        return NULL;
    row = &evidence->generic_insts[evidence->ngeneric_insts++];
    *row = *summary;
    return row;
}

XR_FUNC XgGenericBodyUseSummary *
xg_global_evidence_add_generic_body_use(XgGlobalEvidence *evidence,
                                        const XgGenericBodyUseSummary *summary) {
    XgGenericBodyUseSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_generic_body_uses(evidence, evidence->ngeneric_body_uses + 1))
        return NULL;
    row = &evidence->generic_body_uses[evidence->ngeneric_body_uses++];
    *row = *summary;
    return row;
}

XR_FUNC XgGenericStorageSummary *
xg_global_evidence_add_generic_storage(XgGlobalEvidence *evidence,
                                       const XgGenericStorageSummary *summary) {
    XgGenericStorageSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_generic_storages(evidence, evidence->ngeneric_storages + 1))
        return NULL;
    row = &evidence->generic_storages[evidence->ngeneric_storages++];
    *row = *summary;
    return row;
}

XR_FUNC XgGenericCodeSizeSummary *
xg_global_evidence_add_generic_code_size(XgGlobalEvidence *evidence,
                                         const XgGenericCodeSizeSummary *summary) {
    XgGenericCodeSizeSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_generic_code_sizes(evidence, evidence->ngeneric_code_sizes + 1))
        return NULL;
    row = &evidence->generic_code_sizes[evidence->ngeneric_code_sizes++];
    *row = *summary;
    return row;
}

XR_FUNC XgSequenceAccessSummary *
xg_global_evidence_add_sequence_access(XgGlobalEvidence *evidence,
                                       const XgSequenceAccessSummary *summary) {
    XgSequenceAccessSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_sequence_accesses(evidence, evidence->nsequence_accesses + 1))
        return NULL;
    row = &evidence->sequence_accesses[evidence->nsequence_accesses++];
    *row = *summary;
    return row;
}

XR_FUNC XgCapacityOpSummary *
xg_global_evidence_add_capacity_op(XgGlobalEvidence *evidence, const XgCapacityOpSummary *summary) {
    XgCapacityOpSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_capacity_ops(evidence, evidence->ncapacity_ops + 1))
        return NULL;
    row = &evidence->capacity_ops[evidence->ncapacity_ops++];
    *row = *summary;
    return row;
}

XR_FUNC XgBulkOpSummary *xg_global_evidence_add_bulk_op(XgGlobalEvidence *evidence,
                                                        const XgBulkOpSummary *summary) {
    XgBulkOpSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_bulk_ops(evidence, evidence->nbulk_ops + 1))
        return NULL;
    row = &evidence->bulk_ops[evidence->nbulk_ops++];
    *row = *summary;
    return row;
}

XR_FUNC XgEncodingOpSummary *
xg_global_evidence_add_encoding_op(XgGlobalEvidence *evidence, const XgEncodingOpSummary *summary) {
    XgEncodingOpSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_encoding_ops(evidence, evidence->nencoding_ops + 1))
        return NULL;
    row = &evidence->encoding_ops[evidence->nencoding_ops++];
    *row = *summary;
    return row;
}

XR_FUNC XgDeriveSummary *xg_global_evidence_add_derive(XgGlobalEvidence *evidence,
                                                       const XgDeriveSummary *summary) {
    XgDeriveSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_derives(evidence, evidence->nderives + 1))
        return NULL;
    row = &evidence->derives[evidence->nderives++];
    *row = *summary;
    return row;
}

XR_FUNC XgDerivedFieldSummary *
xg_global_evidence_add_derived_field(XgGlobalEvidence *evidence,
                                     const XgDerivedFieldSummary *summary) {
    XgDerivedFieldSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_derived_fields(evidence, evidence->nderived_fields + 1))
        return NULL;
    row = &evidence->derived_fields[evidence->nderived_fields++];
    *row = *summary;
    return row;
}

XR_FUNC XgDerivedMethodSummary *
xg_global_evidence_add_derived_method(XgGlobalEvidence *evidence,
                                      const XgDerivedMethodSummary *summary) {
    XgDerivedMethodSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_derived_methods(evidence, evidence->nderived_methods + 1))
        return NULL;
    row = &evidence->derived_methods[evidence->nderived_methods++];
    *row = *summary;
    return row;
}

XR_FUNC XgJsonShapeSummary *xg_global_evidence_add_json_shape(XgGlobalEvidence *evidence,
                                                              const XgJsonShapeSummary *summary) {
    XgJsonShapeSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_json_shapes(evidence, evidence->njson_shapes + 1))
        return NULL;
    row = &evidence->json_shapes[evidence->njson_shapes++];
    *row = *summary;
    return row;
}

XR_FUNC XgJsonFieldSummary *xg_global_evidence_add_json_field(XgGlobalEvidence *evidence,
                                                              const XgJsonFieldSummary *summary) {
    XgJsonFieldSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_json_fields(evidence, evidence->njson_fields + 1))
        return NULL;
    row = &evidence->json_fields[evidence->njson_fields++];
    *row = *summary;
    return row;
}

XR_FUNC XgJsonAccessSummary *
xg_global_evidence_add_json_access(XgGlobalEvidence *evidence, const XgJsonAccessSummary *summary) {
    XgJsonAccessSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_json_accesses(evidence, evidence->njson_accesses + 1))
        return NULL;
    row = &evidence->json_accesses[evidence->njson_accesses++];
    *row = *summary;
    return row;
}

XR_FUNC XgJsonCodecSummary *xg_global_evidence_add_json_codec(XgGlobalEvidence *evidence,
                                                              const XgJsonCodecSummary *summary) {
    XgJsonCodecSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_json_codecs(evidence, evidence->njson_codecs + 1))
        return NULL;
    row = &evidence->json_codecs[evidence->njson_codecs++];
    *row = *summary;
    return row;
}

XR_FUNC XgRecordShapeSummary *
xg_global_evidence_add_record_shape(XgGlobalEvidence *evidence,
                                    const XgRecordShapeSummary *summary) {
    XgRecordShapeSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_record_shapes(evidence, evidence->nrecord_shapes + 1))
        return NULL;
    row = &evidence->record_shapes[evidence->nrecord_shapes++];
    *row = *summary;
    return row;
}

XR_FUNC XgRecordFieldSummary *
xg_global_evidence_add_record_field(XgGlobalEvidence *evidence,
                                    const XgRecordFieldSummary *summary) {
    XgRecordFieldSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_record_fields(evidence, evidence->nrecord_fields + 1))
        return NULL;
    row = &evidence->record_fields[evidence->nrecord_fields++];
    *row = *summary;
    return row;
}

XR_FUNC XgRecordAccessSummary *
xg_global_evidence_add_record_access(XgGlobalEvidence *evidence,
                                     const XgRecordAccessSummary *summary) {
    XgRecordAccessSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_record_accesses(evidence, evidence->nrecord_accesses + 1))
        return NULL;
    row = &evidence->record_accesses[evidence->nrecord_accesses++];
    *row = *summary;
    return row;
}

XR_FUNC XgRecordMergeSummary *
xg_global_evidence_add_record_merge(XgGlobalEvidence *evidence,
                                    const XgRecordMergeSummary *summary) {
    XgRecordMergeSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_record_merges(evidence, evidence->nrecord_merges + 1))
        return NULL;
    row = &evidence->record_merges[evidence->nrecord_merges++];
    *row = *summary;
    return row;
}

XR_FUNC XgOptionsBagSummary *
xg_global_evidence_add_options_bag(XgGlobalEvidence *evidence, const XgOptionsBagSummary *summary) {
    XgOptionsBagSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_options_bags(evidence, evidence->noptions_bags + 1))
        return NULL;
    row = &evidence->options_bags[evidence->noptions_bags++];
    *row = *summary;
    return row;
}

XR_FUNC XgMapShapeSummary *xg_global_evidence_add_map_shape(XgGlobalEvidence *evidence,
                                                            const XgMapShapeSummary *summary) {
    XgMapShapeSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_map_shapes(evidence, evidence->nmap_shapes + 1))
        return NULL;
    row = &evidence->map_shapes[evidence->nmap_shapes++];
    *row = *summary;
    return row;
}

XR_FUNC XgMapEntrySummary *xg_global_evidence_add_map_entry(XgGlobalEvidence *evidence,
                                                            const XgMapEntrySummary *summary) {
    XgMapEntrySummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_map_entries(evidence, evidence->nmap_entries + 1))
        return NULL;
    row = &evidence->map_entries[evidence->nmap_entries++];
    *row = *summary;
    return row;
}

XR_FUNC XgKeyAccessSummary *xg_global_evidence_add_key_access(XgGlobalEvidence *evidence,
                                                              const XgKeyAccessSummary *summary) {
    XgKeyAccessSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_key_accesses(evidence, evidence->nkey_accesses + 1))
        return NULL;
    row = &evidence->key_accesses[evidence->nkey_accesses++];
    *row = *summary;
    return row;
}

XR_FUNC XgHashEqSummary *xg_global_evidence_add_hash_eq(XgGlobalEvidence *evidence,
                                                        const XgHashEqSummary *summary) {
    XgHashEqSummary *row;
    if (!evidence || !summary ||
        !xg_global_evidence_reserve_hash_eqs(evidence, evidence->nhash_eqs + 1))
        return NULL;
    row = &evidence->hash_eqs[evidence->nhash_eqs++];
    *row = *summary;
    return row;
}

XR_FUNC const XgClassFieldSummary *
xg_global_evidence_find_class_field(const XgGlobalEvidence *evidence, XgFieldId field_id) {
    if (!evidence || field_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->nclass_fields; i++) {
        if (evidence->class_fields[i].field_id == field_id)
            return &evidence->class_fields[i];
    }
    return NULL;
}

XR_FUNC const XgCallsiteSummary *xg_global_evidence_find_callsite(const XgGlobalEvidence *evidence,
                                                                  XgCallsiteId callsite_id) {
    if (!evidence || callsite_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->ncallsites; i++) {
        if (evidence->callsites[i].callsite_id == callsite_id)
            return &evidence->callsites[i];
    }
    return NULL;
}

XR_FUNC const XgGenericInstSummary *
xg_global_evidence_find_generic_inst(const XgGlobalEvidence *evidence,
                                     XgGenericInstId generic_inst_id) {
    if (!evidence || generic_inst_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->ngeneric_insts; i++) {
        if (evidence->generic_insts[i].generic_inst_id == generic_inst_id)
            return &evidence->generic_insts[i];
    }
    return NULL;
}

XR_FUNC const XgGenericBodyUseSummary *
xg_global_evidence_find_generic_body_use(const XgGlobalEvidence *evidence,
                                         XgGenericBodyUseId use_id) {
    if (!evidence || use_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->ngeneric_body_uses; i++) {
        if (evidence->generic_body_uses[i].use_id == use_id)
            return &evidence->generic_body_uses[i];
    }
    return NULL;
}

XR_FUNC const XgGenericStorageSummary *
xg_global_evidence_find_generic_storage(const XgGlobalEvidence *evidence,
                                        XgGenericStorageId storage_id) {
    if (!evidence || storage_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->ngeneric_storages; i++) {
        if (evidence->generic_storages[i].storage_id == storage_id)
            return &evidence->generic_storages[i];
    }
    return NULL;
}

XR_FUNC const XgGenericCodeSizeSummary *
xg_global_evidence_find_generic_code_size(const XgGlobalEvidence *evidence,
                                          XgGenericCodeSizeId code_size_id) {
    if (!evidence || code_size_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->ngeneric_code_sizes; i++) {
        if (evidence->generic_code_sizes[i].code_size_id == code_size_id)
            return &evidence->generic_code_sizes[i];
    }
    return NULL;
}

XR_FUNC const XgSequenceAccessSummary *
xg_global_evidence_find_sequence_access(const XgGlobalEvidence *evidence,
                                        XgSequenceAccessId access_id) {
    if (!evidence || access_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->nsequence_accesses; i++) {
        if (evidence->sequence_accesses[i].access_id == access_id)
            return &evidence->sequence_accesses[i];
    }
    return NULL;
}

XR_FUNC const XgCapacityOpSummary *
xg_global_evidence_find_capacity_op(const XgGlobalEvidence *evidence, XgCapacityOpId op_id) {
    if (!evidence || op_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->ncapacity_ops; i++) {
        if (evidence->capacity_ops[i].op_id == op_id)
            return &evidence->capacity_ops[i];
    }
    return NULL;
}

XR_FUNC const XgBulkOpSummary *xg_global_evidence_find_bulk_op(const XgGlobalEvidence *evidence,
                                                               XgBulkOpId op_id) {
    if (!evidence || op_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->nbulk_ops; i++) {
        if (evidence->bulk_ops[i].op_id == op_id)
            return &evidence->bulk_ops[i];
    }
    return NULL;
}

XR_FUNC const XgEncodingOpSummary *
xg_global_evidence_find_encoding_op(const XgGlobalEvidence *evidence, XgEncodingOpId op_id) {
    if (!evidence || op_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->nencoding_ops; i++) {
        if (evidence->encoding_ops[i].op_id == op_id)
            return &evidence->encoding_ops[i];
    }
    return NULL;
}

XR_FUNC const XgJsonShapeSummary *
xg_global_evidence_find_json_shape(const XgGlobalEvidence *evidence, XgJsonShapeId json_shape_id) {
    if (!evidence || json_shape_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->njson_shapes; i++) {
        if (evidence->json_shapes[i].json_shape_id == json_shape_id)
            return &evidence->json_shapes[i];
    }
    return NULL;
}

XR_FUNC const XgJsonFieldSummary *
xg_global_evidence_find_json_field(const XgGlobalEvidence *evidence, XgJsonFieldId field_id) {
    if (!evidence || field_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->njson_fields; i++) {
        if (evidence->json_fields[i].field_id == field_id)
            return &evidence->json_fields[i];
    }
    return NULL;
}

XR_FUNC const XgJsonCodecSummary *
xg_global_evidence_find_json_codec(const XgGlobalEvidence *evidence, XgJsonCodecId codec_id) {
    if (!evidence || codec_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->njson_codecs; i++) {
        if (evidence->json_codecs[i].codec_id == codec_id)
            return &evidence->json_codecs[i];
    }
    return NULL;
}

XR_FUNC const XgRecordShapeSummary *
xg_global_evidence_find_record_shape(const XgGlobalEvidence *evidence,
                                     XgRecordShapeId record_shape_id) {
    if (!evidence || record_shape_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->nrecord_shapes; i++) {
        if (evidence->record_shapes[i].record_shape_id == record_shape_id)
            return &evidence->record_shapes[i];
    }
    return NULL;
}

XR_FUNC const XgRecordFieldSummary *
xg_global_evidence_find_record_field(const XgGlobalEvidence *evidence, XgRecordFieldId field_id) {
    if (!evidence || field_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->nrecord_fields; i++) {
        if (evidence->record_fields[i].field_id == field_id)
            return &evidence->record_fields[i];
    }
    return NULL;
}

XR_FUNC const XgRecordMergeSummary *
xg_global_evidence_find_record_merge(const XgGlobalEvidence *evidence,
                                     XgRecordMergeId record_merge_id) {
    if (!evidence || record_merge_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->nrecord_merges; i++) {
        if (evidence->record_merges[i].merge_id == record_merge_id)
            return &evidence->record_merges[i];
    }
    return NULL;
}

XR_FUNC const XgOptionsBagSummary *
xg_global_evidence_find_options_bag(const XgGlobalEvidence *evidence, XgOptionsId options_id) {
    if (!evidence || options_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->noptions_bags; i++) {
        if (evidence->options_bags[i].options_id == options_id)
            return &evidence->options_bags[i];
    }
    return NULL;
}

XR_FUNC const XgMapShapeSummary *xg_global_evidence_find_map_shape(const XgGlobalEvidence *evidence,
                                                                   XgMapShapeId shape_id) {
    if (!evidence || shape_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->nmap_shapes; i++) {
        if (evidence->map_shapes[i].shape_id == shape_id)
            return &evidence->map_shapes[i];
    }
    return NULL;
}

XR_FUNC const XgHashEqSummary *xg_global_evidence_find_hash_eq(const XgGlobalEvidence *evidence,
                                                               uint32_t type_key) {
    if (!evidence || type_key == 0)
        return NULL;
    for (uint32_t i = 0; i < evidence->nhash_eqs; i++) {
        if (evidence->hash_eqs[i].type_key == type_key)
            return &evidence->hash_eqs[i];
    }
    return NULL;
}

static bool xg_global_evidence_find_body_index_by_func(const XgGlobalEvidence *evidence,
                                                       XgFuncId func_id, uint32_t *out_index) {
    if (!evidence || func_id == XG_NO_ID)
        return false;
    for (uint32_t i = 0; i < evidence->nbodies; i++) {
        if (evidence->bodies[i].func_id == func_id) {
            if (out_index)
                *out_index = i;
            return true;
        }
    }
    return false;
}

static bool xg_global_evidence_find_body_index_by_method(const XgGlobalEvidence *evidence,
                                                         XgMethodId method_id,
                                                         uint32_t *out_index) {
    if (!evidence || method_id == XG_NO_ID)
        return false;
    for (uint32_t i = 0; i < evidence->nbodies; i++) {
        if (evidence->bodies[i].kind == XG_BODY_METHOD &&
            evidence->bodies[i].owner_method_id == method_id) {
            if (out_index)
                *out_index = i;
            return true;
        }
    }
    return false;
}

static const XgClassSummary *xg_global_evidence_find_class(const XgGlobalEvidence *evidence,
                                                           XgClassId class_id) {
    if (!evidence || class_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->nclasses; i++) {
        if (evidence->classes[i].class_id == class_id)
            return &evidence->classes[i];
    }
    return NULL;
}

static bool xg_global_evidence_class_is_descendant_or_self(const XgGlobalEvidence *evidence,
                                                           XgClassId class_id,
                                                           XgClassId ancestor_id) {
    const XgClassSummary *cls;
    uint8_t depth = 0;
    if (!evidence || class_id == XG_NO_ID || ancestor_id == XG_NO_ID)
        return false;
    if (class_id == ancestor_id)
        return true;
    cls = xg_global_evidence_find_class(evidence, class_id);
    while (cls && cls->parent_class_id != XG_NO_ID && depth++ < 64) {
        if (cls->parent_class_id == ancestor_id)
            return true;
        cls = xg_global_evidence_find_class(evidence, cls->parent_class_id);
    }
    return false;
}

static const XgMethodSummary *xg_global_evidence_find_method(const XgGlobalEvidence *evidence,
                                                             XgMethodId method_id) {
    if (!evidence || method_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->nmethods; i++) {
        if (evidence->methods[i].method_id == method_id)
            return &evidence->methods[i];
    }
    return NULL;
}

static const XgMethodSummary *
xg_global_evidence_find_method_by_signature_in_class(const XgGlobalEvidence *evidence,
                                                     const XgClassSummary *cls, uint32_t name_id,
                                                     uint32_t signature_key) {
    if (!evidence || !cls || cls->method_start == 0 || name_id == 0)
        return NULL;
    for (uint32_t i = 0; i < cls->method_count; i++) {
        uint32_t idx = cls->method_start - 1 + i;
        const XgMethodSummary *method = idx < evidence->nmethods ? &evidence->methods[idx] : NULL;
        if (method && method->owner_class_id == cls->class_id && method->name_id == name_id &&
            method->signature_key == signature_key && (method->flags & XG_METHOD_STATIC) == 0 &&
            (method->flags & XG_METHOD_CONSTRUCTOR) == 0)
            return method;
    }
    return NULL;
}

static const XgMethodSummary *
xg_global_evidence_find_method_by_signature_in_hierarchy(const XgGlobalEvidence *evidence,
                                                         XgClassId class_id, uint32_t name_id,
                                                         uint32_t signature_key) {
    const XgClassSummary *cls = xg_global_evidence_find_class(evidence, class_id);
    while (cls) {
        const XgMethodSummary *method = xg_global_evidence_find_method_by_signature_in_class(
            evidence, cls, name_id, signature_key);
        if (method)
            return method;
        if (cls->parent_class_id == XG_NO_ID)
            break;
        cls = xg_global_evidence_find_class(evidence, cls->parent_class_id);
    }
    return NULL;
}

static bool xg_global_evidence_interface_extends_reaches(const XgGlobalEvidence *evidence,
                                                         XgInterfaceId from, XgInterfaceId target,
                                                         uint32_t depth) {
    if (!evidence || from == XG_NO_ID || target == XG_NO_ID || depth > 64)
        return false;
    if (from == target)
        return true;
    for (uint32_t i = 0; i < evidence->ninterface_extends; i++) {
        const XgInterfaceExtendsSummary *edge = &evidence->interface_extends[i];
        if (edge->child_interface_id != from)
            continue;
        if (xg_global_evidence_interface_extends_reaches(evidence, edge->parent_interface_id,
                                                         target, depth + 1))
            return true;
    }
    return false;
}

static bool xg_global_evidence_interface_impl_matches(const XgGlobalEvidence *evidence,
                                                      XgInterfaceId implementor_interface,
                                                      XgInterfaceId receiver_interface) {
    return implementor_interface == receiver_interface ||
           xg_global_evidence_interface_extends_reaches(evidence, implementor_interface,
                                                        receiver_interface, 0);
}

static bool xg_global_evidence_effective_interface_implementor_seen(
    const XgGlobalEvidence *evidence, XgInterfaceId receiver_interface, XgClassId implementor_class,
    uint32_t upto_index) {
    if (!evidence || receiver_interface == XG_NO_ID || implementor_class == XG_NO_ID)
        return false;
    for (uint32_t i = 0; i < upto_index && i < evidence->ninterface_impls; i++) {
        const XgInterfaceImplSummary *impl = &evidence->interface_impls[i];
        if (impl->implementor_class_id == implementor_class &&
            xg_global_evidence_interface_impl_matches(evidence, impl->interface_id,
                                                      receiver_interface))
            return true;
    }
    return false;
}

static bool
xg_global_evidence_interface_method_visible_from(const XgGlobalEvidence *evidence,
                                                 XgInterfaceId receiver_interface_id,
                                                 const XgInterfaceMethodSummary *method) {
    if (!evidence || receiver_interface_id == XG_NO_ID || !method)
        return false;
    return xg_global_evidence_interface_extends_reaches(evidence, receiver_interface_id,
                                                        method->owner_interface_id, 0);
}

static const XgInterfaceMethodSummary *
xg_global_evidence_find_visible_interface_method(const XgGlobalEvidence *evidence,
                                                 XgInterfaceId receiver_interface_id,
                                                 uint32_t name_id, uint32_t signature_key) {
    if (!evidence || receiver_interface_id == XG_NO_ID || name_id == 0 || signature_key == 0)
        return NULL;
    for (uint32_t i = 0; i < evidence->ninterface_methods; i++) {
        const XgInterfaceMethodSummary *method = &evidence->interface_methods[i];
        if (!xg_global_evidence_interface_method_visible_from(evidence, receiver_interface_id,
                                                              method))
            continue;
        if (method->name_id == name_id && method->signature_key == signature_key)
            return method;
    }
    return NULL;
}

XR_FUNC bool xg_global_evidence_interface_dispatch_slot(const XgGlobalEvidence *evidence,
                                                        XgInterfaceId receiver_interface_id,
                                                        XgInterfaceMethodId interface_method_id,
                                                        uint32_t *out_slot) {
    uint32_t slot = 0;
    if (out_slot)
        *out_slot = UINT32_MAX;
    if (!evidence || receiver_interface_id == XG_NO_ID || interface_method_id == XG_NO_ID ||
        !out_slot)
        return false;
    for (uint32_t i = 0; i < evidence->ninterface_methods; i++) {
        const XgInterfaceMethodSummary *method = &evidence->interface_methods[i];
        if (!xg_global_evidence_interface_method_visible_from(evidence, receiver_interface_id,
                                                              method))
            continue;
        if (method->interface_method_id == interface_method_id) {
            *out_slot = slot;
            return true;
        }
        slot++;
    }
    return false;
}

static bool xg_method_callsite_is_direct_dispatch(const XgGlobalEvidence *evidence,
                                                  const XgCallsiteSummary *call) {
    const XgClassSummary *receiver_class;
    const XgMethodSummary *method;

    if (!evidence || !call || call->kind != XG_CALL_METHOD ||
        call->receiver_static_class_id == XG_NO_ID || call->method_id == XG_NO_ID)
        return false;
    receiver_class = xg_global_evidence_find_class(evidence, call->receiver_static_class_id);
    method = xg_global_evidence_find_method(evidence, call->method_id);
    if (!receiver_class || !method || (method->flags & XG_METHOD_NATIVE) != 0)
        return false;
    if ((receiver_class->flags & (XG_CLASS_EXPLICIT_FINAL | XG_CLASS_INFERRED_FINAL)) != 0)
        return true;
    return (method->flags & XG_METHOD_OVERRIDDEN) == 0;
}

static bool xg_interface_callsite_direct_target_method(const XgGlobalEvidence *evidence,
                                                       const XgCallsiteSummary *call,
                                                       XgMethodId *out_method_id) {
    const XgInterfaceMethodSummary *interface_method;
    XgClassId target_class_id = XG_NO_ID;
    uint32_t implementor_count = 0;
    const XgMethodSummary *target_method;

    if (!evidence || !call || !out_method_id || call->kind != XG_CALL_INTERFACE ||
        call->receiver_static_interface_id == XG_NO_ID || call->method_id == XG_NO_ID)
        return false;
    interface_method = xg_global_evidence_find_visible_interface_method(
        evidence, call->receiver_static_interface_id, call->method_name_id,
        call->method_signature_key);
    if (!interface_method || interface_method->interface_method_id != call->method_id)
        return false;
    for (uint32_t i = 0; i < evidence->ninterface_impls; i++) {
        const XgInterfaceImplSummary *impl = &evidence->interface_impls[i];
        if (!xg_global_evidence_interface_impl_matches(evidence, impl->interface_id,
                                                       call->receiver_static_interface_id))
            continue;
        if (xg_global_evidence_effective_interface_implementor_seen(
                evidence, call->receiver_static_interface_id, impl->implementor_class_id, i))
            continue;
        implementor_count++;
        target_class_id = impl->implementor_class_id;
        if (implementor_count > 1)
            return false;
    }
    if (implementor_count != 1)
        return false;
    target_method = xg_global_evidence_find_method_by_signature_in_hierarchy(
        evidence, target_class_id, call->method_name_id, call->method_signature_key);
    if (!target_method || (target_method->flags & XG_METHOD_NATIVE) != 0)
        return false;
    *out_method_id = target_method->method_id;
    return true;
}

static bool xg_body_effects_compose_rec(const XgGlobalEvidence *evidence, uint32_t body_index,
                                        uint8_t *state, uint32_t *memo, uint32_t *out_effect_bits);

static bool xg_body_effects_compose_method_target(const XgGlobalEvidence *evidence,
                                                  XgMethodId method_id, uint8_t *state,
                                                  uint32_t *memo, uint32_t *effect_bits) {
    const XgMethodSummary *method;
    uint32_t target_index = 0;
    uint32_t target_effects = 0;
    if (!effect_bits)
        return false;
    method = xg_global_evidence_find_method(evidence, method_id);
    if (method && (method->flags & XG_METHOD_NATIVE) != 0)
        return true;
    if (!xg_global_evidence_find_body_index_by_method(evidence, method_id, &target_index))
        return false;
    if (!xg_body_effects_compose_rec(evidence, target_index, state, memo, &target_effects))
        return false;
    *effect_bits |= target_effects;
    return true;
}

static bool xg_method_callsite_compose_target_set(const XgGlobalEvidence *evidence,
                                                  const XgCallsiteSummary *call, uint8_t *state,
                                                  uint32_t *memo, uint32_t *effect_bits) {
    const XgMethodSummary *method;
    uint32_t target_count = 0;

    if (!evidence || !call || call->kind != XG_CALL_METHOD ||
        call->receiver_static_class_id == XG_NO_ID || call->method_id == XG_NO_ID ||
        call->method_name_id == 0 || call->method_signature_key == 0)
        return false;
    method = xg_global_evidence_find_method(evidence, call->method_id);
    if (!method)
        return false;
    if ((method->flags & XG_METHOD_NATIVE) != 0)
        return true;
    for (uint32_t i = 0; i < evidence->nclasses; i++) {
        const XgClassSummary *candidate = &evidence->classes[i];
        const XgMethodSummary *target_method;
        if (!xg_decl_kind_is_runtime_class(candidate->decl_kind))
            continue;
        if (!xg_global_evidence_class_is_descendant_or_self(evidence, candidate->class_id,
                                                            call->receiver_static_class_id))
            continue;
        target_method = xg_global_evidence_find_method_by_signature_in_hierarchy(
            evidence, candidate->class_id, call->method_name_id, call->method_signature_key);
        if (!target_method)
            return false;
        if ((target_method->flags & XG_METHOD_NATIVE) == 0 &&
            !xg_body_effects_compose_method_target(evidence, target_method->method_id, state, memo,
                                                   effect_bits))
            return false;
        target_count++;
    }
    return target_count > 0;
}

static bool xg_interface_callsite_compose_target_set(const XgGlobalEvidence *evidence,
                                                     const XgCallsiteSummary *call, uint8_t *state,
                                                     uint32_t *memo, uint32_t *effect_bits) {
    const XgInterfaceMethodSummary *interface_method;
    uint32_t target_count = 0;

    if (!evidence || !call || call->kind != XG_CALL_INTERFACE ||
        call->receiver_static_interface_id == XG_NO_ID || call->method_id == XG_NO_ID ||
        call->method_name_id == 0 || call->method_signature_key == 0)
        return false;
    interface_method = xg_global_evidence_find_visible_interface_method(
        evidence, call->receiver_static_interface_id, call->method_name_id,
        call->method_signature_key);
    if (!interface_method || interface_method->interface_method_id != call->method_id)
        return false;
    for (uint32_t i = 0; i < evidence->ninterface_impls; i++) {
        const XgInterfaceImplSummary *impl = &evidence->interface_impls[i];
        const XgMethodSummary *target_method;
        if (!xg_global_evidence_interface_impl_matches(evidence, impl->interface_id,
                                                       call->receiver_static_interface_id))
            continue;
        if (xg_global_evidence_effective_interface_implementor_seen(
                evidence, call->receiver_static_interface_id, impl->implementor_class_id, i))
            continue;
        target_count++;
        target_method = xg_global_evidence_find_method_by_signature_in_hierarchy(
            evidence, impl->implementor_class_id, call->method_name_id, call->method_signature_key);
        if (!target_method)
            return false;
        if ((target_method->flags & XG_METHOD_NATIVE) == 0 &&
            !xg_body_effects_compose_method_target(evidence, target_method->method_id, state, memo,
                                                   effect_bits))
            return false;
    }
    return target_count > 0;
}

static bool xg_body_reachability_mark_rec(const XgGlobalEvidence *evidence, uint32_t body_index,
                                          uint8_t *reachable, uint32_t reachable_count);

static bool xg_body_reachability_mark_method(const XgGlobalEvidence *evidence, XgMethodId method_id,
                                             uint8_t *reachable, uint32_t reachable_count) {
    uint32_t target_index = 0;
    return xg_global_evidence_find_body_index_by_method(evidence, method_id, &target_index) &&
           xg_body_reachability_mark_rec(evidence, target_index, reachable, reachable_count);
}

static bool xg_body_reachability_mark_call(const XgGlobalEvidence *evidence,
                                           const XgCallsiteSummary *call, uint8_t *reachable,
                                           uint32_t reachable_count) {
    uint32_t target_index = 0;
    if (!call)
        return false;
    if (call->kind == XG_CALL_NATIVE || call->kind == XG_CALL_EXTERN)
        return true;
    if (call->kind == XG_CALL_DIRECT_FUNC ||
        (call->kind == XG_CALL_CLOSURE && call->static_target_func_id != XG_NO_ID)) {
        return xg_global_evidence_find_body_index_by_func(evidence, call->static_target_func_id,
                                                          &target_index) &&
               xg_body_reachability_mark_rec(evidence, target_index, reachable, reachable_count);
    }
    if (call->kind == XG_CALL_CLOSURE)
        /* Closure and builtin calls carry their local effect/capability
         * contract on the owner body.  Concrete direct function targets are
         * recorded above; no declaration-tree fallback is permitted here. */
        return true;
    if (xg_method_callsite_is_direct_dispatch(evidence, call))
        return xg_body_reachability_mark_method(evidence, call->method_id, reachable,
                                                reachable_count);
    if (call->kind == XG_CALL_INTERFACE) {
        XgMethodId direct = XG_NO_ID;
        uint32_t target_count = 0;
        if (xg_interface_callsite_direct_target_method(evidence, call, &direct))
            return xg_body_reachability_mark_method(evidence, direct, reachable, reachable_count);
        for (uint32_t i = 0; i < evidence->ninterface_impls; i++) {
            const XgInterfaceImplSummary *impl = &evidence->interface_impls[i];
            const XgMethodSummary *target;
            if (!xg_global_evidence_interface_impl_matches(evidence, impl->interface_id,
                                                           call->receiver_static_interface_id) ||
                xg_global_evidence_effective_interface_implementor_seen(
                    evidence, call->receiver_static_interface_id, impl->implementor_class_id, i))
                continue;
            target = xg_global_evidence_find_method_by_signature_in_hierarchy(
                evidence, impl->implementor_class_id, call->method_name_id,
                call->method_signature_key);
            if (!target)
                return false;
            if ((target->flags & XG_METHOD_NATIVE) == 0 &&
                !xg_body_reachability_mark_method(evidence, target->method_id, reachable,
                                                  reachable_count))
                return false;
            target_count++;
        }
        return target_count > 0;
    }
    if (call->kind == XG_CALL_METHOD) {
        uint32_t target_count = 0;
        for (uint32_t i = 0; i < evidence->nclasses; i++) {
            const XgClassSummary *candidate = &evidence->classes[i];
            const XgMethodSummary *target;
            if (!xg_decl_kind_is_runtime_class(candidate->decl_kind) ||
                !xg_global_evidence_class_is_descendant_or_self(evidence, candidate->class_id,
                                                                call->receiver_static_class_id))
                continue;
            target = xg_global_evidence_find_method_by_signature_in_hierarchy(
                evidence, candidate->class_id, call->method_name_id, call->method_signature_key);
            if (!target)
                return false;
            if ((target->flags & XG_METHOD_NATIVE) == 0 &&
                !xg_body_reachability_mark_method(evidence, target->method_id, reachable,
                                                  reachable_count))
                return false;
            target_count++;
        }
        return target_count > 0;
    }
    return false;
}

static bool xg_body_reachability_mark_rec(const XgGlobalEvidence *evidence, uint32_t body_index,
                                          uint8_t *reachable, uint32_t reachable_count) {
    const XgBodySummary *body;
    if (!evidence || !reachable || body_index >= evidence->nbodies || body_index >= reachable_count)
        return false;
    if (reachable[body_index])
        return true;
    reachable[body_index] = 1;
    body = &evidence->bodies[body_index];
    if ((body->effect_bits & XG_BODY_MAY_CALL) == 0)
        return true;
    if (body->callsite_count == 0)
        return false;
    for (uint32_t i = 0; i < body->callsite_count; i++) {
        const XgCallsiteSummary *call =
            xg_global_evidence_find_callsite(evidence, (XgCallsiteId) (body->callsite_start + i));
        if (!call || call->owner_func_id != body->func_id || call->body_ordinal != i ||
            !xg_body_reachability_mark_call(evidence, call, reachable, reachable_count))
            return false;
    }
    return true;
}

XR_FUNC bool xg_body_reachability_mark_closed_world_calls(const XgGlobalEvidence *evidence,
                                                          XgFuncId root_func_id, uint8_t *reachable,
                                                          uint32_t reachable_count) {
    uint32_t root_index = 0;
    if (!evidence || !reachable || reachable_count < evidence->nbodies ||
        !xg_global_evidence_find_body_index_by_func(evidence, root_func_id, &root_index))
        return false;
    return xg_body_reachability_mark_rec(evidence, root_index, reachable, reachable_count);
}

static bool xg_body_effects_compose_rec(const XgGlobalEvidence *evidence, uint32_t body_index,
                                        uint8_t *state, uint32_t *memo, uint32_t *out_effect_bits) {
    const XgBodySummary *body;
    uint32_t effect_bits;

    if (!evidence || body_index >= evidence->nbodies || !state || !memo || !out_effect_bits)
        return false;
    if (state[body_index] == 1) {
        *out_effect_bits = memo[body_index];
        return true;
    }
    if (state[body_index] == 2) {
        *out_effect_bits = memo[body_index];
        return true;
    }

    state[body_index] = 1;
    body = &evidence->bodies[body_index];
    effect_bits = body->effect_bits & ~XG_BODY_MAY_CALL;
    memo[body_index] = effect_bits;

    if ((body->effect_bits & XG_BODY_MAY_CALL) != 0) {
        if (body->callsite_count == 0)
            return false;
        for (uint32_t i = 0; i < body->callsite_count; i++) {
            const XgCallsiteSummary *call = xg_global_evidence_find_callsite(
                evidence, (XgCallsiteId) (body->callsite_start + i));
            uint32_t target_index = 0;
            uint32_t target_effects = 0;

            if (!call || call->owner_func_id != body->func_id || call->body_ordinal != i)
                return false;
            if (call->kind == XG_CALL_NATIVE || call->kind == XG_CALL_EXTERN)
                continue;
            if (call->kind == XG_CALL_DIRECT_FUNC ||
                (call->kind == XG_CALL_CLOSURE && call->static_target_func_id != XG_NO_ID)) {
                if (call->static_target_func_id == XG_NO_ID ||
                    !xg_global_evidence_find_body_index_by_func(
                        evidence, call->static_target_func_id, &target_index))
                    return false;
                if (!xg_body_effects_compose_rec(evidence, target_index, state, memo,
                                                 &target_effects))
                    return false;
                effect_bits |= target_effects;
                memo[body_index] = effect_bits;
            } else if (xg_method_callsite_is_direct_dispatch(evidence, call)) {
                if (!xg_body_effects_compose_method_target(evidence, call->method_id, state, memo,
                                                           &effect_bits))
                    return false;
                memo[body_index] = effect_bits;
            } else if (call->kind == XG_CALL_INTERFACE) {
                XgMethodId target_method_id = XG_NO_ID;
                if (xg_interface_callsite_direct_target_method(evidence, call, &target_method_id)) {
                    if (!xg_body_effects_compose_method_target(evidence, target_method_id, state,
                                                               memo, &effect_bits))
                        return false;
                } else if (!xg_interface_callsite_compose_target_set(evidence, call, state, memo,
                                                                     &effect_bits)) {
                    return false;
                }
                memo[body_index] = effect_bits;
            } else if (call->kind == XG_CALL_METHOD) {
                if (!xg_method_callsite_compose_target_set(evidence, call, state, memo,
                                                           &effect_bits))
                    return false;
                memo[body_index] = effect_bits;
            } else if (call->kind == XG_CALL_CLOSURE) {
                /* Function-value calls follow the IR coroutine resolver:
                 * unresolved ordinary calls are not suspension points. A
                 * suspendable closure contributes through a resolved static
                 * target or through the callee body's own local effects. */
                continue;
            } else {
                return false;
            }
        }
    }

    state[body_index] = 2;
    memo[body_index] = effect_bits;
    *out_effect_bits = effect_bits;
    return true;
}

XR_FUNC bool xg_body_effects_compose_closed_world_calls(const XgGlobalEvidence *evidence,
                                                        const XgBodySummary *body,
                                                        uint32_t *out_effect_bits) {
    uint8_t *state;
    uint32_t *memo;
    uint32_t body_index = 0;
    bool ok;

    if (!evidence || !body || !out_effect_bits ||
        !xg_global_evidence_find_body_index_by_func(evidence, body->func_id, &body_index))
        return false;
    if (evidence->nbodies == 0)
        return false;
    state = (uint8_t *) xr_calloc(evidence->nbodies, sizeof(uint8_t));
    memo = (uint32_t *) xr_calloc(evidence->nbodies, sizeof(uint32_t));
    if (!state || !memo) {
        xr_free(state);
        xr_free(memo);
        return false;
    }
    ok = xg_body_effects_compose_rec(evidence, body_index, state, memo, out_effect_bits);
    xr_free(state);
    xr_free(memo);
    return ok;
}

XR_FUNC uint64_t xg_global_evidence_hash(const XgGlobalEvidence *evidence) {
    uint64_t hash = XR_FNV64_OFFSET_BASIS;
    if (!evidence)
        return hash;
    hash = hash_build_key(hash, &evidence->key);
    hash = hash_mix(hash, &evidence->nmodules, sizeof(evidence->nmodules));
    hash = hash_mix(hash, &evidence->ndecls, sizeof(evidence->ndecls));
    hash = hash_mix(hash, &evidence->nclasses, sizeof(evidence->nclasses));
    hash = hash_mix(hash, &evidence->nclass_fields, sizeof(evidence->nclass_fields));
    hash = hash_mix(hash, &evidence->nmethods, sizeof(evidence->nmethods));
    hash = hash_mix(hash, &evidence->ninterface_impls, sizeof(evidence->ninterface_impls));
    hash = hash_mix(hash, &evidence->ninterface_extends, sizeof(evidence->ninterface_extends));
    hash = hash_mix(hash, &evidence->ninterface_methods, sizeof(evidence->ninterface_methods));
    hash =
        hash_mix(hash, &evidence->ninterface_object_uses, sizeof(evidence->ninterface_object_uses));
    hash = hash_mix(hash, &evidence->nbodies, sizeof(evidence->nbodies));
    hash = hash_mix(hash, &evidence->nparam_storages, sizeof(evidence->nparam_storages));
    hash = hash_mix(hash, &evidence->ncallsites, sizeof(evidence->ncallsites));
    hash = hash_mix(hash, &evidence->nlink_deps, sizeof(evidence->nlink_deps));
    hash = hash_mix(hash, &evidence->ngeneric_insts, sizeof(evidence->ngeneric_insts));
    hash = hash_mix(hash, &evidence->ngeneric_body_uses, sizeof(evidence->ngeneric_body_uses));
    hash = hash_mix(hash, &evidence->ngeneric_storages, sizeof(evidence->ngeneric_storages));
    hash = hash_mix(hash, &evidence->ngeneric_code_sizes, sizeof(evidence->ngeneric_code_sizes));
    hash = hash_mix(hash, &evidence->nsequence_accesses, sizeof(evidence->nsequence_accesses));
    hash = hash_mix(hash, &evidence->ncapacity_ops, sizeof(evidence->ncapacity_ops));
    hash = hash_mix(hash, &evidence->nbulk_ops, sizeof(evidence->nbulk_ops));
    hash = hash_mix(hash, &evidence->nencoding_ops, sizeof(evidence->nencoding_ops));
    hash = hash_mix(hash, &evidence->nderives, sizeof(evidence->nderives));
    hash = hash_mix(hash, &evidence->nderived_fields, sizeof(evidence->nderived_fields));
    hash = hash_mix(hash, &evidence->nderived_methods, sizeof(evidence->nderived_methods));
    hash = hash_mix(hash, &evidence->njson_shapes, sizeof(evidence->njson_shapes));
    hash = hash_mix(hash, &evidence->njson_fields, sizeof(evidence->njson_fields));
    hash = hash_mix(hash, &evidence->njson_accesses, sizeof(evidence->njson_accesses));
    hash = hash_mix(hash, &evidence->njson_codecs, sizeof(evidence->njson_codecs));
    hash = hash_mix(hash, &evidence->nrecord_shapes, sizeof(evidence->nrecord_shapes));
    hash = hash_mix(hash, &evidence->nrecord_fields, sizeof(evidence->nrecord_fields));
    hash = hash_mix(hash, &evidence->nrecord_accesses, sizeof(evidence->nrecord_accesses));
    hash = hash_mix(hash, &evidence->nrecord_merges, sizeof(evidence->nrecord_merges));
    hash = hash_mix(hash, &evidence->noptions_bags, sizeof(evidence->noptions_bags));
    hash = hash_mix(hash, &evidence->nmap_shapes, sizeof(evidence->nmap_shapes));
    hash = hash_mix(hash, &evidence->nmap_entries, sizeof(evidence->nmap_entries));
    hash = hash_mix(hash, &evidence->nkey_accesses, sizeof(evidence->nkey_accesses));
    hash = hash_mix(hash, &evidence->nhash_eqs, sizeof(evidence->nhash_eqs));
    for (uint32_t i = 0; i < evidence->nmodules; i++)
        hash = hash_module_summary(hash, &evidence->modules[i]);
    for (uint32_t i = 0; i < evidence->ndecls; i++)
        hash = hash_decl_summary(hash, &evidence->decls[i]);
    for (uint32_t i = 0; i < evidence->nclasses; i++)
        hash = hash_class_summary(hash, &evidence->classes[i]);
    for (uint32_t i = 0; i < evidence->nclass_fields; i++)
        hash = hash_class_field_summary(hash, &evidence->class_fields[i]);
    for (uint32_t i = 0; i < evidence->nmethods; i++)
        hash = hash_method_summary(hash, &evidence->methods[i]);
    for (uint32_t i = 0; i < evidence->ninterface_impls; i++)
        hash = hash_interface_impl_summary(hash, &evidence->interface_impls[i]);
    for (uint32_t i = 0; i < evidence->ninterface_extends; i++)
        hash = hash_interface_extends_summary(hash, &evidence->interface_extends[i]);
    for (uint32_t i = 0; i < evidence->ninterface_methods; i++)
        hash = hash_interface_method_summary(hash, &evidence->interface_methods[i]);
    for (uint32_t i = 0; i < evidence->ninterface_object_uses; i++)
        hash = hash_interface_object_use_summary(hash, &evidence->interface_object_uses[i]);
    for (uint32_t i = 0; i < evidence->nbodies; i++)
        hash = hash_body_summary(hash, &evidence->bodies[i]);
    for (uint32_t i = 0; i < evidence->nparam_storages; i++)
        hash = hash_param_storage_summary(hash, &evidence->param_storages[i]);
    for (uint32_t i = 0; i < evidence->ncallsites; i++)
        hash = hash_callsite_summary(hash, &evidence->callsites[i]);
    for (uint32_t i = 0; i < evidence->nlink_deps; i++)
        hash = hash_link_dependency_summary(hash, &evidence->link_deps[i]);
    for (uint32_t i = 0; i < evidence->ngeneric_insts; i++)
        hash = hash_generic_inst_summary(hash, &evidence->generic_insts[i]);
    for (uint32_t i = 0; i < evidence->ngeneric_body_uses; i++)
        hash = hash_generic_body_use_summary(hash, &evidence->generic_body_uses[i]);
    for (uint32_t i = 0; i < evidence->ngeneric_storages; i++)
        hash = hash_generic_storage_summary(hash, &evidence->generic_storages[i]);
    for (uint32_t i = 0; i < evidence->ngeneric_code_sizes; i++)
        hash = hash_generic_code_size_summary(hash, &evidence->generic_code_sizes[i]);
    for (uint32_t i = 0; i < evidence->nsequence_accesses; i++)
        hash = hash_sequence_access_summary(hash, &evidence->sequence_accesses[i]);
    for (uint32_t i = 0; i < evidence->ncapacity_ops; i++)
        hash = hash_capacity_op_summary(hash, &evidence->capacity_ops[i]);
    for (uint32_t i = 0; i < evidence->nbulk_ops; i++)
        hash = hash_bulk_op_summary(hash, &evidence->bulk_ops[i]);
    for (uint32_t i = 0; i < evidence->nencoding_ops; i++)
        hash = hash_encoding_op_summary(hash, &evidence->encoding_ops[i]);
    for (uint32_t i = 0; i < evidence->nderives; i++)
        hash = hash_derive_summary(hash, &evidence->derives[i]);
    for (uint32_t i = 0; i < evidence->nderived_fields; i++)
        hash = hash_derived_field_summary(hash, &evidence->derived_fields[i]);
    for (uint32_t i = 0; i < evidence->nderived_methods; i++)
        hash = hash_derived_method_summary(hash, &evidence->derived_methods[i]);
    for (uint32_t i = 0; i < evidence->njson_shapes; i++)
        hash = hash_json_shape_summary(hash, &evidence->json_shapes[i]);
    for (uint32_t i = 0; i < evidence->njson_fields; i++)
        hash = hash_json_field_summary(hash, &evidence->json_fields[i]);
    for (uint32_t i = 0; i < evidence->njson_accesses; i++)
        hash = hash_json_access_summary(hash, &evidence->json_accesses[i]);
    for (uint32_t i = 0; i < evidence->njson_codecs; i++)
        hash = hash_json_codec_summary(hash, &evidence->json_codecs[i]);
    for (uint32_t i = 0; i < evidence->nrecord_shapes; i++)
        hash = hash_record_shape_summary(hash, &evidence->record_shapes[i]);
    for (uint32_t i = 0; i < evidence->nrecord_fields; i++)
        hash = hash_record_field_summary(hash, &evidence->record_fields[i]);
    for (uint32_t i = 0; i < evidence->nrecord_accesses; i++)
        hash = hash_record_access_summary(hash, &evidence->record_accesses[i]);
    for (uint32_t i = 0; i < evidence->nrecord_merges; i++)
        hash = hash_record_merge_summary(hash, &evidence->record_merges[i]);
    for (uint32_t i = 0; i < evidence->noptions_bags; i++)
        hash = hash_options_bag_summary(hash, &evidence->options_bags[i]);
    for (uint32_t i = 0; i < evidence->nmap_shapes; i++)
        hash = hash_map_shape_summary(hash, &evidence->map_shapes[i]);
    for (uint32_t i = 0; i < evidence->nmap_entries; i++)
        hash = hash_map_entry_summary(hash, &evidence->map_entries[i]);
    for (uint32_t i = 0; i < evidence->nkey_accesses; i++)
        hash = hash_key_access_summary(hash, &evidence->key_accesses[i]);
    for (uint32_t i = 0; i < evidence->nhash_eqs; i++)
        hash = hash_hash_eq_summary(hash, &evidence->hash_eqs[i]);
    return hash == 0 ? 1 : hash;
}

static uint64_t hash_evidence_cache_key_common(uint64_t hash, const XgGlobalEvidence *evidence,
                                               uint32_t phase) {
    hash = hash_u32(hash, XG_GLOBAL_EVIDENCE_SCHEMA_VERSION);
    hash = hash_u32(hash, phase);
    if (!evidence)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, evidence->key.module_id);
    hash = hash_u32(hash, evidence->key.profile);
    hash = hash_u64(hash, evidence->key.compiler_semver_hash);
    hash = hash_u64(hash, evidence->key.profile_hash);
    return hash_u64(hash, evidence->key.imported_summary_hash);
}

static uint64_t xg_global_evidence_phase_content_hash(const XgGlobalEvidence *evidence,
                                                      uint32_t phase) {
    uint64_t hash = XR_FNV64_OFFSET_BASIS;
    if (!evidence)
        return hash;
    hash = hash_evidence_cache_key_common(hash, evidence, phase);
    switch ((XgEvidenceCachePhase) phase) {
        case XG_EVIDENCE_CACHE_DECLARATIONS:
            hash = hash_u32(hash, evidence->nmodules);
            hash = hash_u32(hash, evidence->ndecls);
            for (uint32_t i = 0; i < evidence->nmodules; i++)
                hash = hash_module_summary(hash, &evidence->modules[i]);
            for (uint32_t i = 0; i < evidence->ndecls; i++)
                hash = hash_decl_summary(hash, &evidence->decls[i]);
            break;
        case XG_EVIDENCE_CACHE_SEMANTIC_GRAPH:
            hash = hash_u32(hash, evidence->nmodules);
            hash = hash_u32(hash, evidence->ndecls);
            hash = hash_u32(hash, evidence->nclasses);
            hash = hash_u32(hash, evidence->nclass_fields);
            hash = hash_u32(hash, evidence->nmethods);
            hash = hash_u32(hash, evidence->ninterface_impls);
            hash = hash_u32(hash, evidence->ninterface_extends);
            hash = hash_u32(hash, evidence->ninterface_methods);
            hash = hash_u32(hash, evidence->nderives);
            hash = hash_u32(hash, evidence->nderived_fields);
            hash = hash_u32(hash, evidence->nderived_methods);
            for (uint32_t i = 0; i < evidence->nmodules; i++)
                hash = hash_module_summary(hash, &evidence->modules[i]);
            for (uint32_t i = 0; i < evidence->ndecls; i++)
                hash = hash_decl_summary(hash, &evidence->decls[i]);
            for (uint32_t i = 0; i < evidence->nclasses; i++)
                hash = hash_class_summary(hash, &evidence->classes[i]);
            for (uint32_t i = 0; i < evidence->nclass_fields; i++)
                hash = hash_class_field_summary(hash, &evidence->class_fields[i]);
            for (uint32_t i = 0; i < evidence->nmethods; i++)
                hash = hash_method_summary(hash, &evidence->methods[i]);
            for (uint32_t i = 0; i < evidence->ninterface_impls; i++)
                hash = hash_interface_impl_summary(hash, &evidence->interface_impls[i]);
            for (uint32_t i = 0; i < evidence->ninterface_extends; i++)
                hash = hash_interface_extends_summary(hash, &evidence->interface_extends[i]);
            for (uint32_t i = 0; i < evidence->ninterface_methods; i++)
                hash = hash_interface_method_summary(hash, &evidence->interface_methods[i]);
            for (uint32_t i = 0; i < evidence->nderives; i++)
                hash = hash_derive_summary(hash, &evidence->derives[i]);
            for (uint32_t i = 0; i < evidence->nderived_fields; i++)
                hash = hash_derived_field_summary(hash, &evidence->derived_fields[i]);
            for (uint32_t i = 0; i < evidence->nderived_methods; i++)
                hash = hash_derived_method_summary(hash, &evidence->derived_methods[i]);
            break;
        case XG_EVIDENCE_CACHE_BODY_SUMMARY:
            hash = hash_u32(hash, evidence->nbodies);
            hash = hash_u32(hash, evidence->nparam_storages);
            hash = hash_u32(hash, evidence->ncallsites);
            hash = hash_u32(hash, evidence->ninterface_object_uses);
            hash = hash_u32(hash, evidence->nlink_deps);
            hash = hash_u32(hash, evidence->ngeneric_insts);
            for (uint32_t i = 0; i < evidence->nbodies; i++)
                hash = hash_body_summary(hash, &evidence->bodies[i]);
            for (uint32_t i = 0; i < evidence->nparam_storages; i++)
                hash = hash_param_storage_summary(hash, &evidence->param_storages[i]);
            for (uint32_t i = 0; i < evidence->ncallsites; i++)
                hash = hash_callsite_summary(hash, &evidence->callsites[i]);
            for (uint32_t i = 0; i < evidence->ninterface_object_uses; i++)
                hash = hash_interface_object_use_summary(hash, &evidence->interface_object_uses[i]);
            for (uint32_t i = 0; i < evidence->nlink_deps; i++)
                hash = hash_link_dependency_summary(hash, &evidence->link_deps[i]);
            for (uint32_t i = 0; i < evidence->ngeneric_insts; i++)
                hash = hash_generic_inst_summary(hash, &evidence->generic_insts[i]);
            break;
        case XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE:
            hash = xg_global_evidence_hash(evidence);
            break;
        default:
            hash = hash_u32(hash, 0);
            break;
    }
    return hash == 0 ? 1 : hash;
}

XR_FUNC XgEvidenceCacheKey xg_global_evidence_cache_key(const XgGlobalEvidence *evidence,
                                                        uint32_t phase) {
    XgEvidenceCacheKey key;
    memset(&key, 0, sizeof(key));
    key.schema_version = XG_GLOBAL_EVIDENCE_SCHEMA_VERSION;
    key.phase = phase;
    if (!evidence)
        return key;
    key.module_id = evidence->key.module_id;
    key.profile = evidence->key.profile;
    key.compiler_semver_hash = evidence->key.compiler_semver_hash;
    key.profile_hash = evidence->key.profile_hash;
    key.imported_summary_hash = evidence->key.imported_summary_hash;
    key.content_hash = xg_global_evidence_phase_content_hash(evidence, phase);
    return key;
}

XR_FUNC XgEvidenceCacheRequestKey
xg_evidence_cache_request_key_from_build_key(const XgBuildKey *build_key, uint32_t phase) {
    XgEvidenceCacheRequestKey key;
    memset(&key, 0, sizeof(key));
    key.schema_version = XG_GLOBAL_EVIDENCE_SCHEMA_VERSION;
    key.phase = phase;
    if (!build_key)
        return key;
    key.module_id = build_key->module_id;
    key.profile = build_key->profile;
    key.source_hash = build_key->source_hash;
    key.compiler_semver_hash = build_key->compiler_semver_hash;
    key.profile_hash = build_key->profile_hash;
    key.imported_summary_hash = build_key->imported_summary_hash;
    return key;
}

XR_FUNC XgEvidenceCacheRequestKey
xg_global_evidence_cache_request_key(const XgGlobalEvidence *evidence, uint32_t phase) {
    return xg_evidence_cache_request_key_from_build_key(evidence ? &evidence->key : NULL, phase);
}

XR_FUNC uint64_t xg_evidence_cache_request_key_hash(const XgEvidenceCacheRequestKey *key) {
    uint64_t hash = XR_FNV64_OFFSET_BASIS;
    if (!key)
        return hash;
    hash = hash_u32(hash, key->schema_version);
    hash = hash_u32(hash, key->phase);
    hash = hash_u32(hash, key->module_id);
    hash = hash_u32(hash, key->profile);
    hash = hash_u64(hash, key->source_hash);
    hash = hash_u64(hash, key->compiler_semver_hash);
    hash = hash_u64(hash, key->profile_hash);
    hash = hash_u64(hash, key->imported_summary_hash);
    return hash == 0 ? 1 : hash;
}

XR_FUNC bool xg_evidence_cache_request_key_matches(const XgEvidenceCacheRequestKey *cached,
                                                   const XgEvidenceCacheRequestKey *expected) {
    return cached && expected && cached->schema_version == expected->schema_version &&
           cached->phase == expected->phase && cached->module_id == expected->module_id &&
           cached->profile == expected->profile && cached->source_hash == expected->source_hash &&
           cached->compiler_semver_hash == expected->compiler_semver_hash &&
           cached->profile_hash == expected->profile_hash &&
           cached->imported_summary_hash == expected->imported_summary_hash;
}

XR_FUNC bool xg_evidence_cache_request_key_format(const XgEvidenceCacheRequestKey *key, char *buf,
                                                  size_t buf_len) {
    int written;
    if (!key || !buf || buf_len == 0)
        return false;
    written = snprintf(buf, buf_len,
                       "xg-cache-request v1 schema=%u phase=%u module=%u profile=%u "
                       "source=%016" PRIx64 " compiler=%016" PRIx64 " profile_hash=%016" PRIx64
                       " imports=%016" PRIx64 " request=%016" PRIx64,
                       key->schema_version, key->phase, key->module_id, key->profile,
                       key->source_hash, key->compiler_semver_hash, key->profile_hash,
                       key->imported_summary_hash, xg_evidence_cache_request_key_hash(key));
    return written > 0 && (size_t) written < buf_len;
}

XR_FUNC bool xg_evidence_cache_request_key_parse(const char *text,
                                                 XgEvidenceCacheRequestKey *out_key) {
    XgEvidenceCacheRequestKey key;
    uint64_t recorded_hash = 0;
    char trailing = '\0';
    int matched;
    if (!text || !out_key)
        return false;
    memset(&key, 0, sizeof(key));
    matched = sscanf(text,
                     "xg-cache-request v1 schema=%" SCNu32 " phase=%" SCNu32 " module=%" SCNu32
                     " profile=%" SCNu32 " source=%" SCNx64 " compiler=%" SCNx64
                     " profile_hash=%" SCNx64 " imports=%" SCNx64 " request=%" SCNx64 " %c",
                     &key.schema_version, &key.phase, &key.module_id, &key.profile,
                     &key.source_hash, &key.compiler_semver_hash, &key.profile_hash,
                     &key.imported_summary_hash, &recorded_hash, &trailing);
    if (matched != 9)
        return false;
    if (recorded_hash != xg_evidence_cache_request_key_hash(&key))
        return false;
    *out_key = key;
    return true;
}

XR_FUNC uint64_t xg_evidence_cache_key_hash(const XgEvidenceCacheKey *key) {
    uint64_t hash = XR_FNV64_OFFSET_BASIS;
    if (!key)
        return hash;
    hash = hash_u32(hash, key->schema_version);
    hash = hash_u32(hash, key->phase);
    hash = hash_u32(hash, key->module_id);
    hash = hash_u32(hash, key->profile);
    hash = hash_u64(hash, key->compiler_semver_hash);
    hash = hash_u64(hash, key->profile_hash);
    hash = hash_u64(hash, key->imported_summary_hash);
    hash = hash_u64(hash, key->content_hash);
    return hash == 0 ? 1 : hash;
}

XR_FUNC bool xg_evidence_cache_key_matches(const XgEvidenceCacheKey *cached,
                                           const XgEvidenceCacheKey *expected) {
    return cached && expected && cached->schema_version == expected->schema_version &&
           cached->phase == expected->phase && cached->module_id == expected->module_id &&
           cached->profile == expected->profile &&
           cached->compiler_semver_hash == expected->compiler_semver_hash &&
           cached->profile_hash == expected->profile_hash &&
           cached->imported_summary_hash == expected->imported_summary_hash &&
           cached->content_hash == expected->content_hash;
}

XR_FUNC bool xg_evidence_cache_key_format(const XgEvidenceCacheKey *key, char *buf,
                                          size_t buf_len) {
    int written;
    if (!key || !buf || buf_len == 0)
        return false;
    written = snprintf(buf, buf_len,
                       "xg-cache-key v1 schema=%u phase=%u module=%u profile=%u "
                       "compiler=%016" PRIx64 " profile_hash=%016" PRIx64 " imports=%016" PRIx64
                       " content=%016" PRIx64 " key=%016" PRIx64,
                       key->schema_version, key->phase, key->module_id, key->profile,
                       key->compiler_semver_hash, key->profile_hash, key->imported_summary_hash,
                       key->content_hash, xg_evidence_cache_key_hash(key));
    return written > 0 && (size_t) written < buf_len;
}

XR_FUNC bool xg_evidence_cache_key_parse(const char *text, XgEvidenceCacheKey *out_key) {
    XgEvidenceCacheKey key;
    uint64_t recorded_hash = 0;
    char trailing = '\0';
    int matched;
    if (!text || !out_key)
        return false;
    memset(&key, 0, sizeof(key));
    matched = sscanf(text,
                     "xg-cache-key v1 schema=%" SCNu32 " phase=%" SCNu32 " module=%" SCNu32
                     " profile=%" SCNu32 " compiler=%" SCNx64 " profile_hash=%" SCNx64
                     " imports=%" SCNx64 " content=%" SCNx64 " key=%" SCNx64 " %c",
                     &key.schema_version, &key.phase, &key.module_id, &key.profile,
                     &key.compiler_semver_hash, &key.profile_hash, &key.imported_summary_hash,
                     &key.content_hash, &recorded_hash, &trailing);
    if (matched != 9)
        return false;
    if (recorded_hash != xg_evidence_cache_key_hash(&key))
        return false;
    *out_key = key;
    return true;
}

static bool evidence_cache_phase_index(uint32_t phase, uint32_t *out_index) {
    switch ((XgEvidenceCachePhase) phase) {
        case XG_EVIDENCE_CACHE_DECLARATIONS:
        case XG_EVIDENCE_CACHE_SEMANTIC_GRAPH:
        case XG_EVIDENCE_CACHE_BODY_SUMMARY:
        case XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE:
            if (out_index)
                *out_index = phase - 1;
            return true;
        default:
            return false;
    }
}

static uint32_t evidence_cache_phase_bit(uint32_t phase) {
    uint32_t index = 0;
    if (!evidence_cache_phase_index(phase, &index))
        return 0;
    return 1u << index;
}

static uint32_t evidence_cache_all_phase_mask(void) {
    return (1u << XG_EVIDENCE_CACHE_PHASE_COUNT) - 1u;
}

XR_FUNC XgEvidenceCacheManifest
xg_global_evidence_cache_manifest(const XgGlobalEvidence *evidence) {
    static const uint32_t phases[] = {
        XG_EVIDENCE_CACHE_DECLARATIONS,
        XG_EVIDENCE_CACHE_SEMANTIC_GRAPH,
        XG_EVIDENCE_CACHE_BODY_SUMMARY,
        XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE,
    };
    XgEvidenceCacheManifest manifest;
    memset(&manifest, 0, sizeof(manifest));
    if (!evidence)
        return manifest;
    for (uint32_t i = 0; i < XG_EVIDENCE_CACHE_PHASE_COUNT; i++) {
        manifest.keys[i] = xg_global_evidence_cache_key(evidence, phases[i]);
        manifest.phase_mask |= 1u << i;
    }
    return manifest;
}

XR_FUNC const XgEvidenceCacheKey *
xg_evidence_cache_manifest_find(const XgEvidenceCacheManifest *manifest, uint32_t phase) {
    uint32_t index = 0;
    uint32_t bit = evidence_cache_phase_bit(phase);
    if (!manifest || bit == 0 || (manifest->phase_mask & bit) == 0 ||
        !evidence_cache_phase_index(phase, &index))
        return NULL;
    if (manifest->keys[index].phase != phase)
        return NULL;
    return &manifest->keys[index];
}

XR_FUNC bool xg_evidence_cache_manifest_phase_matches(const XgEvidenceCacheManifest *manifest,
                                                      const XgEvidenceCacheKey *expected) {
    const XgEvidenceCacheKey *cached;
    if (!expected)
        return false;
    cached = xg_evidence_cache_manifest_find(manifest, expected->phase);
    return xg_evidence_cache_key_matches(cached, expected);
}

XR_FUNC bool xg_evidence_cache_manifest_format(const XgEvidenceCacheManifest *manifest, char *buf,
                                               size_t buf_len) {
    static const uint32_t phases[] = {
        XG_EVIDENCE_CACHE_DECLARATIONS,
        XG_EVIDENCE_CACHE_SEMANTIC_GRAPH,
        XG_EVIDENCE_CACHE_BODY_SUMMARY,
        XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE,
    };
    size_t used = 0;
    int written;
    char line[256];
    if (!manifest || !buf || buf_len == 0)
        return false;
    if ((manifest->phase_mask & evidence_cache_all_phase_mask()) != evidence_cache_all_phase_mask())
        return false;
    written = snprintf(buf, buf_len, "xg-cache-manifest v1 phases=0x%x\n",
                       manifest->phase_mask & evidence_cache_all_phase_mask());
    if (written <= 0 || (size_t) written >= buf_len)
        return false;
    used = (size_t) written;
    for (uint32_t i = 0; i < XG_EVIDENCE_CACHE_PHASE_COUNT; i++) {
        const XgEvidenceCacheKey *key = xg_evidence_cache_manifest_find(manifest, phases[i]);
        if (!key || !xg_evidence_cache_key_format(key, line, sizeof(line)))
            return false;
        written = snprintf(buf + used, buf_len - used, "%s\n", line);
        if (written <= 0 || (size_t) written >= buf_len - used)
            return false;
        used += (size_t) written;
    }
    return true;
}

static bool evidence_cache_next_line(const char **cursor, char *line, size_t line_len) {
    const char *start;
    const char *end;
    size_t len;
    if (!cursor || !*cursor || !line || line_len == 0 || **cursor == '\0')
        return false;
    start = *cursor;
    end = strchr(start, '\n');
    len = end ? (size_t) (end - start) : strlen(start);
    if (len >= line_len)
        return false;
    memcpy(line, start, len);
    line[len] = '\0';
    *cursor = end ? end + 1 : start + len;
    return true;
}

XR_FUNC bool xg_evidence_cache_manifest_parse(const char *text,
                                              XgEvidenceCacheManifest *out_manifest) {
    XgEvidenceCacheManifest manifest;
    const char *cursor = text;
    char line[320];
    uint32_t declared_mask = 0;
    char trailing = '\0';
    if (!text || !out_manifest)
        return false;
    memset(&manifest, 0, sizeof(manifest));
    if (!evidence_cache_next_line(&cursor, line, sizeof(line)))
        return false;
    if (sscanf(line, "xg-cache-manifest v1 phases=%" SCNx32 "%c", &declared_mask, &trailing) != 1)
        return false;
    if ((declared_mask & evidence_cache_all_phase_mask()) != evidence_cache_all_phase_mask())
        return false;
    for (uint32_t i = 0; i < XG_EVIDENCE_CACHE_PHASE_COUNT; i++) {
        XgEvidenceCacheKey key;
        uint32_t index = 0;
        uint32_t bit;
        if (!evidence_cache_next_line(&cursor, line, sizeof(line)))
            return false;
        if (!xg_evidence_cache_key_parse(line, &key) ||
            !evidence_cache_phase_index(key.phase, &index))
            return false;
        bit = 1u << index;
        if ((manifest.phase_mask & bit) != 0)
            return false;
        manifest.keys[index] = key;
        manifest.phase_mask |= bit;
    }
    if (manifest.phase_mask != (declared_mask & evidence_cache_all_phase_mask()))
        return false;
    while (*cursor) {
        if (*cursor != '\n' && *cursor != '\r' && *cursor != ' ' && *cursor != '\t')
            return false;
        cursor++;
    }
    *out_manifest = manifest;
    return true;
}

static uint64_t cache_payload_hash_bytes(const char *text, size_t len) {
    uint64_t hash = xr_hash_bytes64(text ? text : "", len);
    return hash == 0 ? 1 : hash;
}

static void dump_cache_payload_module_rows(FILE *out, const XgGlobalEvidence *evidence) {
    if (!evidence)
        return;
    for (uint32_t i = 0; i < evidence->nmodules; i++) {
        const XgModuleSummary *m = &evidence->modules[i];
        fprintf(out,
                "module id=%u name=%u canonical=%016" PRIx64 " source=%016" PRIx64
                " kind=%u flags=0x%x\n",
                m->module_id, m->name_id, m->canonical_hash, m->source_hash, (unsigned) m->kind,
                m->flags);
    }
}

static void dump_cache_payload_declarations(FILE *out, const XgGlobalEvidence *evidence) {
    fprintf(out, "payload-count modules=%u decls=%u\n", evidence ? evidence->nmodules : 0,
            evidence ? evidence->ndecls : 0);
    if (!evidence)
        return;
    dump_cache_payload_module_rows(out, evidence);
    for (uint32_t i = 0; i < evidence->ndecls; i++) {
        const XgDeclSummary *d = &evidence->decls[i];
        fprintf(out,
                "decl id=%u module=%u node=%u kind=%u flags=0x%x name=%u type=%u sig=%u span=%u "
                "derive=0x%x storage_flags=0x%x owner=%u mutability=%u address=%u materialize=%u\n",
                d->decl_id, d->module_id, d->source_node_id, (unsigned) d->kind, d->flags,
                d->name_id, d->type_key, d->signature_key, d->source_span_id, d->derive_flags,
                d->storage_flags, (unsigned) d->storage_owner, (unsigned) d->storage_mutability,
                (unsigned) d->address_identity, (unsigned) d->materialization_kind);
    }
}

static void dump_cache_payload_semantic(FILE *out, const XgGlobalEvidence *evidence) {
    fprintf(out,
            "payload-count modules=%u decls=%u classes=%u class_fields=%u methods=%u impls=%u "
            "extends=%u interface_methods=%u derives=%u derived_fields=%u derived_methods=%u\n",
            evidence ? evidence->nmodules : 0, evidence ? evidence->ndecls : 0,
            evidence ? evidence->nclasses : 0, evidence ? evidence->nclass_fields : 0,
            evidence ? evidence->nmethods : 0, evidence ? evidence->ninterface_impls : 0,
            evidence ? evidence->ninterface_extends : 0,
            evidence ? evidence->ninterface_methods : 0, evidence ? evidence->nderives : 0,
            evidence ? evidence->nderived_fields : 0, evidence ? evidence->nderived_methods : 0);
    if (!evidence)
        return;
    dump_cache_payload_declarations(out, evidence);
    for (uint32_t i = 0; i < evidence->nclasses; i++) {
        const XgClassSummary *c = &evidence->classes[i];
        fprintf(out,
                "class id=%u module=%u decl=%u name=%u parent=%u flags=0x%x fields=%u+%u "
                "methods=%u+%u impls=%u+%u origin=%u origin_name=%u type=%u args=%u+%u "
                "decl_kind=%u\n",
                c->class_id, c->module_id, c->decl_id, c->name_id, c->parent_class_id, c->flags,
                c->field_start, c->field_count, c->method_start, c->method_count,
                c->interface_start, c->interface_count, c->generic_origin_class_id,
                c->generic_origin_name_id, c->generic_type_key, c->generic_type_arg_key_start,
                (unsigned) c->generic_type_arg_count, (unsigned) c->decl_kind);
    }
    for (uint32_t i = 0; i < evidence->nclass_fields; i++) {
        const XgClassFieldSummary *f = &evidence->class_fields[i];
        fprintf(out,
                "class-field id=%u module=%u node=%u owner=%u name=%u type=%u target_name=%u "
                "target_class=%u target_interface=%u element=%u key=%u value=%u fixed=%u "
                "ordinal=%u slot=%u semantic=%u width=%u flags=0x%x\n",
                f->field_id, f->module_id, f->source_node_id, f->owner_class_id, f->name_id,
                f->type_key, f->target_name_id, f->target_class_id, f->target_interface_id,
                f->element_type_key, f->key_type_key, f->value_type_key, f->fixed_length,
                f->decl_ordinal, f->instance_slot, (unsigned) f->semantic_kind,
                (unsigned) f->native_width, f->flags);
    }
    for (uint32_t i = 0; i < evidence->nmethods; i++) {
        const XgMethodSummary *m = &evidence->methods[i];
        fprintf(out,
                "method id=%u owner=%u node=%u name=%u sig=%u override=%u root=%u depth=%u "
                "default=%u flags=0x%x\n",
                m->method_id, m->owner_class_id, m->source_node_id, m->name_id, m->signature_key,
                m->override_of, m->root_method_id, m->override_depth, m->default_arg_contract_id,
                m->flags);
    }
    for (uint32_t i = 0; i < evidence->ninterface_impls; i++) {
        const XgInterfaceImplSummary *impl = &evidence->interface_impls[i];
        fprintf(out, "interface-impl class=%u interface=%u name=%u type=%u span=%u flags=0x%x\n",
                impl->implementor_class_id, impl->interface_id, impl->name_id, impl->type_key,
                impl->source_span_id, impl->flags);
    }
    for (uint32_t i = 0; i < evidence->ninterface_extends; i++) {
        const XgInterfaceExtendsSummary *edge = &evidence->interface_extends[i];
        fprintf(out, "interface-extends child=%u parent=%u name=%u type=%u span=%u flags=0x%x\n",
                edge->child_interface_id, edge->parent_interface_id, edge->name_id, edge->type_key,
                edge->source_span_id, edge->flags);
    }
    for (uint32_t i = 0; i < evidence->ninterface_methods; i++) {
        const XgInterfaceMethodSummary *m = &evidence->interface_methods[i];
        fprintf(out,
                "interface-method id=%u owner=%u name=%u sig=%u ordinal=%u span=%u flags=0x%x\n",
                m->interface_method_id, m->owner_interface_id, m->name_id, m->signature_key,
                m->ordinal, m->source_span_id, m->flags);
    }
    for (uint32_t i = 0; i < evidence->nderives; i++) {
        const XgDeriveSummary *d = &evidence->derives[i];
        fprintf(out,
                "derive id=%u module=%u decl=%u span=%u type=%u kind=%u fields=%u+%u "
                "methods=%u+%u flags=0x%x hash=%016" PRIx64 "\n",
                d->derive_id, d->module_id, d->owner_decl_id, d->source_span_id, d->type_key,
                (unsigned) d->derive_kind, d->field_start, (unsigned) d->field_count,
                d->method_start, (unsigned) d->method_count, d->flags, d->derive_hash);
    }
    for (uint32_t i = 0; i < evidence->nderived_fields; i++) {
        const XgDerivedFieldSummary *f = &evidence->derived_fields[i];
        fprintf(out,
                "derived-field id=%u derive=%u ordinal=%u name=%u type=%u source=%u flags=0x%x\n",
                f->field_id, f->derive_id, (unsigned) f->field_ordinal, f->name_id, f->type_key,
                f->source_field_id, f->flags);
    }
    for (uint32_t i = 0; i < evidence->nderived_methods; i++) {
        const XgDerivedMethodSummary *m = &evidence->derived_methods[i];
        fprintf(out, "derived-method id=%u derive=%u kind=%u body=%u sig=%u flags=0x%x\n",
                m->method_id, m->derive_id, (unsigned) m->method_kind, m->generated_body_func_id,
                m->signature_key, m->flags);
    }
}

static void dump_cache_payload_body(FILE *out, const XgGlobalEvidence *evidence) {
    fprintf(out,
            "payload-count bodies=%u param_storages=%u callsites=%u interface_object_uses=%u "
            "link_deps=%u generic_insts=%u\n",
            evidence ? evidence->nbodies : 0, evidence ? evidence->nparam_storages : 0,
            evidence ? evidence->ncallsites : 0, evidence ? evidence->ninterface_object_uses : 0,
            evidence ? evidence->nlink_deps : 0, evidence ? evidence->ngeneric_insts : 0);
    if (!evidence)
        return;
    for (uint32_t i = 0; i < evidence->nbodies; i++) {
        const XgBodySummary *b = &evidence->bodies[i];
        fprintf(out,
                "body id=%u module=%u node=%u decl=%u class=%u method=%u name=%u sig=%u span=%u "
                "kind=%u hash=%016" PRIx64 " effect=0x%x escape=0x%x caps=0x%x "
                "param_storage=%u params=%u+%u calls=%u+%u metadata=0x%x static=0x%x\n",
                b->func_id, b->module_id, b->source_node_id, b->owner_decl_id, b->owner_class_id,
                b->owner_method_id, b->name_id, b->signature_key, b->source_span_id,
                (unsigned) b->kind, b->body_hash, b->effect_bits, b->escape_bits,
                b->capability_bits, b->param_storage_key, b->param_storage_start,
                b->param_storage_count, b->callsite_start, b->callsite_count, b->metadata_use_bits,
                b->static_data_use_bits);
    }
    for (uint32_t i = 0; i < evidence->nparam_storages; i++) {
        const XgParamStorageSummary *p = &evidence->param_storages[i];
        fprintf(out, "param-storage id=%u owner=%u index=%u storage=%u flags=0x%x\n",
                p->requirement_id, p->owner_func_id, p->param_index, (unsigned) p->storage_owner,
                p->flags);
    }
    for (uint32_t i = 0; i < evidence->ncallsites; i++) {
        const XgCallsiteSummary *c = &evidence->callsites[i];
        fprintf(
            out,
            "callsite id=%u owner=%u node=%u span=%u ordinal=%u kind=%u target=%u recv_class=%u "
            "recv_interface=%u method=%u name=%u sig=%u args=%u+%u flags=0x%x\n",
            c->callsite_id, c->owner_func_id, c->source_node_id, c->source_span_id, c->body_ordinal,
            (unsigned) c->kind, c->static_target_func_id, c->receiver_static_class_id,
            c->receiver_static_interface_id, c->method_id, c->method_name_id,
            c->method_signature_key, c->arg_type_key_start, (unsigned) c->arg_count, c->flags);
    }
    for (uint32_t i = 0; i < evidence->ninterface_object_uses; i++) {
        const XgInterfaceObjectUseSummary *u = &evidence->interface_object_uses[i];
        fprintf(out,
                "interface-object-use id=%u interface=%u owner=%u span=%u ordinal=%u type=%u "
                "reason=0x%x flags=0x%x\n",
                u->use_id, u->interface_id, u->owner_func_id, u->source_span_id, u->body_ordinal,
                u->type_key, u->reason, u->flags);
    }
    for (uint32_t i = 0; i < evidence->nlink_deps; i++) {
        const XgLinkDependencySummary *l = &evidence->link_deps[i];
        fprintf(out,
                "link-dep id=%u module=%u decl=%u span=%u name_id=%u kind=%u flags=0x%x name=%s\n",
                l->link_id, l->module_id, l->decl_id, l->source_span_id, l->name_id,
                (unsigned) l->kind, l->flags, l->name);
    }
    for (uint32_t i = 0; i < evidence->ngeneric_insts; i++) {
        const XgGenericInstSummary *g = &evidence->generic_insts[i];
        fprintf(out,
                "generic-inst id=%u module=%u origin_decl=%u origin_func=%u origin_method=%u "
                "origin_class=%u spec_func=%u spec_class=%u root=%u constraint=%u name=%u "
                "type=%u args=%u+%u span=%u kind=%u flags=0x%x\n",
                g->generic_inst_id, g->module_id, g->origin_decl_id, g->origin_func_id,
                g->origin_method_id, g->origin_class_id, g->specialized_func_id,
                g->specialized_class_id, g->root_callsite_id, g->constraint_interface_id,
                g->name_id, g->type_key, g->type_arg_key_start, (unsigned) g->type_arg_count,
                g->source_span_id, (unsigned) g->kind, g->flags);
    }
}

static void dump_cache_payload_global_extra(FILE *out, const XgGlobalEvidence *evidence) {
    fprintf(out,
            "payload-extra v1 generic_body_uses=%u generic_storages=%u generic_code_sizes=%u "
            "seq=%u capacity=%u bulk=%u encoding=%u json_shapes=%u json_fields=%u "
            "json_accesses=%u json_codecs=%u record_shapes=%u record_fields=%u "
            "record_accesses=%u record_merges=%u options=%u map_shapes=%u map_entries=%u "
            "key_accesses=%u hash_eqs=%u\n",
            evidence ? evidence->ngeneric_body_uses : 0, evidence ? evidence->ngeneric_storages : 0,
            evidence ? evidence->ngeneric_code_sizes : 0,
            evidence ? evidence->nsequence_accesses : 0, evidence ? evidence->ncapacity_ops : 0,
            evidence ? evidence->nbulk_ops : 0, evidence ? evidence->nencoding_ops : 0,
            evidence ? evidence->njson_shapes : 0, evidence ? evidence->njson_fields : 0,
            evidence ? evidence->njson_accesses : 0, evidence ? evidence->njson_codecs : 0,
            evidence ? evidence->nrecord_shapes : 0, evidence ? evidence->nrecord_fields : 0,
            evidence ? evidence->nrecord_accesses : 0, evidence ? evidence->nrecord_merges : 0,
            evidence ? evidence->noptions_bags : 0, evidence ? evidence->nmap_shapes : 0,
            evidence ? evidence->nmap_entries : 0, evidence ? evidence->nkey_accesses : 0,
            evidence ? evidence->nhash_eqs : 0);
    if (!evidence)
        return;
    for (uint32_t i = 0; i < evidence->ngeneric_body_uses; i++) {
        const XgGenericBodyUseSummary *u = &evidence->generic_body_uses[i];
        fprintf(out,
                "generic-body-use id=%u inst=%u module=%u owner=%u origin_body=%u "
                "specialized_body=%u root=%u type=%u args=%u+%u size=%u flags=0x%x "
                "hash=%016" PRIx64 "\n",
                u->use_id, u->generic_inst_id, u->module_id, u->owner_func_id,
                u->origin_body_func_id, u->specialized_body_func_id, u->root_callsite_id,
                u->type_key, u->type_arg_key_start, (unsigned) u->type_arg_count,
                u->estimated_body_size, u->flags, u->body_use_hash);
    }
    for (uint32_t i = 0; i < evidence->ngeneric_storages; i++) {
        const XgGenericStorageSummary *s = &evidence->generic_storages[i];
        fprintf(out,
                "generic-storage id=%u inst=%u module=%u kind=%u origin_type=%u "
                "specialized_type=%u elem_type=%u key_type=%u value_type=%u container_plan=%u "
                "flags=0x%x hash=%016" PRIx64 "\n",
                s->storage_id, s->generic_inst_id, s->module_id, (unsigned) s->storage_kind,
                s->origin_type_key, s->specialized_type_key, s->elem_type_key, s->key_type_key,
                s->value_type_key, s->container_plan_id, s->flags, s->storage_hash);
    }
    for (uint32_t i = 0; i < evidence->ngeneric_code_sizes; i++) {
        const XgGenericCodeSizeSummary *s = &evidence->generic_code_sizes[i];
        fprintf(out,
                "generic-code-size id=%u inst=%u module=%u body_use=%u origin=%u specialized=%u "
                "count=%u threshold=%u flags=0x%x\n",
                s->code_size_id, s->generic_inst_id, s->module_id, s->body_use_id,
                s->origin_body_size_estimate, s->specialized_body_size_estimate,
                s->instantiation_count, s->threshold, s->flags);
    }
    for (uint32_t i = 0; i < evidence->nsequence_accesses; i++) {
        const XgSequenceAccessSummary *s = &evidence->sequence_accesses[i];
        fprintf(out,
                "sequence-access id=%u owner=%u span=%u ordinal=%u kind=%u access=%u "
                "receiver_type=%u elem_type=%u index=%u length=%u flags=0x%x\n",
                s->access_id, s->owner_func_id, s->source_span_id, s->body_ordinal,
                (unsigned) s->sequence_kind, (unsigned) s->access_kind, s->receiver_type_key,
                s->elem_type_key, s->index_expr_id, s->length_expr_id, s->flags);
    }
    for (uint32_t i = 0; i < evidence->ncapacity_ops; i++) {
        const XgCapacityOpSummary *c = &evidence->capacity_ops[i];
        fprintf(out,
                "capacity-op id=%u owner=%u span=%u ordinal=%u kind=%u op=%u receiver_type=%u "
                "elem_type=%u count=%u loop=%u flags=0x%x\n",
                c->op_id, c->owner_func_id, c->source_span_id, c->body_ordinal,
                (unsigned) c->sequence_kind, (unsigned) c->op_kind, c->receiver_type_key,
                c->elem_type_key, c->count_expr_id, c->loop_id, c->flags);
    }
    for (uint32_t i = 0; i < evidence->nbulk_ops; i++) {
        const XgBulkOpSummary *b = &evidence->bulk_ops[i];
        fprintf(out,
                "bulk-op id=%u owner=%u span=%u ordinal=%u op=%u elem_type=%u src_type=%u "
                "dst_type=%u length=%u flags=0x%x\n",
                b->op_id, b->owner_func_id, b->source_span_id, b->body_ordinal,
                (unsigned) b->op_kind, b->elem_type_key, b->src_type_key, b->dst_type_key,
                b->length_expr_id, b->flags);
    }
    for (uint32_t i = 0; i < evidence->nencoding_ops; i++) {
        const XgEncodingOpSummary *e = &evidence->encoding_ops[i];
        fprintf(out,
                "encoding-op id=%u owner=%u span=%u ordinal=%u op=%u input_type=%u "
                "output_type=%u flags=0x%x\n",
                e->op_id, e->owner_func_id, e->source_span_id, e->body_ordinal,
                (unsigned) e->op_kind, e->input_type_key, e->output_type_key, e->flags);
    }
    for (uint32_t i = 0; i < evidence->njson_shapes; i++) {
        const XgJsonShapeSummary *s = &evidence->json_shapes[i];
        fprintf(out,
                "json-shape id=%u record_shape=%u module=%u func=%u type=%u kind=%u span=%u "
                "fields=%u+%u "
                "flags=0x%x hash=%016" PRIx64 "\n",
                s->json_shape_id, s->record_shape_id, s->module_id, s->owner_func_id, s->type_key,
                (unsigned) s->shape_kind, s->source_span_id, s->field_name_start,
                (unsigned) s->field_count, s->flags, s->shape_hash);
    }
    for (uint32_t i = 0; i < evidence->njson_fields; i++) {
        const XgJsonFieldSummary *f = &evidence->json_fields[i];
        fprintf(out, "json-field id=%u shape=%u ordinal=%u name=%u type=%u flags=0x%x\n",
                f->field_id, f->shape_id, (unsigned) f->field_ordinal, f->name_id, f->type_key,
                f->flags);
    }
    for (uint32_t i = 0; i < evidence->njson_accesses; i++) {
        const XgJsonAccessSummary *a = &evidence->json_accesses[i];
        fprintf(out,
                "json-access id=%u module=%u func=%u shape=%u kind=%u span=%u key=%u "
                "result_type=%u field=%u flags=0x%x\n",
                a->json_access_id, a->module_id, a->owner_func_id, a->receiver_shape_id,
                (unsigned) a->access_kind, a->source_span_id, a->key_name_id, a->result_type_key,
                (unsigned) a->field_ordinal, a->flags);
    }
    for (uint32_t i = 0; i < evidence->njson_codecs; i++) {
        const XgJsonCodecSummary *c = &evidence->json_codecs[i];
        fprintf(
            out,
            "json-codec id=%u module=%u func=%u node=%u kind=%u span=%u input_type=%u "
            "target_type=%u input_shape=%u output_shape=%u record_shape=%u fields=%u flags=0x%x\n",
            c->codec_id, c->module_id, c->owner_func_id, c->source_node_id,
            (unsigned) c->codec_kind, c->source_span_id, c->input_type_key, c->target_type_key,
            c->input_shape_id, c->output_shape_id, c->record_shape_id, (unsigned) c->field_count,
            c->flags);
    }
    for (uint32_t i = 0; i < evidence->nrecord_shapes; i++) {
        const XgRecordShapeSummary *s = &evidence->record_shapes[i];
        fprintf(out,
                "record-shape id=%u json_shape=%u module=%u func=%u type=%u kind=%u span=%u "
                "fields=%u+%u "
                "flags=0x%x hash=%016" PRIx64 "\n",
                s->record_shape_id, s->json_shape_id, s->module_id, s->owner_func_id, s->type_key,
                (unsigned) s->shape_kind, s->source_span_id, s->field_name_start,
                (unsigned) s->field_count, s->flags, s->shape_hash);
    }
    for (uint32_t i = 0; i < evidence->nrecord_fields; i++) {
        const XgRecordFieldSummary *f = &evidence->record_fields[i];
        fprintf(out,
                "record-field id=%u shape=%u ordinal=%u name=%u type=%u default=%u flags=0x%x\n",
                f->field_id, f->shape_id, (unsigned) f->field_ordinal, f->name_id, f->type_key,
                f->default_value_id, f->flags);
    }
    for (uint32_t i = 0; i < evidence->nrecord_accesses; i++) {
        const XgRecordAccessSummary *a = &evidence->record_accesses[i];
        fprintf(out,
                "record-access id=%u module=%u func=%u shape=%u kind=%u span=%u field_name=%u "
                "result_type=%u field=%u flags=0x%x\n",
                a->record_access_id, a->module_id, a->owner_func_id, a->receiver_shape_id,
                (unsigned) a->access_kind, a->source_span_id, a->field_name_id, a->result_type_key,
                (unsigned) a->field_ordinal, a->flags);
    }
    for (uint32_t i = 0; i < evidence->nrecord_merges; i++) {
        const XgRecordMergeSummary *m = &evidence->record_merges[i];
        fprintf(
            out,
            "record-merge id=%u module=%u func=%u source=%u span=%u base_shape=%u patch_shape=%u "
            "result_shape=%u base_fields=%u patch_fields=%u result_fields=%u overwrites=%u "
            "copy_table=%u flags=0x%x hash=%016" PRIx64 "\n",
            m->merge_id, m->module_id, m->owner_func_id, m->source_node_id, m->source_span_id,
            m->base_shape_id, m->patch_shape_id, m->result_shape_id, (unsigned) m->base_field_count,
            (unsigned) m->patch_field_count, (unsigned) m->result_field_count,
            (unsigned) m->overwrite_count, m->copy_table_id, m->flags, m->merge_hash);
    }
    for (uint32_t i = 0; i < evidence->noptions_bags; i++) {
        const XgOptionsBagSummary *o = &evidence->options_bags[i];
        fprintf(out,
                "options-bag id=%u module=%u func=%u callsite=%u param_shape=%u "
                "supplied_shape=%u action=%u span=%u supplied_mask=%u default_mask=%u "
                "required_mask=%u supplied=%u defaults=%u required=%u flags=0x%x\n",
                o->options_id, o->module_id, o->owner_func_id, o->callsite_id, o->param_shape_id,
                o->supplied_shape_id, (unsigned) o->action, o->source_span_id,
                o->supplied_field_mask_id, o->default_field_mask_id, o->required_field_mask_id,
                (unsigned) o->supplied_count, (unsigned) o->default_count,
                (unsigned) o->required_count, o->flags);
    }
    for (uint32_t i = 0; i < evidence->nmap_shapes; i++) {
        const XgMapShapeSummary *s = &evidence->map_shapes[i];
        fprintf(out,
                "map-shape id=%u module=%u func=%u container=%u source=%u span=%u key_type=%u "
                "value_type=%u entries=%u+%u literal_count=%u flags=0x%x hash=%016" PRIx64 "\n",
                s->shape_id, s->module_id, s->owner_func_id, (unsigned) s->container_kind,
                (unsigned) s->source, s->source_span_id, s->key_type_key, s->value_type_key,
                s->entry_start, (unsigned) s->entry_count, s->literal_count, s->flags,
                s->shape_hash);
    }
    for (uint32_t i = 0; i < evidence->nmap_entries; i++) {
        const XgMapEntrySummary *e = &evidence->map_entries[i];
        fprintf(out,
                "map-entry id=%u shape=%u ordinal=%u key_const=%u value_const=%u key_i64=%" PRId64
                " prehash=%016" PRIx64 " flags=0x%x\n",
                e->entry_id, e->shape_id, e->entry_ordinal, e->key_const_id, e->value_const_id,
                e->key_i64, e->prehash, e->flags);
    }
    for (uint32_t i = 0; i < evidence->nkey_accesses; i++) {
        const XgKeyAccessSummary *a = &evidence->key_accesses[i];
        fprintf(out,
                "key-access id=%u func=%u span=%u ordinal=%u container=%u op=%u shape=%u "
                "receiver_type=%u key_type=%u value_type=%u key_const=%u prehash=%016" PRIx64
                " flags=0x%x\n",
                a->access_id, a->owner_func_id, a->source_span_id, a->body_ordinal,
                (unsigned) a->container_kind, (unsigned) a->op, a->receiver_shape_id,
                a->receiver_type_key, a->key_type_key, a->value_type_key, a->key_const_id,
                a->key_prehash, a->flags);
    }
    for (uint32_t i = 0; i < evidence->nhash_eqs; i++) {
        const XgHashEqSummary *h = &evidence->hash_eqs[i];
        fprintf(out,
                "hash-eq id=%u type=%u kind=%u eq_derive=%u hash_derive=%u eq_func=%u "
                "hash_func=%u flags=0x%x\n",
                h->hash_eq_id, h->type_key, (unsigned) h->kind, h->eq_derive_id, h->hash_derive_id,
                h->eq_func_id, h->hash_func_id, h->flags);
    }
}

static void dump_cache_payload_global(FILE *out, const XgGlobalEvidence *evidence) {
    fprintf(out, "payload-global v1 global_hash=%016" PRIx64 "\n",
            evidence ? xg_global_evidence_hash(evidence) : 0);
    dump_cache_payload_semantic(out, evidence);
    dump_cache_payload_body(out, evidence);
    dump_cache_payload_global_extra(out, evidence);
}

static void dump_cache_payload_body_for_phase(FILE *out, const XgGlobalEvidence *evidence,
                                              uint32_t phase) {
    switch ((XgEvidenceCachePhase) phase) {
        case XG_EVIDENCE_CACHE_DECLARATIONS:
            dump_cache_payload_declarations(out, evidence);
            break;
        case XG_EVIDENCE_CACHE_SEMANTIC_GRAPH:
            dump_cache_payload_semantic(out, evidence);
            break;
        case XG_EVIDENCE_CACHE_BODY_SUMMARY:
            dump_cache_payload_body(out, evidence);
            break;
        case XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE:
            dump_cache_payload_global(out, evidence);
            break;
        default:
            break;
    }
}

XR_FUNC char *xg_global_evidence_cache_payload_dump(const XgGlobalEvidence *evidence,
                                                    uint32_t phase) {
    XgEvidenceCacheRequestKey request_key;
    XgEvidenceCacheKey key;
    char request_line[320];
    char key_line[256];
    char *body = NULL;
    size_t body_sz = 0;
    FILE *body_out;
    char *buf = NULL;
    size_t buf_sz = 0;
    FILE *out;
    uint64_t body_hash;
    if (!evidence || !evidence_cache_phase_index(phase, NULL))
        return NULL;
    request_key = xg_global_evidence_cache_request_key(evidence, phase);
    key = xg_global_evidence_cache_key(evidence, phase);
    if (!xg_evidence_cache_request_key_format(&request_key, request_line, sizeof(request_line)))
        return NULL;
    if (!xg_evidence_cache_key_format(&key, key_line, sizeof(key_line)))
        return NULL;
    body_out = xr_open_memstream(&body, &body_sz);
    if (!body_out)
        return NULL;
    dump_cache_payload_body_for_phase(body_out, evidence, phase);
    if (xr_close_memstream(body_out, &body, &body_sz) != 0 || !body) {
        xr_free(body);
        return NULL;
    }
    body_hash = cache_payload_hash_bytes(body, strlen(body));
    out = xr_open_memstream(&buf, &buf_sz);
    if (!out) {
        xr_free(body);
        return NULL;
    }
    fprintf(out,
            "xg-cache-payload v2 phase=%u request=%016" PRIx64 " key=%016" PRIx64
            " payload=%016" PRIx64 " bytes=%zu\n",
            phase, xg_evidence_cache_request_key_hash(&request_key),
            xg_evidence_cache_key_hash(&key), body_hash, strlen(body));
    fprintf(out, "%s\n", request_line);
    fprintf(out, "%s\n", key_line);
    fputs(body, out);
    xr_free(body);
    if (xr_close_memstream(out, &buf, &buf_sz) != 0 || !buf) {
        xr_free(buf);
        return NULL;
    }
    return buf;
}

XR_FUNC bool xg_evidence_cache_payload_parse(const char *text,
                                             XgEvidenceCachePayloadInfo *out_info) {
    const char *cursor = text;
    char line[320];
    XgEvidenceCachePayloadInfo info;
    char trailing = '\0';
    if (!text || !out_info)
        return false;
    memset(&info, 0, sizeof(info));
    if (!evidence_cache_next_line(&cursor, line, sizeof(line)))
        return false;
    if (sscanf(line,
               "xg-cache-payload v2 phase=%" SCNu32 " request=%" SCNx64 " key=%" SCNx64
               " payload=%" SCNx64 " bytes=%zu %c",
               &info.phase, &info.request_hash, &info.key_hash, &info.payload_hash,
               &info.payload_bytes, &trailing) != 5)
        return false;
    if (!evidence_cache_phase_index(info.phase, NULL))
        return false;
    if (!evidence_cache_next_line(&cursor, line, sizeof(line)))
        return false;
    if (!xg_evidence_cache_request_key_parse(line, &info.request_key))
        return false;
    if (info.request_key.phase != info.phase ||
        info.request_hash != xg_evidence_cache_request_key_hash(&info.request_key))
        return false;
    if (!evidence_cache_next_line(&cursor, line, sizeof(line)))
        return false;
    if (!xg_evidence_cache_key_parse(line, &info.key))
        return false;
    if (info.key.phase != info.phase || info.key_hash != xg_evidence_cache_key_hash(&info.key))
        return false;
    if (info.key.module_id != info.request_key.module_id ||
        info.key.profile != info.request_key.profile ||
        info.key.compiler_semver_hash != info.request_key.compiler_semver_hash ||
        info.key.profile_hash != info.request_key.profile_hash ||
        info.key.imported_summary_hash != info.request_key.imported_summary_hash)
        return false;
    info.body = cursor;
    info.body_len = strlen(cursor);
    if (info.body_len != info.payload_bytes)
        return false;
    if (cache_payload_hash_bytes(info.body, info.body_len) != info.payload_hash)
        return false;
    *out_info = info;
    return true;
}

XR_FUNC bool xg_evidence_cache_payload_matches(const char *text,
                                               const XgEvidenceCacheKey *expected) {
    XgEvidenceCachePayloadInfo info;
    if (!expected || !xg_evidence_cache_payload_parse(text, &info))
        return false;
    if (info.phase != expected->phase || info.key_hash != xg_evidence_cache_key_hash(expected))
        return false;
    return xg_evidence_cache_key_matches(&info.key, expected);
}

XR_FUNC bool xg_evidence_cache_payload_request_matches(const char *text,
                                                       const XgEvidenceCacheRequestKey *expected) {
    XgEvidenceCachePayloadInfo info;
    if (!expected || !xg_evidence_cache_payload_parse(text, &info))
        return false;
    if (info.phase != expected->phase ||
        info.request_hash != xg_evidence_cache_request_key_hash(expected))
        return false;
    return xg_evidence_cache_request_key_matches(&info.request_key, expected);
}

static bool materialize_payload_declarations(const char **cursor, XgGlobalEvidence *evidence,
                                             uint32_t *out_module_count, uint32_t *out_decl_count) {
    char line[1024];
    uint32_t module_count = 0;
    uint32_t decl_count = 0;
    char trailing = '\0';
    if (!cursor || !evidence || !evidence_cache_next_line(cursor, line, sizeof(line)))
        return false;
    if (sscanf(line, "payload-count modules=%" SCNu32 " decls=%" SCNu32 " %c", &module_count,
               &decl_count, &trailing) != 2)
        return false;
    if (!xg_global_evidence_reserve_modules(evidence, module_count) ||
        !xg_global_evidence_reserve_decls(evidence, decl_count))
        return false;
    for (uint32_t i = 0; i < module_count; i++) {
        XgModuleSummary row;
        uint32_t kind = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "module id=%" SCNu32 " name=%" SCNu32 " canonical=%" SCNx64 " source=%" SCNx64
                   " kind=%" SCNu32 " flags=0x%" SCNx32 " %c",
                   &row.module_id, &row.name_id, &row.canonical_hash, &row.source_hash, &kind,
                   &row.flags, &trailing) != 6)
            return false;
        if (kind > UINT8_MAX)
            return false;
        row.kind = (uint8_t) kind;
        if (!xg_global_evidence_add_module(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < decl_count; i++) {
        XgDeclSummary row;
        uint32_t kind = 0;
        uint32_t storage_owner = 0;
        uint32_t storage_mutability = 0;
        uint32_t address_identity = 0;
        uint32_t materialization_kind = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "decl id=%" SCNu32 " module=%" SCNu32 " node=%" SCNu32 " kind=%" SCNu32
                   " flags=0x%" SCNx32 " name=%" SCNu32 " type=%" SCNu32 " sig=%" SCNu32
                   " span=%" SCNu32 " derive=0x%" SCNx32 " storage_flags=0x%" SCNx32
                   " owner=%" SCNu32 " mutability=%" SCNu32 " address=%" SCNu32
                   " materialize=%" SCNu32 " %c",
                   &row.decl_id, &row.module_id, &row.source_node_id, &kind, &row.flags,
                   &row.name_id, &row.type_key, &row.signature_key, &row.source_span_id,
                   &row.derive_flags, &row.storage_flags, &storage_owner, &storage_mutability,
                   &address_identity, &materialization_kind, &trailing) != 15)
            return false;
        if (kind > UINT8_MAX || storage_owner > UINT8_MAX || storage_mutability > UINT8_MAX ||
            address_identity > UINT8_MAX || materialization_kind > UINT8_MAX)
            return false;
        row.kind = (uint8_t) kind;
        row.storage_owner = (uint8_t) storage_owner;
        row.storage_mutability = (uint8_t) storage_mutability;
        row.address_identity = (uint8_t) address_identity;
        row.materialization_kind = (uint8_t) materialization_kind;
        if (!xg_global_evidence_add_decl(evidence, &row))
            return false;
    }
    if (out_module_count)
        *out_module_count = module_count;
    if (out_decl_count)
        *out_decl_count = decl_count;
    return true;
}

static bool materialize_payload_semantic_cursor(const char **cursor, XgGlobalEvidence *evidence) {
    char line[1024];
    uint32_t module_count = 0;
    uint32_t decl_count = 0;
    uint32_t class_count = 0;
    uint32_t class_field_count = 0;
    uint32_t method_count = 0;
    uint32_t impl_count = 0;
    uint32_t extends_count = 0;
    uint32_t interface_method_count = 0;
    uint32_t derive_count = 0;
    uint32_t derived_field_count = 0;
    uint32_t derived_method_count = 0;
    uint32_t parsed_module_count = 0;
    uint32_t parsed_decl_count = 0;
    char trailing = '\0';
    if (!cursor || !*cursor || !evidence || !evidence_cache_next_line(cursor, line, sizeof(line)))
        return false;
    if (sscanf(line,
               "payload-count modules=%" SCNu32 " decls=%" SCNu32 " classes=%" SCNu32
               " class_fields=%" SCNu32 " methods=%" SCNu32 " impls=%" SCNu32 " extends=%" SCNu32
               " interface_methods=%" SCNu32 " derives=%" SCNu32 " derived_fields=%" SCNu32
               " derived_methods=%" SCNu32 " %c",
               &module_count, &decl_count, &class_count, &class_field_count, &method_count,
               &impl_count, &extends_count, &interface_method_count, &derive_count,
               &derived_field_count, &derived_method_count, &trailing) != 11)
        return false;
    if (!materialize_payload_declarations(cursor, evidence, &parsed_module_count,
                                          &parsed_decl_count) ||
        parsed_module_count != module_count || parsed_decl_count != decl_count)
        return false;
    if (!xg_global_evidence_reserve_classes(evidence, class_count) ||
        !xg_global_evidence_reserve_class_fields(evidence, class_field_count) ||
        !xg_global_evidence_reserve_methods(evidence, method_count) ||
        !xg_global_evidence_reserve_interface_impls(evidence, impl_count) ||
        !xg_global_evidence_reserve_interface_extends(evidence, extends_count) ||
        !xg_global_evidence_reserve_interface_methods(evidence, interface_method_count) ||
        !xg_global_evidence_reserve_derives(evidence, derive_count) ||
        !xg_global_evidence_reserve_derived_fields(evidence, derived_field_count) ||
        !xg_global_evidence_reserve_derived_methods(evidence, derived_method_count))
        return false;
    for (uint32_t i = 0; i < class_count; i++) {
        XgClassSummary row;
        uint32_t type_arg_count = 0;
        uint32_t decl_kind = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "class id=%" SCNu32 " module=%" SCNu32 " decl=%" SCNu32 " name=%" SCNu32
                   " parent=%" SCNu32 " flags=0x%" SCNx32 " fields=%" SCNu32 "+%" SCNu32
                   " methods=%" SCNu32 "+%" SCNu32 " impls=%" SCNu32 "+%" SCNu32 " origin=%" SCNu32
                   " origin_name=%" SCNu32 " type=%" SCNu32 " args=%" SCNu32 "+%" SCNu32
                   " decl_kind=%" SCNu32 " %c",
                   &row.class_id, &row.module_id, &row.decl_id, &row.name_id, &row.parent_class_id,
                   &row.flags, &row.field_start, &row.field_count, &row.method_start,
                   &row.method_count, &row.interface_start, &row.interface_count,
                   &row.generic_origin_class_id, &row.generic_origin_name_id, &row.generic_type_key,
                   &row.generic_type_arg_key_start, &type_arg_count, &decl_kind, &trailing) != 18)
            return false;
        row.generic_type_arg_count = (uint16_t) type_arg_count;
        row.decl_kind = (uint8_t) decl_kind;
        if (!xg_global_evidence_add_class(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < class_field_count; i++) {
        XgClassFieldSummary row;
        uint32_t semantic_kind = 0;
        uint32_t native_width = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "class-field id=%" SCNu32 " module=%" SCNu32 " node=%" SCNu32 " owner=%" SCNu32
                   " name=%" SCNu32 " type=%" SCNu32 " target_name=%" SCNu32
                   " target_class=%" SCNu32 " target_interface=%" SCNu32 " element=%" SCNu32
                   " key=%" SCNu32 " value=%" SCNu32 " fixed=%" SCNu32 " ordinal=%" SCNu32
                   " slot=%" SCNu32 " semantic=%" SCNu32 " width=%" SCNu32 " flags=0x%" SCNx32
                   " %c",
                   &row.field_id, &row.module_id, &row.source_node_id, &row.owner_class_id,
                   &row.name_id, &row.type_key, &row.target_name_id, &row.target_class_id,
                   &row.target_interface_id, &row.element_type_key, &row.key_type_key,
                   &row.value_type_key, &row.fixed_length, &row.decl_ordinal, &row.instance_slot,
                   &semantic_kind, &native_width, &row.flags, &trailing) != 18)
            return false;
        if (semantic_kind == 0 || semantic_kind > XG_CLASS_FIELD_TYPE_DYNAMIC ||
            native_width > UINT8_MAX)
            return false;
        row.semantic_kind = (uint8_t) semantic_kind;
        row.native_width = (uint8_t) native_width;
        if (!xg_global_evidence_add_class_field(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < method_count; i++) {
        XgMethodSummary row;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "method id=%" SCNu32 " owner=%" SCNu32 " node=%" SCNu32 " name=%" SCNu32
                   " sig=%" SCNu32 " override=%" SCNu32 " root=%" SCNu32 " depth=%" SCNu32
                   " default=%" SCNu32 " flags=0x%" SCNx32 " %c",
                   &row.method_id, &row.owner_class_id, &row.source_node_id, &row.name_id,
                   &row.signature_key, &row.override_of, &row.root_method_id, &row.override_depth,
                   &row.default_arg_contract_id, &row.flags, &trailing) != 10)
            return false;
        if (!xg_global_evidence_add_method(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < impl_count; i++) {
        XgInterfaceImplSummary row;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "interface-impl class=%" SCNu32 " interface=%" SCNu32 " name=%" SCNu32
                   " type=%" SCNu32 " span=%" SCNu32 " flags=0x%" SCNx32 " %c",
                   &row.implementor_class_id, &row.interface_id, &row.name_id, &row.type_key,
                   &row.source_span_id, &row.flags, &trailing) != 6)
            return false;
        if (!xg_global_evidence_add_interface_impl(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < extends_count; i++) {
        XgInterfaceExtendsSummary row;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "interface-extends child=%" SCNu32 " parent=%" SCNu32 " name=%" SCNu32
                   " type=%" SCNu32 " span=%" SCNu32 " flags=0x%" SCNx32 " %c",
                   &row.child_interface_id, &row.parent_interface_id, &row.name_id, &row.type_key,
                   &row.source_span_id, &row.flags, &trailing) != 6)
            return false;
        if (!xg_global_evidence_add_interface_extends(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < interface_method_count; i++) {
        XgInterfaceMethodSummary row;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "interface-method id=%" SCNu32 " owner=%" SCNu32 " name=%" SCNu32 " sig=%" SCNu32
                   " ordinal=%" SCNu32 " span=%" SCNu32 " flags=0x%" SCNx32 " %c",
                   &row.interface_method_id, &row.owner_interface_id, &row.name_id,
                   &row.signature_key, &row.ordinal, &row.source_span_id, &row.flags,
                   &trailing) != 7)
            return false;
        if (!xg_global_evidence_add_interface_method(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < derive_count; i++) {
        XgDeriveSummary row;
        uint32_t derive_kind = 0;
        uint32_t field_count = 0;
        uint32_t method_count_row = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "derive id=%" SCNu32 " module=%" SCNu32 " decl=%" SCNu32 " span=%" SCNu32
                   " type=%" SCNu32 " kind=%" SCNu32 " fields=%" SCNu32 "+%" SCNu32
                   " methods=%" SCNu32 "+%" SCNu32 " flags=0x%" SCNx32 " hash=%" SCNx64 " %c",
                   &row.derive_id, &row.module_id, &row.owner_decl_id, &row.source_span_id,
                   &row.type_key, &derive_kind, &row.field_start, &field_count, &row.method_start,
                   &method_count_row, &row.flags, &row.derive_hash, &trailing) != 12)
            return false;
        row.derive_kind = (uint8_t) derive_kind;
        row.field_count = (uint16_t) field_count;
        row.method_count = (uint16_t) method_count_row;
        if (!xg_global_evidence_add_derive(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < derived_field_count; i++) {
        XgDerivedFieldSummary row;
        uint32_t field_ordinal = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "derived-field id=%" SCNu32 " derive=%" SCNu32 " ordinal=%" SCNu32
                   " name=%" SCNu32 " type=%" SCNu32 " source=%" SCNu32 " flags=0x%" SCNx32 " %c",
                   &row.field_id, &row.derive_id, &field_ordinal, &row.name_id, &row.type_key,
                   &row.source_field_id, &row.flags, &trailing) != 7)
            return false;
        row.field_ordinal = (uint16_t) field_ordinal;
        if (!xg_global_evidence_add_derived_field(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < derived_method_count; i++) {
        XgDerivedMethodSummary row;
        uint32_t method_kind = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "derived-method id=%" SCNu32 " derive=%" SCNu32 " kind=%" SCNu32 " body=%" SCNu32
                   " sig=%" SCNu32 " flags=0x%" SCNx32 " %c",
                   &row.method_id, &row.derive_id, &method_kind, &row.generated_body_func_id,
                   &row.signature_key, &row.flags, &trailing) != 6)
            return false;
        row.method_kind = (uint8_t) method_kind;
        if (!xg_global_evidence_add_derived_method(evidence, &row))
            return false;
    }
    return true;
}

static bool materialize_payload_semantic(const char *body, XgGlobalEvidence *evidence) {
    const char *cursor = body;
    return materialize_payload_semantic_cursor(&cursor, evidence) && *cursor == '\0';
}

static bool materialize_payload_body_cursor(const char **cursor, XgGlobalEvidence *evidence) {
    char line[1024];
    uint32_t body_count = 0;
    uint32_t param_storage_count = 0;
    uint32_t callsite_count = 0;
    uint32_t interface_object_use_count = 0;
    uint32_t link_dep_count = 0;
    uint32_t generic_inst_count = 0;
    char trailing = '\0';
    if (!cursor || !*cursor || !evidence || !evidence_cache_next_line(cursor, line, sizeof(line)))
        return false;
    if (sscanf(line,
               "payload-count bodies=%" SCNu32 " param_storages=%" SCNu32 " callsites=%" SCNu32
               " interface_object_uses=%" SCNu32 " link_deps=%" SCNu32 " generic_insts=%" SCNu32
               " %c",
               &body_count, &param_storage_count, &callsite_count, &interface_object_use_count,
               &link_dep_count, &generic_inst_count, &trailing) != 6)
        return false;
    if (!xg_global_evidence_reserve_bodies(evidence, body_count) ||
        !xg_global_evidence_reserve_param_storages(evidence, param_storage_count) ||
        !xg_global_evidence_reserve_callsites(evidence, callsite_count) ||
        !xg_global_evidence_reserve_interface_object_uses(evidence, interface_object_use_count) ||
        !xg_global_evidence_reserve_link_deps(evidence, link_dep_count) ||
        !xg_global_evidence_reserve_generic_insts(evidence, generic_inst_count))
        return false;
    for (uint32_t i = 0; i < body_count; i++) {
        XgBodySummary row;
        uint32_t kind = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "body id=%" SCNu32 " module=%" SCNu32 " node=%" SCNu32 " decl=%" SCNu32
                   " class=%" SCNu32 " method=%" SCNu32 " name=%" SCNu32 " sig=%" SCNu32
                   " span=%" SCNu32 " kind=%" SCNu32 " hash=%" SCNx64 " effect=0x%" SCNx32
                   " escape=0x%" SCNx32 " caps=0x%" SCNx32 " param_storage=%" SCNu32
                   " params=%" SCNu32 "+%" SCNu32 " calls=%" SCNu32 "+%" SCNu32
                   " metadata=0x%" SCNx32 " static=0x%" SCNx32 " %c",
                   &row.func_id, &row.module_id, &row.source_node_id, &row.owner_decl_id,
                   &row.owner_class_id, &row.owner_method_id, &row.name_id, &row.signature_key,
                   &row.source_span_id, &kind, &row.body_hash, &row.effect_bits, &row.escape_bits,
                   &row.capability_bits, &row.param_storage_key, &row.param_storage_start,
                   &row.param_storage_count, &row.callsite_start, &row.callsite_count,
                   &row.metadata_use_bits, &row.static_data_use_bits, &trailing) != 21)
            return false;
        row.kind = (uint8_t) kind;
        if (!xg_global_evidence_add_body(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < param_storage_count; i++) {
        XgParamStorageSummary row;
        uint32_t storage_owner = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "param-storage id=%" SCNu32 " owner=%" SCNu32 " index=%" SCNu32
                   " storage=%" SCNu32 " flags=0x%" SCNx32 " %c",
                   &row.requirement_id, &row.owner_func_id, &row.param_index, &storage_owner,
                   &row.flags, &trailing) != 5)
            return false;
        row.storage_owner = (uint8_t) storage_owner;
        if (!xg_global_evidence_add_param_storage(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < callsite_count; i++) {
        XgCallsiteSummary row;
        uint32_t kind = 0;
        uint32_t arg_count = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "callsite id=%" SCNu32 " owner=%" SCNu32 " node=%" SCNu32 " span=%" SCNu32
                   " ordinal=%" SCNu32 " kind=%" SCNu32 " target=%" SCNu32 " recv_class=%" SCNu32
                   " recv_interface=%" SCNu32 " method=%" SCNu32 " name=%" SCNu32 " sig=%" SCNu32
                   " args=%" SCNu32 "+%" SCNu32 " flags=0x%" SCNx32 " %c",
                   &row.callsite_id, &row.owner_func_id, &row.source_node_id, &row.source_span_id,
                   &row.body_ordinal, &kind, &row.static_target_func_id,
                   &row.receiver_static_class_id, &row.receiver_static_interface_id, &row.method_id,
                   &row.method_name_id, &row.method_signature_key, &row.arg_type_key_start,
                   &arg_count, &row.flags, &trailing) != 15)
            return false;
        row.kind = (uint8_t) kind;
        row.arg_count = (uint16_t) arg_count;
        if (!xg_global_evidence_add_callsite(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < interface_object_use_count; i++) {
        XgInterfaceObjectUseSummary row;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "interface-object-use id=%" SCNu32 " interface=%" SCNu32 " owner=%" SCNu32
                   " span=%" SCNu32 " ordinal=%" SCNu32 " type=%" SCNu32 " reason=0x%" SCNx32
                   " flags=0x%" SCNx32 " %c",
                   &row.use_id, &row.interface_id, &row.owner_func_id, &row.source_span_id,
                   &row.body_ordinal, &row.type_key, &row.reason, &row.flags, &trailing) != 8)
            return false;
        if (!xg_global_evidence_add_interface_object_use(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < link_dep_count; i++) {
        XgLinkDependencySummary row;
        uint32_t kind = 0;
        char name[XG_LINK_DEP_NAME_MAX];
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        memset(name, 0, sizeof(name));
        if (sscanf(line,
                   "link-dep id=%" SCNu32 " module=%" SCNu32 " decl=%" SCNu32 " span=%" SCNu32
                   " name_id=%" SCNu32 " kind=%" SCNu32 " flags=0x%" SCNx32 " name=%511[^\n]%c",
                   &row.link_id, &row.module_id, &row.decl_id, &row.source_span_id, &row.name_id,
                   &kind, &row.flags, name, &trailing) < 8)
            return false;
        row.kind = (uint8_t) kind;
        snprintf(row.name, sizeof(row.name), "%s", name);
        if (!xg_global_evidence_add_link_dependency(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < generic_inst_count; i++) {
        XgGenericInstSummary row;
        uint32_t type_arg_count = 0;
        uint32_t kind = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "generic-inst id=%" SCNu32 " module=%" SCNu32 " origin_decl=%" SCNu32
                   " origin_func=%" SCNu32 " origin_method=%" SCNu32 " origin_class=%" SCNu32
                   " spec_func=%" SCNu32 " spec_class=%" SCNu32 " root=%" SCNu32
                   " constraint=%" SCNu32 " name=%" SCNu32 " type=%" SCNu32 " args=%" SCNu32
                   "+%" SCNu32 " span=%" SCNu32 " kind=%" SCNu32 " flags=0x%" SCNx32 " %c",
                   &row.generic_inst_id, &row.module_id, &row.origin_decl_id, &row.origin_func_id,
                   &row.origin_method_id, &row.origin_class_id, &row.specialized_func_id,
                   &row.specialized_class_id, &row.root_callsite_id, &row.constraint_interface_id,
                   &row.name_id, &row.type_key, &row.type_arg_key_start, &type_arg_count,
                   &row.source_span_id, &kind, &row.flags, &trailing) != 17)
            return false;
        row.type_arg_count = (uint16_t) type_arg_count;
        row.kind = (uint8_t) kind;
        if (!xg_global_evidence_add_generic_inst(evidence, &row))
            return false;
    }
    return true;
}

static bool materialize_payload_body(const char *body, XgGlobalEvidence *evidence) {
    const char *cursor = body;
    return materialize_payload_body_cursor(&cursor, evidence) && *cursor == '\0';
}

static bool materialize_payload_global_extra(const char **cursor, XgGlobalEvidence *evidence) {
    char line[1024];
    uint32_t generic_body_use_count = 0;
    uint32_t generic_storage_count = 0;
    uint32_t generic_code_size_count = 0;
    uint32_t sequence_access_count = 0;
    uint32_t capacity_op_count = 0;
    uint32_t bulk_op_count = 0;
    uint32_t encoding_op_count = 0;
    uint32_t json_shape_count = 0;
    uint32_t json_field_count = 0;
    uint32_t json_access_count = 0;
    uint32_t json_codec_count = 0;
    uint32_t record_shape_count = 0;
    uint32_t record_field_count = 0;
    uint32_t record_access_count = 0;
    uint32_t record_merge_count = 0;
    uint32_t options_bag_count = 0;
    uint32_t map_shape_count = 0;
    uint32_t map_entry_count = 0;
    uint32_t key_access_count = 0;
    uint32_t hash_eq_count = 0;
    char trailing = '\0';

    if (!cursor || !*cursor || !evidence || !evidence_cache_next_line(cursor, line, sizeof(line)))
        return false;
    if (sscanf(line,
               "payload-extra v1 generic_body_uses=%" SCNu32 " generic_storages=%" SCNu32
               " generic_code_sizes=%" SCNu32 " seq=%" SCNu32 " capacity=%" SCNu32 " bulk=%" SCNu32
               " encoding=%" SCNu32 " json_shapes=%" SCNu32 " json_fields=%" SCNu32
               " json_accesses=%" SCNu32 " json_codecs=%" SCNu32 " record_shapes=%" SCNu32
               " record_fields=%" SCNu32 " record_accesses=%" SCNu32 " record_merges=%" SCNu32
               " options=%" SCNu32 " map_shapes=%" SCNu32 " map_entries=%" SCNu32
               " key_accesses=%" SCNu32 " hash_eqs=%" SCNu32 " %c",
               &generic_body_use_count, &generic_storage_count, &generic_code_size_count,
               &sequence_access_count, &capacity_op_count, &bulk_op_count, &encoding_op_count,
               &json_shape_count, &json_field_count, &json_access_count, &json_codec_count,
               &record_shape_count, &record_field_count, &record_access_count, &record_merge_count,
               &options_bag_count, &map_shape_count, &map_entry_count, &key_access_count,
               &hash_eq_count, &trailing) != 20)
        return false;

    if (!xg_global_evidence_reserve_generic_body_uses(evidence, generic_body_use_count) ||
        !xg_global_evidence_reserve_generic_storages(evidence, generic_storage_count) ||
        !xg_global_evidence_reserve_generic_code_sizes(evidence, generic_code_size_count) ||
        !xg_global_evidence_reserve_sequence_accesses(evidence, sequence_access_count) ||
        !xg_global_evidence_reserve_capacity_ops(evidence, capacity_op_count) ||
        !xg_global_evidence_reserve_bulk_ops(evidence, bulk_op_count) ||
        !xg_global_evidence_reserve_encoding_ops(evidence, encoding_op_count) ||
        !xg_global_evidence_reserve_json_shapes(evidence, json_shape_count) ||
        !xg_global_evidence_reserve_json_fields(evidence, json_field_count) ||
        !xg_global_evidence_reserve_json_accesses(evidence, json_access_count) ||
        !xg_global_evidence_reserve_json_codecs(evidence, json_codec_count) ||
        !xg_global_evidence_reserve_record_shapes(evidence, record_shape_count) ||
        !xg_global_evidence_reserve_record_fields(evidence, record_field_count) ||
        !xg_global_evidence_reserve_record_accesses(evidence, record_access_count) ||
        !xg_global_evidence_reserve_record_merges(evidence, record_merge_count) ||
        !xg_global_evidence_reserve_options_bags(evidence, options_bag_count) ||
        !xg_global_evidence_reserve_map_shapes(evidence, map_shape_count) ||
        !xg_global_evidence_reserve_map_entries(evidence, map_entry_count) ||
        !xg_global_evidence_reserve_key_accesses(evidence, key_access_count) ||
        !xg_global_evidence_reserve_hash_eqs(evidence, hash_eq_count))
        return false;

    for (uint32_t i = 0; i < generic_body_use_count; i++) {
        XgGenericBodyUseSummary row;
        uint32_t type_arg_count = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "generic-body-use id=%" SCNu32 " inst=%" SCNu32 " module=%" SCNu32
                   " owner=%" SCNu32 " origin_body=%" SCNu32 " specialized_body=%" SCNu32
                   " root=%" SCNu32 " type=%" SCNu32 " args=%" SCNu32 "+%" SCNu32 " size=%" SCNu32
                   " flags=0x%" SCNx32 " hash=%" SCNx64 " %c",
                   &row.use_id, &row.generic_inst_id, &row.module_id, &row.owner_func_id,
                   &row.origin_body_func_id, &row.specialized_body_func_id, &row.root_callsite_id,
                   &row.type_key, &row.type_arg_key_start, &type_arg_count,
                   &row.estimated_body_size, &row.flags, &row.body_use_hash, &trailing) != 13)
            return false;
        row.type_arg_count = (uint16_t) type_arg_count;
        if (!xg_global_evidence_add_generic_body_use(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < generic_storage_count; i++) {
        XgGenericStorageSummary row;
        uint32_t storage_kind = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "generic-storage id=%" SCNu32 " inst=%" SCNu32 " module=%" SCNu32
                   " kind=%" SCNu32 " origin_type=%" SCNu32 " specialized_type=%" SCNu32
                   " elem_type=%" SCNu32 " key_type=%" SCNu32 " value_type=%" SCNu32
                   " container_plan=%" SCNu32 " flags=0x%" SCNx32 " hash=%" SCNx64 " %c",
                   &row.storage_id, &row.generic_inst_id, &row.module_id, &storage_kind,
                   &row.origin_type_key, &row.specialized_type_key, &row.elem_type_key,
                   &row.key_type_key, &row.value_type_key, &row.container_plan_id, &row.flags,
                   &row.storage_hash, &trailing) != 12)
            return false;
        row.storage_kind = (uint8_t) storage_kind;
        if (!xg_global_evidence_add_generic_storage(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < generic_code_size_count; i++) {
        XgGenericCodeSizeSummary row;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "generic-code-size id=%" SCNu32 " inst=%" SCNu32 " module=%" SCNu32
                   " body_use=%" SCNu32 " origin=%" SCNu32 " specialized=%" SCNu32 " count=%" SCNu32
                   " threshold=%" SCNu32 " flags=0x%" SCNx32 " %c",
                   &row.code_size_id, &row.generic_inst_id, &row.module_id, &row.body_use_id,
                   &row.origin_body_size_estimate, &row.specialized_body_size_estimate,
                   &row.instantiation_count, &row.threshold, &row.flags, &trailing) != 9)
            return false;
        if (!xg_global_evidence_add_generic_code_size(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < sequence_access_count; i++) {
        XgSequenceAccessSummary row;
        uint32_t sequence_kind = 0;
        uint32_t access_kind = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "sequence-access id=%" SCNu32 " owner=%" SCNu32 " span=%" SCNu32
                   " ordinal=%" SCNu32 " kind=%" SCNu32 " access=%" SCNu32 " receiver_type=%" SCNu32
                   " elem_type=%" SCNu32 " index=%" SCNu32 " length=%" SCNu32 " flags=0x%" SCNx32
                   " %c",
                   &row.access_id, &row.owner_func_id, &row.source_span_id, &row.body_ordinal,
                   &sequence_kind, &access_kind, &row.receiver_type_key, &row.elem_type_key,
                   &row.index_expr_id, &row.length_expr_id, &row.flags, &trailing) != 11)
            return false;
        row.sequence_kind = (uint8_t) sequence_kind;
        row.access_kind = (uint8_t) access_kind;
        if (!xg_global_evidence_add_sequence_access(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < capacity_op_count; i++) {
        XgCapacityOpSummary row;
        uint32_t sequence_kind = 0;
        uint32_t op_kind = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "capacity-op id=%" SCNu32 " owner=%" SCNu32 " span=%" SCNu32 " ordinal=%" SCNu32
                   " kind=%" SCNu32 " op=%" SCNu32 " receiver_type=%" SCNu32 " elem_type=%" SCNu32
                   " count=%" SCNu32 " loop=%" SCNu32 " flags=0x%" SCNx32 " %c",
                   &row.op_id, &row.owner_func_id, &row.source_span_id, &row.body_ordinal,
                   &sequence_kind, &op_kind, &row.receiver_type_key, &row.elem_type_key,
                   &row.count_expr_id, &row.loop_id, &row.flags, &trailing) != 11)
            return false;
        row.sequence_kind = (uint8_t) sequence_kind;
        row.op_kind = (uint8_t) op_kind;
        if (!xg_global_evidence_add_capacity_op(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < bulk_op_count; i++) {
        XgBulkOpSummary row;
        uint32_t op_kind = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "bulk-op id=%" SCNu32 " owner=%" SCNu32 " span=%" SCNu32 " ordinal=%" SCNu32
                   " op=%" SCNu32 " elem_type=%" SCNu32 " src_type=%" SCNu32 " dst_type=%" SCNu32
                   " length=%" SCNu32 " flags=0x%" SCNx32 " %c",
                   &row.op_id, &row.owner_func_id, &row.source_span_id, &row.body_ordinal, &op_kind,
                   &row.elem_type_key, &row.src_type_key, &row.dst_type_key, &row.length_expr_id,
                   &row.flags, &trailing) != 10)
            return false;
        row.op_kind = (uint8_t) op_kind;
        if (!xg_global_evidence_add_bulk_op(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < encoding_op_count; i++) {
        XgEncodingOpSummary row;
        uint32_t op_kind = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "encoding-op id=%" SCNu32 " owner=%" SCNu32 " span=%" SCNu32 " ordinal=%" SCNu32
                   " op=%" SCNu32 " input_type=%" SCNu32 " output_type=%" SCNu32 " flags=0x%" SCNx32
                   " %c",
                   &row.op_id, &row.owner_func_id, &row.source_span_id, &row.body_ordinal, &op_kind,
                   &row.input_type_key, &row.output_type_key, &row.flags, &trailing) != 8)
            return false;
        row.op_kind = (uint8_t) op_kind;
        if (!xg_global_evidence_add_encoding_op(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < json_shape_count; i++) {
        XgJsonShapeSummary row;
        uint32_t field_count = 0;
        uint32_t shape_kind = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "json-shape id=%" SCNu32 " record_shape=%" SCNu32 " module=%" SCNu32
                   " func=%" SCNu32 " type=%" SCNu32 " kind=%" SCNu32 " span=%" SCNu32
                   " fields=%" SCNu32 "+%" SCNu32 " flags=0x%" SCNx32 " hash=%" SCNx64 " %c",
                   &row.json_shape_id, &row.record_shape_id, &row.module_id, &row.owner_func_id,
                   &row.type_key, &shape_kind, &row.source_span_id, &row.field_name_start,
                   &field_count, &row.flags, &row.shape_hash, &trailing) != 11)
            return false;
        row.shape_kind = (uint8_t) shape_kind;
        row.field_count = (uint16_t) field_count;
        if (!xg_global_evidence_add_json_shape(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < json_field_count; i++) {
        XgJsonFieldSummary row;
        uint32_t field_ordinal = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "json-field id=%" SCNu32 " shape=%" SCNu32 " ordinal=%" SCNu32 " name=%" SCNu32
                   " type=%" SCNu32 " flags=0x%" SCNx32 " %c",
                   &row.field_id, &row.shape_id, &field_ordinal, &row.name_id, &row.type_key,
                   &row.flags, &trailing) != 6)
            return false;
        row.field_ordinal = (uint16_t) field_ordinal;
        if (!xg_global_evidence_add_json_field(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < json_access_count; i++) {
        XgJsonAccessSummary row;
        uint32_t access_kind = 0;
        uint32_t field_ordinal = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "json-access id=%" SCNu32 " module=%" SCNu32 " func=%" SCNu32 " shape=%" SCNu32
                   " kind=%" SCNu32 " span=%" SCNu32 " key=%" SCNu32 " result_type=%" SCNu32
                   " field=%" SCNu32 " flags=0x%" SCNx32 " %c",
                   &row.json_access_id, &row.module_id, &row.owner_func_id, &row.receiver_shape_id,
                   &access_kind, &row.source_span_id, &row.key_name_id, &row.result_type_key,
                   &field_ordinal, &row.flags, &trailing) != 10)
            return false;
        row.access_kind = (uint8_t) access_kind;
        row.field_ordinal = (uint16_t) field_ordinal;
        if (!xg_global_evidence_add_json_access(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < json_codec_count; i++) {
        XgJsonCodecSummary row;
        uint32_t codec_kind = 0;
        uint32_t field_count = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "json-codec id=%" SCNu32 " module=%" SCNu32 " func=%" SCNu32 " node=%" SCNu32
                   " kind=%" SCNu32 " span=%" SCNu32 " input_type=%" SCNu32 " target_type=%" SCNu32
                   " input_shape=%" SCNu32 " output_shape=%" SCNu32 " record_shape=%" SCNu32
                   " fields=%" SCNu32 " flags=0x%" SCNx32 " %c",
                   &row.codec_id, &row.module_id, &row.owner_func_id, &row.source_node_id,
                   &codec_kind, &row.source_span_id, &row.input_type_key, &row.target_type_key,
                   &row.input_shape_id, &row.output_shape_id, &row.record_shape_id, &field_count,
                   &row.flags, &trailing) != 13)
            return false;
        row.codec_kind = (uint8_t) codec_kind;
        row.field_count = (uint16_t) field_count;
        if (!xg_global_evidence_add_json_codec(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < record_shape_count; i++) {
        XgRecordShapeSummary row;
        uint32_t field_count = 0;
        uint32_t shape_kind = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "record-shape id=%" SCNu32 " json_shape=%" SCNu32 " module=%" SCNu32
                   " func=%" SCNu32 " type=%" SCNu32 " kind=%" SCNu32 " span=%" SCNu32
                   " fields=%" SCNu32 "+%" SCNu32 " flags=0x%" SCNx32 " hash=%" SCNx64 " %c",
                   &row.record_shape_id, &row.json_shape_id, &row.module_id, &row.owner_func_id,
                   &row.type_key, &shape_kind, &row.source_span_id, &row.field_name_start,
                   &field_count, &row.flags, &row.shape_hash, &trailing) != 11)
            return false;
        row.shape_kind = (uint8_t) shape_kind;
        row.field_count = (uint16_t) field_count;
        if (!xg_global_evidence_add_record_shape(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < record_field_count; i++) {
        XgRecordFieldSummary row;
        uint32_t field_ordinal = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "record-field id=%" SCNu32 " shape=%" SCNu32 " ordinal=%" SCNu32 " name=%" SCNu32
                   " type=%" SCNu32 " default=%" SCNu32 " flags=0x%" SCNx32 " %c",
                   &row.field_id, &row.shape_id, &field_ordinal, &row.name_id, &row.type_key,
                   &row.default_value_id, &row.flags, &trailing) != 7)
            return false;
        row.field_ordinal = (uint16_t) field_ordinal;
        if (!xg_global_evidence_add_record_field(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < record_access_count; i++) {
        XgRecordAccessSummary row;
        uint32_t access_kind = 0;
        uint32_t field_ordinal = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "record-access id=%" SCNu32 " module=%" SCNu32 " func=%" SCNu32 " shape=%" SCNu32
                   " kind=%" SCNu32 " span=%" SCNu32 " field_name=%" SCNu32 " result_type=%" SCNu32
                   " field=%" SCNu32 " flags=0x%" SCNx32 " %c",
                   &row.record_access_id, &row.module_id, &row.owner_func_id,
                   &row.receiver_shape_id, &access_kind, &row.source_span_id, &row.field_name_id,
                   &row.result_type_key, &field_ordinal, &row.flags, &trailing) != 10)
            return false;
        row.access_kind = (uint8_t) access_kind;
        row.field_ordinal = (uint16_t) field_ordinal;
        if (!xg_global_evidence_add_record_access(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < record_merge_count; i++) {
        XgRecordMergeSummary row;
        uint32_t base_field_count = 0;
        uint32_t patch_field_count = 0;
        uint32_t result_field_count = 0;
        uint32_t overwrite_count = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "record-merge id=%" SCNu32 " module=%" SCNu32 " func=%" SCNu32 " source=%" SCNu32
                   " span=%" SCNu32 " base_shape=%" SCNu32 " patch_shape=%" SCNu32
                   " result_shape=%" SCNu32 " base_fields=%" SCNu32 " patch_fields=%" SCNu32
                   " result_fields=%" SCNu32 " overwrites=%" SCNu32 " copy_table=%" SCNu32
                   " flags=0x%" SCNx32 " hash=%" SCNx64 " %c",
                   &row.merge_id, &row.module_id, &row.owner_func_id, &row.source_node_id,
                   &row.source_span_id, &row.base_shape_id, &row.patch_shape_id,
                   &row.result_shape_id, &base_field_count, &patch_field_count, &result_field_count,
                   &overwrite_count, &row.copy_table_id, &row.flags, &row.merge_hash,
                   &trailing) != 15)
            return false;
        row.base_field_count = (uint16_t) base_field_count;
        row.patch_field_count = (uint16_t) patch_field_count;
        row.result_field_count = (uint16_t) result_field_count;
        row.overwrite_count = (uint16_t) overwrite_count;
        if (!xg_global_evidence_add_record_merge(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < options_bag_count; i++) {
        XgOptionsBagSummary row;
        uint32_t action = 0;
        uint32_t supplied_count = 0;
        uint32_t default_count = 0;
        uint32_t required_count = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "options-bag id=%" SCNu32 " module=%" SCNu32 " func=%" SCNu32
                   " callsite=%" SCNu32 " param_shape=%" SCNu32 " supplied_shape=%" SCNu32
                   " action=%" SCNu32 " span=%" SCNu32 " supplied_mask=%" SCNu32
                   " default_mask=%" SCNu32 " required_mask=%" SCNu32 " supplied=%" SCNu32
                   " defaults=%" SCNu32 " required=%" SCNu32 " flags=0x%" SCNx32 " %c",
                   &row.options_id, &row.module_id, &row.owner_func_id, &row.callsite_id,
                   &row.param_shape_id, &row.supplied_shape_id, &action, &row.source_span_id,
                   &row.supplied_field_mask_id, &row.default_field_mask_id,
                   &row.required_field_mask_id, &supplied_count, &default_count, &required_count,
                   &row.flags, &trailing) != 15)
            return false;
        row.action = (uint8_t) action;
        row.supplied_count = (uint16_t) supplied_count;
        row.default_count = (uint16_t) default_count;
        row.required_count = (uint16_t) required_count;
        if (!xg_global_evidence_add_options_bag(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < map_shape_count; i++) {
        XgMapShapeSummary row;
        uint32_t container_kind = 0;
        uint32_t source = 0;
        uint32_t entry_count = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "map-shape id=%" SCNu32 " module=%" SCNu32 " func=%" SCNu32 " container=%" SCNu32
                   " source=%" SCNu32 " span=%" SCNu32 " key_type=%" SCNu32 " value_type=%" SCNu32
                   " entries=%" SCNu32 "+%" SCNu32 " literal_count=%" SCNu32 " flags=0x%" SCNx32
                   " hash=%" SCNx64 " %c",
                   &row.shape_id, &row.module_id, &row.owner_func_id, &container_kind, &source,
                   &row.source_span_id, &row.key_type_key, &row.value_type_key, &row.entry_start,
                   &entry_count, &row.literal_count, &row.flags, &row.shape_hash, &trailing) != 13)
            return false;
        row.container_kind = (uint8_t) container_kind;
        row.source = (uint8_t) source;
        row.entry_count = (uint16_t) entry_count;
        if (!xg_global_evidence_add_map_shape(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < map_entry_count; i++) {
        XgMapEntrySummary row;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "map-entry id=%" SCNu32 " shape=%" SCNu32 " ordinal=%" SCNu32
                   " key_const=%" SCNu32 " value_const=%" SCNu32 " key_i64=%" SCNd64
                   " prehash=%" SCNx64 " flags=0x%" SCNx32 " %c",
                   &row.entry_id, &row.shape_id, &row.entry_ordinal, &row.key_const_id,
                   &row.value_const_id, &row.key_i64, &row.prehash, &row.flags, &trailing) != 8)
            return false;
        if (!xg_global_evidence_add_map_entry(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < key_access_count; i++) {
        XgKeyAccessSummary row;
        uint32_t container_kind = 0;
        uint32_t op = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "key-access id=%" SCNu32 " func=%" SCNu32 " span=%" SCNu32 " ordinal=%" SCNu32
                   " container=%" SCNu32 " op=%" SCNu32 " shape=%" SCNu32 " receiver_type=%" SCNu32
                   " key_type=%" SCNu32 " value_type=%" SCNu32 " key_const=%" SCNu32
                   " prehash=%" SCNx64 " flags=0x%" SCNx32 " %c",
                   &row.access_id, &row.owner_func_id, &row.source_span_id, &row.body_ordinal,
                   &container_kind, &op, &row.receiver_shape_id, &row.receiver_type_key,
                   &row.key_type_key, &row.value_type_key, &row.key_const_id, &row.key_prehash,
                   &row.flags, &trailing) != 13)
            return false;
        row.container_kind = (uint8_t) container_kind;
        row.op = (uint8_t) op;
        if (!xg_global_evidence_add_key_access(evidence, &row))
            return false;
    }
    for (uint32_t i = 0; i < hash_eq_count; i++) {
        XgHashEqSummary row;
        uint32_t kind = 0;
        trailing = '\0';
        if (!evidence_cache_next_line(cursor, line, sizeof(line)))
            return false;
        memset(&row, 0, sizeof(row));
        if (sscanf(line,
                   "hash-eq id=%" SCNu32 " type=%" SCNu32 " kind=%" SCNu32 " eq_derive=%" SCNu32
                   " hash_derive=%" SCNu32 " eq_func=%" SCNu32 " hash_func=%" SCNu32
                   " flags=0x%" SCNx32 " %c",
                   &row.hash_eq_id, &row.type_key, &kind, &row.eq_derive_id, &row.hash_derive_id,
                   &row.eq_func_id, &row.hash_func_id, &row.flags, &trailing) != 8)
            return false;
        row.kind = (uint8_t) kind;
        if (!xg_global_evidence_add_hash_eq(evidence, &row))
            return false;
    }
    return true;
}

static bool materialize_payload_global(const char *body, XgGlobalEvidence *evidence) {
    const char *cursor = body;
    char line[1024];
    uint64_t global_hash = 0;
    char trailing = '\0';
    if (!body || !evidence || !evidence_cache_next_line(&cursor, line, sizeof(line)))
        return false;
    if (sscanf(line, "payload-global v1 global_hash=%" SCNx64 " %c", &global_hash, &trailing) != 1)
        return false;
    if (!materialize_payload_semantic_cursor(&cursor, evidence) ||
        !materialize_payload_body_cursor(&cursor, evidence) ||
        !materialize_payload_global_extra(&cursor, evidence) || *cursor != '\0')
        return false;
    return xg_global_evidence_hash(evidence) == global_hash;
}

XR_FUNC bool xg_evidence_cache_payload_materialize(const char *text,
                                                   XgGlobalEvidence *out_evidence) {
    XgEvidenceCachePayloadInfo info;
    XgGlobalEvidence evidence;
    XgBuildKey key;
    XgEvidenceCacheKey materialized_key;
    bool ok = false;
    if (!out_evidence || !xg_evidence_cache_payload_parse(text, &info))
        return false;
    memset(&key, 0, sizeof(key));
    key.module_id = info.key.module_id;
    key.profile = info.key.profile;
    key.source_hash = info.request_key.source_hash;
    key.compiler_semver_hash = info.key.compiler_semver_hash;
    key.profile_hash = info.key.profile_hash;
    key.imported_summary_hash = info.key.imported_summary_hash;
    xg_global_evidence_init(&evidence, key);
    switch ((XgEvidenceCachePhase) info.phase) {
        case XG_EVIDENCE_CACHE_DECLARATIONS: {
            const char *cursor = info.body;
            ok =
                materialize_payload_declarations(&cursor, &evidence, NULL, NULL) && *cursor == '\0';
            break;
        }
        case XG_EVIDENCE_CACHE_SEMANTIC_GRAPH:
            ok = materialize_payload_semantic(info.body, &evidence);
            break;
        case XG_EVIDENCE_CACHE_BODY_SUMMARY:
            ok = materialize_payload_body(info.body, &evidence);
            break;
        case XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE:
            ok = materialize_payload_global(info.body, &evidence);
            break;
        default:
            ok = false;
            break;
    }
    if (ok) {
        materialized_key = xg_global_evidence_cache_key(&evidence, info.phase);
        ok = xg_evidence_cache_key_matches(&materialized_key, &info.key);
    }
    if (!ok) {
        xg_global_evidence_free(&evidence);
        return false;
    }
    *out_evidence = evidence;
    return true;
}

typedef struct XgModuleImportMap {
    XgModuleId from;
    XgModuleId to;
} XgModuleImportMap;

typedef struct XgPackageImportOffsets {
    uint32_t class_field_index;
    uint32_t method_index;
    uint32_t interface_impl_index;
    uint32_t derived_field_index;
    uint32_t derived_method_index;
    uint32_t map_entry_index;
    XgDeclId decl_id;
    XgClassId class_id;
    XgInterfaceId interface_id;
    XgFieldId class_field_id;
    XgMethodId method_id;
    XgInterfaceMethodId interface_method_id;
    XgInterfaceObjectUseId interface_object_use_id;
    XgFuncId func_id;
    XgParamStorageId param_storage_id;
    XgCallsiteId callsite_id;
    XgLinkId link_id;
    XgGenericInstId generic_inst_id;
    XgGenericBodyUseId generic_body_use_id;
    XgGenericStorageId generic_storage_id;
    XgGenericCodeSizeId generic_code_size_id;
    XgSequenceAccessId sequence_access_id;
    XgCapacityOpId capacity_op_id;
    XgBulkOpId bulk_op_id;
    XgEncodingOpId encoding_op_id;
    XgDeriveId derive_id;
    XgDerivedFieldId derived_field_id;
    XgDerivedMethodId derived_method_id;
    XgJsonShapeId json_shape_id;
    XgJsonFieldId json_field_id;
    XgJsonAccessId json_access_id;
    XgJsonCodecId json_codec_id;
    XgRecordShapeId record_shape_id;
    XgRecordFieldId record_field_id;
    XgRecordAccessId record_access_id;
    XgRecordMergeId record_merge_id;
    XgOptionsId options_id;
    XgMapShapeId map_shape_id;
    XgMapEntryId map_entry_id;
    XgKeyAccessId key_access_id;
    XgHashEqId hash_eq_id;
} XgPackageImportOffsets;

static uint32_t max_u32(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

static bool add_u32_checked(uint32_t a, uint32_t b, uint32_t *out) {
    if (!out || UINT32_MAX - a < b)
        return false;
    *out = a + b;
    return true;
}

static bool remap_offset_id(uint32_t value, uint32_t offset, uint32_t *out) {
    if (!out)
        return false;
    if (value == XG_NO_ID) {
        *out = XG_NO_ID;
        return true;
    }
    return add_u32_checked(value, offset, out);
}

static bool remap_index_start(uint32_t start, uint32_t base_count, uint32_t *out) {
    if (!out)
        return false;
    if (start == 0) {
        *out = 0;
        return true;
    }
    return add_u32_checked(start, base_count, out);
}

XR_FUNC bool xg_module_summary_identity_complete(const XgModuleSummary *module) {
    return module && module->module_id != XG_NO_ID && module->name_id != 0 &&
           module->canonical_hash != 0 && module->source_hash != 0 && module->kind != 0;
}

XR_FUNC bool xg_module_summary_identity_matches(const XgModuleSummary *a,
                                                const XgModuleSummary *b) {
    return a && b && a->name_id == b->name_id && a->canonical_hash == b->canonical_hash &&
           a->source_hash == b->source_hash && a->kind == b->kind;
}

static bool validate_package_module_identities(const XgGlobalEvidence *package) {
    if (!package || package->nmodules == 0)
        return false;
    for (uint32_t i = 0; i < package->nmodules; i++) {
        const XgModuleSummary *module = &package->modules[i];
        if (!xg_module_summary_identity_complete(module))
            return false;
        for (uint32_t j = i + 1; j < package->nmodules; j++) {
            const XgModuleSummary *other = &package->modules[j];
            if (module->module_id == other->module_id ||
                xg_module_summary_identity_matches(module, other))
                return false;
        }
    }
    return true;
}

static bool remap_module_id(const XgModuleImportMap *maps, uint32_t map_count, XgModuleId from,
                            XgModuleId *out) {
    if (!out)
        return false;
    if (from == XG_NO_ID) {
        *out = XG_NO_ID;
        return true;
    }
    for (uint32_t i = 0; i < map_count; i++) {
        if (maps[i].from == from) {
            *out = maps[i].to;
            return true;
        }
    }
    return false;
}

static void collect_import_offsets(const XgGlobalEvidence *target,
                                   XgPackageImportOffsets *offsets) {
    memset(offsets, 0, sizeof(*offsets));
    if (!target)
        return;
    offsets->class_field_index = target->nclass_fields;
    offsets->method_index = target->nmethods;
    offsets->interface_impl_index = target->ninterface_impls;
    offsets->derived_field_index = target->nderived_fields;
    offsets->derived_method_index = target->nderived_methods;
    offsets->map_entry_index = target->nmap_entries;
    for (uint32_t i = 0; i < target->ndecls; i++)
        offsets->decl_id = max_u32(offsets->decl_id, target->decls[i].decl_id);
    for (uint32_t i = 0; i < target->nclasses; i++) {
        const XgClassSummary *row = &target->classes[i];
        offsets->class_id = max_u32(offsets->class_id, row->class_id);
        offsets->class_id = max_u32(offsets->class_id, row->parent_class_id);
        offsets->class_id = max_u32(offsets->class_id, row->generic_origin_class_id);
    }
    for (uint32_t i = 0; i < target->nclass_fields; i++) {
        const XgClassFieldSummary *row = &target->class_fields[i];
        offsets->class_field_id = max_u32(offsets->class_field_id, row->field_id);
        offsets->class_id = max_u32(offsets->class_id, row->owner_class_id);
        offsets->class_id = max_u32(offsets->class_id, row->target_class_id);
        offsets->interface_id = max_u32(offsets->interface_id, row->target_interface_id);
    }
    for (uint32_t i = 0; i < target->nmethods; i++) {
        const XgMethodSummary *row = &target->methods[i];
        offsets->method_id = max_u32(offsets->method_id, row->method_id);
        offsets->method_id = max_u32(offsets->method_id, row->override_of);
        offsets->method_id = max_u32(offsets->method_id, row->root_method_id);
        offsets->class_id = max_u32(offsets->class_id, row->owner_class_id);
    }
    for (uint32_t i = 0; i < target->ninterface_impls; i++) {
        offsets->class_id =
            max_u32(offsets->class_id, target->interface_impls[i].implementor_class_id);
        offsets->interface_id =
            max_u32(offsets->interface_id, target->interface_impls[i].interface_id);
    }
    for (uint32_t i = 0; i < target->ninterface_extends; i++) {
        offsets->interface_id =
            max_u32(offsets->interface_id, target->interface_extends[i].child_interface_id);
        offsets->interface_id =
            max_u32(offsets->interface_id, target->interface_extends[i].parent_interface_id);
    }
    for (uint32_t i = 0; i < target->ninterface_methods; i++) {
        offsets->interface_method_id =
            max_u32(offsets->interface_method_id, target->interface_methods[i].interface_method_id);
        offsets->interface_id =
            max_u32(offsets->interface_id, target->interface_methods[i].owner_interface_id);
    }
    for (uint32_t i = 0; i < target->ninterface_object_uses; i++) {
        offsets->interface_object_use_id =
            max_u32(offsets->interface_object_use_id, target->interface_object_uses[i].use_id);
        offsets->interface_id =
            max_u32(offsets->interface_id, target->interface_object_uses[i].interface_id);
        offsets->func_id =
            max_u32(offsets->func_id, target->interface_object_uses[i].owner_func_id);
    }
    for (uint32_t i = 0; i < target->nbodies; i++) {
        const XgBodySummary *row = &target->bodies[i];
        offsets->func_id = max_u32(offsets->func_id, row->func_id);
        offsets->decl_id = max_u32(offsets->decl_id, row->owner_decl_id);
        offsets->class_id = max_u32(offsets->class_id, row->owner_class_id);
        offsets->method_id = max_u32(offsets->method_id, row->owner_method_id);
        offsets->param_storage_id = max_u32(offsets->param_storage_id, row->param_storage_start);
        offsets->callsite_id = max_u32(offsets->callsite_id, row->callsite_start);
    }
    for (uint32_t i = 0; i < target->nparam_storages; i++) {
        const XgParamStorageSummary *row = &target->param_storages[i];
        offsets->param_storage_id = max_u32(offsets->param_storage_id, row->requirement_id);
        offsets->func_id = max_u32(offsets->func_id, row->owner_func_id);
    }
    for (uint32_t i = 0; i < target->ncallsites; i++) {
        const XgCallsiteSummary *row = &target->callsites[i];
        offsets->callsite_id = max_u32(offsets->callsite_id, row->callsite_id);
        offsets->func_id = max_u32(offsets->func_id, row->owner_func_id);
        offsets->func_id = max_u32(offsets->func_id, row->static_target_func_id);
        offsets->class_id = max_u32(offsets->class_id, row->receiver_static_class_id);
        offsets->interface_id = max_u32(offsets->interface_id, row->receiver_static_interface_id);
        offsets->method_id = max_u32(offsets->method_id, row->method_id);
    }
    for (uint32_t i = 0; i < target->nlink_deps; i++) {
        offsets->link_id = max_u32(offsets->link_id, target->link_deps[i].link_id);
        offsets->decl_id = max_u32(offsets->decl_id, target->link_deps[i].decl_id);
    }
    for (uint32_t i = 0; i < target->ngeneric_insts; i++) {
        const XgGenericInstSummary *row = &target->generic_insts[i];
        offsets->generic_inst_id = max_u32(offsets->generic_inst_id, row->generic_inst_id);
        offsets->decl_id = max_u32(offsets->decl_id, row->origin_decl_id);
        offsets->func_id = max_u32(offsets->func_id, row->origin_func_id);
        offsets->method_id = max_u32(offsets->method_id, row->origin_method_id);
        offsets->class_id = max_u32(offsets->class_id, row->origin_class_id);
        offsets->func_id = max_u32(offsets->func_id, row->specialized_func_id);
        offsets->class_id = max_u32(offsets->class_id, row->specialized_class_id);
        offsets->callsite_id = max_u32(offsets->callsite_id, row->root_callsite_id);
        offsets->interface_id = max_u32(offsets->interface_id, row->constraint_interface_id);
    }
    for (uint32_t i = 0; i < target->ngeneric_body_uses; i++) {
        const XgGenericBodyUseSummary *row = &target->generic_body_uses[i];
        offsets->generic_body_use_id = max_u32(offsets->generic_body_use_id, row->use_id);
        offsets->generic_inst_id = max_u32(offsets->generic_inst_id, row->generic_inst_id);
        offsets->func_id = max_u32(offsets->func_id, row->owner_func_id);
        offsets->func_id = max_u32(offsets->func_id, row->origin_body_func_id);
        offsets->func_id = max_u32(offsets->func_id, row->specialized_body_func_id);
        offsets->callsite_id = max_u32(offsets->callsite_id, row->root_callsite_id);
    }
    for (uint32_t i = 0; i < target->ngeneric_storages; i++) {
        offsets->generic_storage_id =
            max_u32(offsets->generic_storage_id, target->generic_storages[i].storage_id);
        offsets->generic_inst_id =
            max_u32(offsets->generic_inst_id, target->generic_storages[i].generic_inst_id);
    }
    for (uint32_t i = 0; i < target->ngeneric_code_sizes; i++) {
        offsets->generic_code_size_id =
            max_u32(offsets->generic_code_size_id, target->generic_code_sizes[i].code_size_id);
        offsets->generic_inst_id =
            max_u32(offsets->generic_inst_id, target->generic_code_sizes[i].generic_inst_id);
        offsets->generic_body_use_id =
            max_u32(offsets->generic_body_use_id, target->generic_code_sizes[i].body_use_id);
    }
    for (uint32_t i = 0; i < target->nsequence_accesses; i++) {
        offsets->sequence_access_id =
            max_u32(offsets->sequence_access_id, target->sequence_accesses[i].access_id);
        offsets->func_id = max_u32(offsets->func_id, target->sequence_accesses[i].owner_func_id);
    }
    for (uint32_t i = 0; i < target->ncapacity_ops; i++) {
        offsets->capacity_op_id = max_u32(offsets->capacity_op_id, target->capacity_ops[i].op_id);
        offsets->func_id = max_u32(offsets->func_id, target->capacity_ops[i].owner_func_id);
    }
    for (uint32_t i = 0; i < target->nbulk_ops; i++) {
        offsets->bulk_op_id = max_u32(offsets->bulk_op_id, target->bulk_ops[i].op_id);
        offsets->func_id = max_u32(offsets->func_id, target->bulk_ops[i].owner_func_id);
    }
    for (uint32_t i = 0; i < target->nencoding_ops; i++) {
        offsets->encoding_op_id = max_u32(offsets->encoding_op_id, target->encoding_ops[i].op_id);
        offsets->func_id = max_u32(offsets->func_id, target->encoding_ops[i].owner_func_id);
    }
    for (uint32_t i = 0; i < target->nderives; i++) {
        offsets->derive_id = max_u32(offsets->derive_id, target->derives[i].derive_id);
        offsets->decl_id = max_u32(offsets->decl_id, target->derives[i].owner_decl_id);
    }
    for (uint32_t i = 0; i < target->nderived_fields; i++) {
        offsets->derived_field_id =
            max_u32(offsets->derived_field_id, target->derived_fields[i].field_id);
        offsets->derive_id = max_u32(offsets->derive_id, target->derived_fields[i].derive_id);
        offsets->class_field_id =
            max_u32(offsets->class_field_id, target->derived_fields[i].source_field_id);
    }
    for (uint32_t i = 0; i < target->nderived_methods; i++) {
        offsets->derived_method_id =
            max_u32(offsets->derived_method_id, target->derived_methods[i].method_id);
        offsets->derive_id = max_u32(offsets->derive_id, target->derived_methods[i].derive_id);
        offsets->func_id =
            max_u32(offsets->func_id, target->derived_methods[i].generated_body_func_id);
    }
    for (uint32_t i = 0; i < target->njson_shapes; i++) {
        offsets->json_shape_id =
            max_u32(offsets->json_shape_id, target->json_shapes[i].json_shape_id);
        offsets->record_shape_id =
            max_u32(offsets->record_shape_id, target->json_shapes[i].record_shape_id);
        offsets->func_id = max_u32(offsets->func_id, target->json_shapes[i].owner_func_id);
    }
    for (uint32_t i = 0; i < target->njson_fields; i++) {
        offsets->json_field_id = max_u32(offsets->json_field_id, target->json_fields[i].field_id);
        offsets->json_shape_id = max_u32(offsets->json_shape_id, target->json_fields[i].shape_id);
    }
    for (uint32_t i = 0; i < target->njson_accesses; i++) {
        offsets->json_access_id =
            max_u32(offsets->json_access_id, target->json_accesses[i].json_access_id);
        offsets->func_id = max_u32(offsets->func_id, target->json_accesses[i].owner_func_id);
        offsets->json_shape_id =
            max_u32(offsets->json_shape_id, target->json_accesses[i].receiver_shape_id);
    }
    for (uint32_t i = 0; i < target->njson_codecs; i++) {
        offsets->json_codec_id = max_u32(offsets->json_codec_id, target->json_codecs[i].codec_id);
        offsets->func_id = max_u32(offsets->func_id, target->json_codecs[i].owner_func_id);
        offsets->json_shape_id =
            max_u32(offsets->json_shape_id, target->json_codecs[i].input_shape_id);
        offsets->json_shape_id =
            max_u32(offsets->json_shape_id, target->json_codecs[i].output_shape_id);
        offsets->record_shape_id =
            max_u32(offsets->record_shape_id, target->json_codecs[i].record_shape_id);
    }
    for (uint32_t i = 0; i < target->nrecord_shapes; i++) {
        offsets->record_shape_id =
            max_u32(offsets->record_shape_id, target->record_shapes[i].record_shape_id);
        offsets->json_shape_id =
            max_u32(offsets->json_shape_id, target->record_shapes[i].json_shape_id);
        offsets->func_id = max_u32(offsets->func_id, target->record_shapes[i].owner_func_id);
    }
    for (uint32_t i = 0; i < target->nrecord_fields; i++) {
        offsets->record_field_id =
            max_u32(offsets->record_field_id, target->record_fields[i].field_id);
        offsets->record_shape_id =
            max_u32(offsets->record_shape_id, target->record_fields[i].shape_id);
    }
    for (uint32_t i = 0; i < target->nrecord_accesses; i++) {
        offsets->record_access_id =
            max_u32(offsets->record_access_id, target->record_accesses[i].record_access_id);
        offsets->func_id = max_u32(offsets->func_id, target->record_accesses[i].owner_func_id);
        offsets->record_shape_id =
            max_u32(offsets->record_shape_id, target->record_accesses[i].receiver_shape_id);
    }
    for (uint32_t i = 0; i < target->nrecord_merges; i++) {
        const XgRecordMergeSummary *row = &target->record_merges[i];
        offsets->record_merge_id = max_u32(offsets->record_merge_id, row->merge_id);
        offsets->func_id = max_u32(offsets->func_id, row->owner_func_id);
        offsets->record_shape_id = max_u32(offsets->record_shape_id, row->base_shape_id);
        offsets->record_shape_id = max_u32(offsets->record_shape_id, row->patch_shape_id);
        offsets->record_shape_id = max_u32(offsets->record_shape_id, row->result_shape_id);
    }
    for (uint32_t i = 0; i < target->noptions_bags; i++) {
        offsets->options_id = max_u32(offsets->options_id, target->options_bags[i].options_id);
        offsets->func_id = max_u32(offsets->func_id, target->options_bags[i].owner_func_id);
        offsets->callsite_id = max_u32(offsets->callsite_id, target->options_bags[i].callsite_id);
        offsets->record_shape_id =
            max_u32(offsets->record_shape_id, target->options_bags[i].param_shape_id);
        offsets->record_shape_id =
            max_u32(offsets->record_shape_id, target->options_bags[i].supplied_shape_id);
    }
    for (uint32_t i = 0; i < target->nmap_shapes; i++) {
        offsets->map_shape_id = max_u32(offsets->map_shape_id, target->map_shapes[i].shape_id);
        offsets->func_id = max_u32(offsets->func_id, target->map_shapes[i].owner_func_id);
    }
    for (uint32_t i = 0; i < target->nmap_entries; i++) {
        offsets->map_entry_id = max_u32(offsets->map_entry_id, target->map_entries[i].entry_id);
        offsets->map_shape_id = max_u32(offsets->map_shape_id, target->map_entries[i].shape_id);
    }
    for (uint32_t i = 0; i < target->nkey_accesses; i++) {
        offsets->key_access_id = max_u32(offsets->key_access_id, target->key_accesses[i].access_id);
        offsets->func_id = max_u32(offsets->func_id, target->key_accesses[i].owner_func_id);
        offsets->map_shape_id =
            max_u32(offsets->map_shape_id, target->key_accesses[i].receiver_shape_id);
    }
    for (uint32_t i = 0; i < target->nhash_eqs; i++) {
        offsets->hash_eq_id = max_u32(offsets->hash_eq_id, target->hash_eqs[i].hash_eq_id);
        offsets->derive_id = max_u32(offsets->derive_id, target->hash_eqs[i].eq_derive_id);
        offsets->derive_id = max_u32(offsets->derive_id, target->hash_eqs[i].hash_derive_id);
        offsets->func_id = max_u32(offsets->func_id, target->hash_eqs[i].eq_func_id);
        offsets->func_id = max_u32(offsets->func_id, target->hash_eqs[i].hash_func_id);
    }
}

static bool reserve_import_capacity(XgGlobalEvidence *target, const XgGlobalEvidence *package) {
    uint32_t capacity;
#define RESERVE_IMPORTED(COUNT, FN)                                                                \
    do {                                                                                           \
        if (!add_u32_checked(target->COUNT, package->COUNT, &capacity) || !FN(target, capacity))   \
            return false;                                                                          \
    } while (0)
    RESERVE_IMPORTED(nmodules, xg_global_evidence_reserve_modules);
    RESERVE_IMPORTED(ndecls, xg_global_evidence_reserve_decls);
    RESERVE_IMPORTED(nclasses, xg_global_evidence_reserve_classes);
    RESERVE_IMPORTED(nclass_fields, xg_global_evidence_reserve_class_fields);
    RESERVE_IMPORTED(nmethods, xg_global_evidence_reserve_methods);
    RESERVE_IMPORTED(ninterface_impls, xg_global_evidence_reserve_interface_impls);
    RESERVE_IMPORTED(ninterface_extends, xg_global_evidence_reserve_interface_extends);
    RESERVE_IMPORTED(ninterface_methods, xg_global_evidence_reserve_interface_methods);
    RESERVE_IMPORTED(ninterface_object_uses, xg_global_evidence_reserve_interface_object_uses);
    RESERVE_IMPORTED(nbodies, xg_global_evidence_reserve_bodies);
    RESERVE_IMPORTED(ncallsites, xg_global_evidence_reserve_callsites);
    RESERVE_IMPORTED(nlink_deps, xg_global_evidence_reserve_link_deps);
    RESERVE_IMPORTED(ngeneric_insts, xg_global_evidence_reserve_generic_insts);
    RESERVE_IMPORTED(ngeneric_body_uses, xg_global_evidence_reserve_generic_body_uses);
    RESERVE_IMPORTED(ngeneric_storages, xg_global_evidence_reserve_generic_storages);
    RESERVE_IMPORTED(ngeneric_code_sizes, xg_global_evidence_reserve_generic_code_sizes);
    RESERVE_IMPORTED(nsequence_accesses, xg_global_evidence_reserve_sequence_accesses);
    RESERVE_IMPORTED(ncapacity_ops, xg_global_evidence_reserve_capacity_ops);
    RESERVE_IMPORTED(nbulk_ops, xg_global_evidence_reserve_bulk_ops);
    RESERVE_IMPORTED(nencoding_ops, xg_global_evidence_reserve_encoding_ops);
    RESERVE_IMPORTED(nderives, xg_global_evidence_reserve_derives);
    RESERVE_IMPORTED(nderived_fields, xg_global_evidence_reserve_derived_fields);
    RESERVE_IMPORTED(nderived_methods, xg_global_evidence_reserve_derived_methods);
    RESERVE_IMPORTED(njson_shapes, xg_global_evidence_reserve_json_shapes);
    RESERVE_IMPORTED(njson_fields, xg_global_evidence_reserve_json_fields);
    RESERVE_IMPORTED(njson_accesses, xg_global_evidence_reserve_json_accesses);
    RESERVE_IMPORTED(njson_codecs, xg_global_evidence_reserve_json_codecs);
    RESERVE_IMPORTED(nrecord_shapes, xg_global_evidence_reserve_record_shapes);
    RESERVE_IMPORTED(nrecord_fields, xg_global_evidence_reserve_record_fields);
    RESERVE_IMPORTED(nrecord_accesses, xg_global_evidence_reserve_record_accesses);
    RESERVE_IMPORTED(nrecord_merges, xg_global_evidence_reserve_record_merges);
    RESERVE_IMPORTED(noptions_bags, xg_global_evidence_reserve_options_bags);
    RESERVE_IMPORTED(nmap_shapes, xg_global_evidence_reserve_map_shapes);
    RESERVE_IMPORTED(nmap_entries, xg_global_evidence_reserve_map_entries);
    RESERVE_IMPORTED(nkey_accesses, xg_global_evidence_reserve_key_accesses);
    RESERVE_IMPORTED(nhash_eqs, xg_global_evidence_reserve_hash_eqs);
#undef RESERVE_IMPORTED
    return true;
}

static bool build_module_import_map(XgGlobalEvidence *target, const XgGlobalEvidence *package,
                                    XgModuleImportMap *maps, uint32_t *out_added) {
    XgModuleId next_module_id = 1;
    uint32_t added = 0;
    if (!target || !package || !maps || !out_added)
        return false;
    for (uint32_t i = 0; i < target->nmodules; i++) {
        if (target->modules[i].module_id == UINT32_MAX)
            return false;
        next_module_id = max_u32(next_module_id, target->modules[i].module_id + 1);
    }
    for (uint32_t i = 0; i < package->nmodules; i++) {
        const XgModuleSummary *source = &package->modules[i];
        XgModuleSummary imported = *source;
        bool found = false;
        for (uint32_t j = 0; j < target->nmodules; j++) {
            if (xg_module_summary_identity_matches(&target->modules[j], source)) {
                maps[i].from = source->module_id;
                maps[i].to = target->modules[j].module_id;
                found = true;
                break;
            }
        }
        if (found)
            continue;
        if (next_module_id == XG_NO_ID)
            return false;
        imported.module_id = next_module_id++;
        if (!xg_global_evidence_add_module(target, &imported))
            return false;
        maps[i].from = source->module_id;
        maps[i].to = imported.module_id;
        added++;
    }
    *out_added = added;
    return true;
}

static uint32_t package_non_module_row_count(const XgGlobalEvidence *package) {
    return package->ndecls + package->nclasses + package->nclass_fields + package->nmethods +
           package->ninterface_impls + package->ninterface_extends + package->ninterface_methods +
           package->ninterface_object_uses + package->nbodies + package->nparam_storages +
           package->ncallsites + package->nlink_deps + package->ngeneric_insts +
           package->ngeneric_body_uses + package->ngeneric_storages + package->ngeneric_code_sizes +
           package->nsequence_accesses + package->ncapacity_ops + package->nbulk_ops +
           package->nencoding_ops + package->nderives + package->nderived_fields +
           package->nderived_methods + package->njson_shapes + package->njson_fields +
           package->njson_accesses + package->njson_codecs + package->nrecord_shapes +
           package->nrecord_fields + package->nrecord_accesses + package->nrecord_merges +
           package->noptions_bags + package->nmap_shapes + package->nmap_entries +
           package->nkey_accesses + package->nhash_eqs;
}

static bool target_has_module_owned_rows(const XgGlobalEvidence *target, XgModuleId module_id) {
    if (!target || module_id == XG_NO_ID)
        return false;
#define HAS_MODULE_ROWS(ROWS, COUNT)                                                               \
    do {                                                                                           \
        for (uint32_t i = 0; i < (target)->COUNT; i++) {                                           \
            if ((target)->ROWS[i].module_id == module_id)                                          \
                return true;                                                                       \
        }                                                                                          \
    } while (0)
    HAS_MODULE_ROWS(decls, ndecls);
    HAS_MODULE_ROWS(classes, nclasses);
    HAS_MODULE_ROWS(class_fields, nclass_fields);
    HAS_MODULE_ROWS(bodies, nbodies);
    HAS_MODULE_ROWS(link_deps, nlink_deps);
    HAS_MODULE_ROWS(generic_insts, ngeneric_insts);
    HAS_MODULE_ROWS(generic_body_uses, ngeneric_body_uses);
    HAS_MODULE_ROWS(generic_storages, ngeneric_storages);
    HAS_MODULE_ROWS(generic_code_sizes, ngeneric_code_sizes);
    HAS_MODULE_ROWS(derives, nderives);
    HAS_MODULE_ROWS(json_shapes, njson_shapes);
    HAS_MODULE_ROWS(json_accesses, njson_accesses);
    HAS_MODULE_ROWS(json_codecs, njson_codecs);
    HAS_MODULE_ROWS(record_shapes, nrecord_shapes);
    HAS_MODULE_ROWS(record_accesses, nrecord_accesses);
    HAS_MODULE_ROWS(record_merges, nrecord_merges);
    HAS_MODULE_ROWS(options_bags, noptions_bags);
    HAS_MODULE_ROWS(map_shapes, nmap_shapes);
#undef HAS_MODULE_ROWS
    return false;
}

static bool package_import_would_duplicate_existing_rows(const XgGlobalEvidence *target,
                                                         const XgGlobalEvidence *package) {
    if (!target || !package || package_non_module_row_count(package) == 0)
        return false;
    for (uint32_t i = 0; i < package->nmodules; i++) {
        const XgModuleSummary *source = &package->modules[i];
        for (uint32_t j = 0; j < target->nmodules; j++) {
            if (xg_module_summary_identity_matches(&target->modules[j], source) &&
                target_has_module_owned_rows(target, target->modules[j].module_id))
                return true;
        }
    }
    return false;
}

static bool target_has_link_dependency_identity(const XgGlobalEvidence *target, uint8_t kind,
                                                const char *name) {
    if (!target || !name || !name[0])
        return false;
    for (uint32_t i = 0; i < target->nlink_deps; i++) {
        const XgLinkDependencySummary *dep = &target->link_deps[i];
        if (dep->kind == kind && strcmp(dep->name, name) == 0)
            return true;
    }
    return false;
}

typedef struct XgImportedPackageHashEntry {
    uint64_t request_hash;
    uint64_t key_hash;
    uint64_t payload_hash;
    uint64_t content_hash;
    uint64_t package_hash;
    uint64_t module_hash;
    uint32_t nmodules;
} XgImportedPackageHashEntry;

static uint64_t imported_package_module_hash(const XgGlobalEvidence *package) {
    uint64_t hash = hash_u32(XR_FNV64_OFFSET_BASIS, package ? package->nmodules : 0);
    if (!package)
        return hash;
    for (uint32_t i = 0; i < package->nmodules; i++)
        hash = hash_module_summary(hash, &package->modules[i]);
    return hash ? hash : 1;
}

static int imported_package_hash_entry_compare(const XgImportedPackageHashEntry *a,
                                               const XgImportedPackageHashEntry *b) {
#define CMP_FIELD(FIELD)                                                                           \
    do {                                                                                           \
        if ((a)->FIELD < (b)->FIELD)                                                               \
            return -1;                                                                             \
        if ((a)->FIELD > (b)->FIELD)                                                               \
            return 1;                                                                              \
    } while (0)
    CMP_FIELD(module_hash);
    CMP_FIELD(request_hash);
    CMP_FIELD(key_hash);
    CMP_FIELD(payload_hash);
    CMP_FIELD(content_hash);
    CMP_FIELD(package_hash);
    CMP_FIELD(nmodules);
#undef CMP_FIELD
    return 0;
}

static void sort_imported_package_hash_entries(XgImportedPackageHashEntry *entries,
                                               uint32_t count) {
    for (uint32_t i = 1; i < count; i++) {
        XgImportedPackageHashEntry item = entries[i];
        uint32_t j = i;
        while (j > 0 && imported_package_hash_entry_compare(&item, &entries[j - 1]) < 0) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = item;
    }
}

static bool imported_package_modules_are_new(const XgGlobalEvidence *package,
                                             const XgModuleSummary *seen_modules,
                                             uint32_t seen_count) {
    if (!package)
        return false;
    for (uint32_t i = 0; i < package->nmodules; i++) {
        for (uint32_t j = 0; j < seen_count; j++) {
            if (xg_module_summary_identity_matches(&package->modules[i], &seen_modules[j]))
                return false;
        }
    }
    return true;
}

XR_FUNC bool xg_imported_summary_hash_from_package_payloads(uint64_t seed,
                                                            const char *const *payloads,
                                                            uint32_t payload_count,
                                                            uint64_t *out_hash) {
    XgImportedPackageHashEntry *entries = NULL;
    XgModuleSummary *seen_modules = NULL;
    uint32_t entries_cap = 0;
    uint32_t seen_count = 0;
    uint32_t seen_cap = 0;
    uint64_t hash = hash_u32(seed, payload_count);
    bool ok = false;
    if (!out_hash || (payload_count > 0 && !payloads))
        return false;
    for (uint32_t i = 0; i < payload_count; i++) {
        XgEvidenceCachePayloadInfo info;
        XgGlobalEvidence package;
        uint64_t package_hash;
        if (!payloads[i] || !xg_evidence_cache_payload_parse(payloads[i], &info) ||
            info.phase != XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE)
            goto done;
        memset(&package, 0, sizeof(package));
        if (!xg_evidence_cache_payload_materialize(payloads[i], &package)) {
            xg_global_evidence_free(&package);
            goto done;
        }
        if (!validate_package_module_identities(&package)) {
            xg_global_evidence_free(&package);
            goto done;
        }
        if (!imported_package_modules_are_new(&package, seen_modules, seen_count) ||
            !reserve_array((void **) &seen_modules, &seen_cap, seen_count + package.nmodules,
                           sizeof(*seen_modules)) ||
            !reserve_array((void **) &entries, &entries_cap, i + 1, sizeof(*entries))) {
            xg_global_evidence_free(&package);
            goto done;
        }
        memcpy(&seen_modules[seen_count], package.modules,
               (size_t) package.nmodules * sizeof(*seen_modules));
        seen_count += package.nmodules;
        package_hash = xg_global_evidence_hash(&package);
        entries[i].request_hash = info.request_hash;
        entries[i].key_hash = info.key_hash;
        entries[i].payload_hash = info.payload_hash;
        entries[i].content_hash = info.key.content_hash;
        entries[i].package_hash = package_hash;
        entries[i].module_hash = imported_package_module_hash(&package);
        entries[i].nmodules = package.nmodules;
        xg_global_evidence_free(&package);
    }
    sort_imported_package_hash_entries(entries, payload_count);
    for (uint32_t i = 0; i < payload_count; i++) {
        hash = hash_u64(hash, entries[i].module_hash);
        hash = hash_u64(hash, entries[i].request_hash);
        hash = hash_u64(hash, entries[i].key_hash);
        hash = hash_u64(hash, entries[i].payload_hash);
        hash = hash_u64(hash, entries[i].content_hash);
        hash = hash_u64(hash, entries[i].package_hash);
        hash = hash_u32(hash, entries[i].nmodules);
    }
    *out_hash = hash ? hash : 1;
    ok = true;

done:
    xr_free(seen_modules);
    xr_free(entries);
    return ok;
}

XR_FUNC bool xg_global_evidence_import_package_payload(XgGlobalEvidence *target,
                                                       const char *payload,
                                                       XgEvidencePackageImportReport *out_report) {
    XgEvidenceCachePayloadInfo info;
    XgGlobalEvidence package;
    XgPackageImportOffsets offsets;
    XgModuleImportMap *module_maps = NULL;
    XgEvidencePackageImportReport report;
    uint32_t skipped_link_deps = 0;
    bool ok = false;
    if (out_report)
        memset(out_report, 0, sizeof(*out_report));
    if (!target || !payload || !xg_evidence_cache_payload_parse(payload, &info) ||
        info.phase != XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE)
        return false;
    memset(&package, 0, sizeof(package));
    memset(&report, 0, sizeof(report));
    if (!xg_evidence_cache_payload_materialize(payload, &package))
        return false;
    report.package_hash = xg_global_evidence_hash(&package);
    if (!validate_package_module_identities(&package) ||
        package_import_would_duplicate_existing_rows(target, &package) ||
        !reserve_import_capacity(target, &package))
        goto done;
    module_maps = (XgModuleImportMap *) xr_calloc(package.nmodules, sizeof(*module_maps));
    if (!module_maps)
        goto done;
    collect_import_offsets(target, &offsets);
    if (!build_module_import_map(target, &package, module_maps, &report.modules_added))
        goto done;
    report.modules_remapped = package.nmodules;

#define REMAP_MODULE(VALUE)                                                                        \
    do {                                                                                           \
        XgModuleId remapped_module = 0;                                                            \
        if (!remap_module_id(module_maps, package.nmodules, (VALUE), &remapped_module))            \
            goto done;                                                                             \
        (VALUE) = remapped_module;                                                                 \
    } while (0)
#define REMAP_ID(VALUE, OFFSET)                                                                    \
    do {                                                                                           \
        uint32_t remapped_id = 0;                                                                  \
        if (!remap_offset_id((VALUE), (OFFSET), &remapped_id))                                     \
            goto done;                                                                             \
        (VALUE) = remapped_id;                                                                     \
    } while (0)
#define REMAP_START(VALUE, BASE)                                                                   \
    do {                                                                                           \
        uint32_t remapped_start = 0;                                                               \
        if (!remap_index_start((VALUE), (BASE), &remapped_start))                                  \
            goto done;                                                                             \
        (VALUE) = remapped_start;                                                                  \
    } while (0)

    for (uint32_t i = 0; i < package.ndecls; i++) {
        XgDeclSummary row = package.decls[i];
        REMAP_MODULE(row.module_id);
        REMAP_ID(row.decl_id, offsets.decl_id);
        if (!xg_global_evidence_add_decl(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.nclasses; i++) {
        XgClassSummary row = package.classes[i];
        REMAP_MODULE(row.module_id);
        REMAP_ID(row.decl_id, offsets.decl_id);
        REMAP_ID(row.class_id, offsets.class_id);
        REMAP_ID(row.parent_class_id, offsets.class_id);
        REMAP_START(row.field_start, offsets.class_field_index);
        REMAP_START(row.method_start, offsets.method_index);
        REMAP_START(row.interface_start, offsets.interface_impl_index);
        REMAP_ID(row.generic_origin_class_id, offsets.class_id);
        if (!xg_global_evidence_add_class(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.nclass_fields; i++) {
        XgClassFieldSummary row = package.class_fields[i];
        REMAP_ID(row.field_id, offsets.class_field_id);
        REMAP_MODULE(row.module_id);
        REMAP_ID(row.owner_class_id, offsets.class_id);
        REMAP_ID(row.target_class_id, offsets.class_id);
        REMAP_ID(row.target_interface_id, offsets.interface_id);
        if (!xg_global_evidence_add_class_field(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.nmethods; i++) {
        XgMethodSummary row = package.methods[i];
        REMAP_ID(row.method_id, offsets.method_id);
        REMAP_ID(row.owner_class_id, offsets.class_id);
        REMAP_ID(row.override_of, offsets.method_id);
        REMAP_ID(row.root_method_id, offsets.method_id);
        if (!xg_global_evidence_add_method(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.ninterface_impls; i++) {
        XgInterfaceImplSummary row = package.interface_impls[i];
        REMAP_ID(row.implementor_class_id, offsets.class_id);
        REMAP_ID(row.interface_id, offsets.interface_id);
        if (!xg_global_evidence_add_interface_impl(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.ninterface_extends; i++) {
        XgInterfaceExtendsSummary row = package.interface_extends[i];
        REMAP_ID(row.child_interface_id, offsets.interface_id);
        REMAP_ID(row.parent_interface_id, offsets.interface_id);
        if (!xg_global_evidence_add_interface_extends(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.ninterface_methods; i++) {
        XgInterfaceMethodSummary row = package.interface_methods[i];
        REMAP_ID(row.interface_method_id, offsets.interface_method_id);
        REMAP_ID(row.owner_interface_id, offsets.interface_id);
        if (!xg_global_evidence_add_interface_method(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.ninterface_object_uses; i++) {
        XgInterfaceObjectUseSummary row = package.interface_object_uses[i];
        REMAP_ID(row.use_id, offsets.interface_object_use_id);
        REMAP_ID(row.interface_id, offsets.interface_id);
        REMAP_ID(row.owner_func_id, offsets.func_id);
        if (!xg_global_evidence_add_interface_object_use(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.nbodies; i++) {
        XgBodySummary row = package.bodies[i];
        REMAP_ID(row.func_id, offsets.func_id);
        REMAP_MODULE(row.module_id);
        REMAP_ID(row.owner_decl_id, offsets.decl_id);
        REMAP_ID(row.owner_class_id, offsets.class_id);
        REMAP_ID(row.owner_method_id, offsets.method_id);
        REMAP_ID(row.param_storage_start, offsets.param_storage_id);
        REMAP_ID(row.callsite_start, offsets.callsite_id);
        if (!xg_global_evidence_add_body(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.nparam_storages; i++) {
        XgParamStorageSummary row = package.param_storages[i];
        REMAP_ID(row.requirement_id, offsets.param_storage_id);
        REMAP_ID(row.owner_func_id, offsets.func_id);
        if (!xg_global_evidence_add_param_storage(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.ncallsites; i++) {
        XgCallsiteSummary row = package.callsites[i];
        REMAP_ID(row.callsite_id, offsets.callsite_id);
        REMAP_ID(row.owner_func_id, offsets.func_id);
        REMAP_ID(row.static_target_func_id, offsets.func_id);
        REMAP_ID(row.receiver_static_class_id, offsets.class_id);
        REMAP_ID(row.receiver_static_interface_id, offsets.interface_id);
        REMAP_ID(row.method_id, offsets.method_id);
        if (!xg_global_evidence_add_callsite(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.nlink_deps; i++) {
        XgLinkDependencySummary row = package.link_deps[i];
        if (target_has_link_dependency_identity(target, row.kind, row.name)) {
            skipped_link_deps++;
            continue;
        }
        REMAP_ID(row.link_id, offsets.link_id);
        REMAP_MODULE(row.module_id);
        REMAP_ID(row.decl_id, offsets.decl_id);
        if (!xg_global_evidence_add_link_dependency(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.ngeneric_insts; i++) {
        XgGenericInstSummary row = package.generic_insts[i];
        REMAP_ID(row.generic_inst_id, offsets.generic_inst_id);
        REMAP_MODULE(row.module_id);
        REMAP_ID(row.origin_decl_id, offsets.decl_id);
        REMAP_ID(row.origin_func_id, offsets.func_id);
        REMAP_ID(row.origin_method_id, offsets.method_id);
        REMAP_ID(row.origin_class_id, offsets.class_id);
        REMAP_ID(row.specialized_func_id, offsets.func_id);
        REMAP_ID(row.specialized_class_id, offsets.class_id);
        REMAP_ID(row.root_callsite_id, offsets.callsite_id);
        REMAP_ID(row.constraint_interface_id, offsets.interface_id);
        if (!xg_global_evidence_add_generic_inst(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.ngeneric_body_uses; i++) {
        XgGenericBodyUseSummary row = package.generic_body_uses[i];
        REMAP_ID(row.use_id, offsets.generic_body_use_id);
        REMAP_ID(row.generic_inst_id, offsets.generic_inst_id);
        REMAP_MODULE(row.module_id);
        REMAP_ID(row.owner_func_id, offsets.func_id);
        REMAP_ID(row.origin_body_func_id, offsets.func_id);
        REMAP_ID(row.specialized_body_func_id, offsets.func_id);
        REMAP_ID(row.root_callsite_id, offsets.callsite_id);
        if (!xg_global_evidence_add_generic_body_use(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.ngeneric_storages; i++) {
        XgGenericStorageSummary row = package.generic_storages[i];
        REMAP_ID(row.storage_id, offsets.generic_storage_id);
        REMAP_ID(row.generic_inst_id, offsets.generic_inst_id);
        REMAP_MODULE(row.module_id);
        if (!xg_global_evidence_add_generic_storage(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.ngeneric_code_sizes; i++) {
        XgGenericCodeSizeSummary row = package.generic_code_sizes[i];
        REMAP_ID(row.code_size_id, offsets.generic_code_size_id);
        REMAP_ID(row.generic_inst_id, offsets.generic_inst_id);
        REMAP_MODULE(row.module_id);
        REMAP_ID(row.body_use_id, offsets.generic_body_use_id);
        if (!xg_global_evidence_add_generic_code_size(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.nsequence_accesses; i++) {
        XgSequenceAccessSummary row = package.sequence_accesses[i];
        REMAP_ID(row.access_id, offsets.sequence_access_id);
        REMAP_ID(row.owner_func_id, offsets.func_id);
        if (!xg_global_evidence_add_sequence_access(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.ncapacity_ops; i++) {
        XgCapacityOpSummary row = package.capacity_ops[i];
        REMAP_ID(row.op_id, offsets.capacity_op_id);
        REMAP_ID(row.owner_func_id, offsets.func_id);
        if (!xg_global_evidence_add_capacity_op(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.nbulk_ops; i++) {
        XgBulkOpSummary row = package.bulk_ops[i];
        REMAP_ID(row.op_id, offsets.bulk_op_id);
        REMAP_ID(row.owner_func_id, offsets.func_id);
        if (!xg_global_evidence_add_bulk_op(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.nencoding_ops; i++) {
        XgEncodingOpSummary row = package.encoding_ops[i];
        REMAP_ID(row.op_id, offsets.encoding_op_id);
        REMAP_ID(row.owner_func_id, offsets.func_id);
        if (!xg_global_evidence_add_encoding_op(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.nderives; i++) {
        XgDeriveSummary row = package.derives[i];
        REMAP_ID(row.derive_id, offsets.derive_id);
        REMAP_MODULE(row.module_id);
        REMAP_ID(row.owner_decl_id, offsets.decl_id);
        REMAP_START(row.field_start, offsets.derived_field_index);
        REMAP_START(row.method_start, offsets.derived_method_index);
        if (!xg_global_evidence_add_derive(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.nderived_fields; i++) {
        XgDerivedFieldSummary row = package.derived_fields[i];
        REMAP_ID(row.field_id, offsets.derived_field_id);
        REMAP_ID(row.derive_id, offsets.derive_id);
        REMAP_ID(row.source_field_id, offsets.class_field_id);
        if (!xg_global_evidence_add_derived_field(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.nderived_methods; i++) {
        XgDerivedMethodSummary row = package.derived_methods[i];
        REMAP_ID(row.method_id, offsets.derived_method_id);
        REMAP_ID(row.derive_id, offsets.derive_id);
        REMAP_ID(row.generated_body_func_id, offsets.func_id);
        if (!xg_global_evidence_add_derived_method(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.njson_shapes; i++) {
        XgJsonShapeSummary row = package.json_shapes[i];
        REMAP_ID(row.json_shape_id, offsets.json_shape_id);
        REMAP_ID(row.record_shape_id, offsets.record_shape_id);
        REMAP_MODULE(row.module_id);
        REMAP_ID(row.owner_func_id, offsets.func_id);
        if (!xg_global_evidence_add_json_shape(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.njson_fields; i++) {
        XgJsonFieldSummary row = package.json_fields[i];
        REMAP_ID(row.field_id, offsets.json_field_id);
        REMAP_ID(row.shape_id, offsets.json_shape_id);
        if (!xg_global_evidence_add_json_field(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.njson_accesses; i++) {
        XgJsonAccessSummary row = package.json_accesses[i];
        REMAP_ID(row.json_access_id, offsets.json_access_id);
        REMAP_MODULE(row.module_id);
        REMAP_ID(row.owner_func_id, offsets.func_id);
        REMAP_ID(row.receiver_shape_id, offsets.json_shape_id);
        if (!xg_global_evidence_add_json_access(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.njson_codecs; i++) {
        XgJsonCodecSummary row = package.json_codecs[i];
        REMAP_ID(row.codec_id, offsets.json_codec_id);
        REMAP_MODULE(row.module_id);
        REMAP_ID(row.owner_func_id, offsets.func_id);
        REMAP_ID(row.input_shape_id, offsets.json_shape_id);
        REMAP_ID(row.output_shape_id, offsets.json_shape_id);
        REMAP_ID(row.record_shape_id, offsets.record_shape_id);
        if (!xg_global_evidence_add_json_codec(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.nrecord_shapes; i++) {
        XgRecordShapeSummary row = package.record_shapes[i];
        REMAP_ID(row.record_shape_id, offsets.record_shape_id);
        REMAP_ID(row.json_shape_id, offsets.json_shape_id);
        REMAP_MODULE(row.module_id);
        REMAP_ID(row.owner_func_id, offsets.func_id);
        if (!xg_global_evidence_add_record_shape(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.nrecord_fields; i++) {
        XgRecordFieldSummary row = package.record_fields[i];
        REMAP_ID(row.field_id, offsets.record_field_id);
        REMAP_ID(row.shape_id, offsets.record_shape_id);
        if (!xg_global_evidence_add_record_field(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.nrecord_accesses; i++) {
        XgRecordAccessSummary row = package.record_accesses[i];
        REMAP_ID(row.record_access_id, offsets.record_access_id);
        REMAP_MODULE(row.module_id);
        REMAP_ID(row.owner_func_id, offsets.func_id);
        REMAP_ID(row.receiver_shape_id, offsets.record_shape_id);
        if (!xg_global_evidence_add_record_access(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.nrecord_merges; i++) {
        XgRecordMergeSummary row = package.record_merges[i];
        REMAP_ID(row.merge_id, offsets.record_merge_id);
        REMAP_MODULE(row.module_id);
        REMAP_ID(row.owner_func_id, offsets.func_id);
        REMAP_ID(row.base_shape_id, offsets.record_shape_id);
        REMAP_ID(row.patch_shape_id, offsets.record_shape_id);
        REMAP_ID(row.result_shape_id, offsets.record_shape_id);
        if (!xg_global_evidence_add_record_merge(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.noptions_bags; i++) {
        XgOptionsBagSummary row = package.options_bags[i];
        REMAP_ID(row.options_id, offsets.options_id);
        REMAP_MODULE(row.module_id);
        REMAP_ID(row.owner_func_id, offsets.func_id);
        REMAP_ID(row.callsite_id, offsets.callsite_id);
        REMAP_ID(row.param_shape_id, offsets.record_shape_id);
        REMAP_ID(row.supplied_shape_id, offsets.record_shape_id);
        if (!xg_global_evidence_add_options_bag(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.nmap_shapes; i++) {
        XgMapShapeSummary row = package.map_shapes[i];
        REMAP_ID(row.shape_id, offsets.map_shape_id);
        REMAP_MODULE(row.module_id);
        REMAP_ID(row.owner_func_id, offsets.func_id);
        REMAP_START(row.entry_start, offsets.map_entry_index);
        if (!xg_global_evidence_add_map_shape(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.nmap_entries; i++) {
        XgMapEntrySummary row = package.map_entries[i];
        REMAP_ID(row.entry_id, offsets.map_entry_id);
        REMAP_ID(row.shape_id, offsets.map_shape_id);
        if (!xg_global_evidence_add_map_entry(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.nkey_accesses; i++) {
        XgKeyAccessSummary row = package.key_accesses[i];
        REMAP_ID(row.access_id, offsets.key_access_id);
        REMAP_ID(row.owner_func_id, offsets.func_id);
        REMAP_ID(row.receiver_shape_id, offsets.map_shape_id);
        if (!xg_global_evidence_add_key_access(target, &row))
            goto done;
    }
    for (uint32_t i = 0; i < package.nhash_eqs; i++) {
        XgHashEqSummary row = package.hash_eqs[i];
        REMAP_ID(row.hash_eq_id, offsets.hash_eq_id);
        REMAP_ID(row.eq_derive_id, offsets.derive_id);
        REMAP_ID(row.hash_derive_id, offsets.derive_id);
        REMAP_ID(row.eq_func_id, offsets.func_id);
        REMAP_ID(row.hash_func_id, offsets.func_id);
        if (!xg_global_evidence_add_hash_eq(target, &row))
            goto done;
    }

#undef REMAP_START
#undef REMAP_ID
#undef REMAP_MODULE

    report.rows_imported = package_non_module_row_count(&package) - skipped_link_deps;
    report.payloads_imported = 1;
    if (out_report)
        *out_report = report;
    ok = true;

done:
    xr_free(module_maps);
    xg_global_evidence_free(&package);
    return ok;
}

XR_FUNC bool
xg_global_evidence_import_package_payload_set(XgGlobalEvidence *target, const char *const *payloads,
                                              uint32_t payload_count,
                                              XgEvidencePackageImportReport *out_report) {
    XgGlobalEvidence scratch;
    XgEvidencePackageImportReport total;
    uint64_t validated_hash = 0;
    bool ok = false;
    if (out_report)
        memset(out_report, 0, sizeof(*out_report));
    if (!target || (payload_count > 0 && !payloads))
        return false;
    memset(&scratch, 0, sizeof(scratch));
    memset(&total, 0, sizeof(total));
    if (payload_count == 0) {
        if (out_report)
            *out_report = total;
        return true;
    }
    if (!xg_imported_summary_hash_from_package_payloads(0, payloads, payload_count,
                                                        &validated_hash))
        return false;
    if (!xg_global_evidence_clone(&scratch, target))
        return false;
    total.package_hash = hash_u32(XR_FNV64_OFFSET_BASIS, payload_count);
    for (uint32_t i = 0; i < payload_count; i++) {
        XgEvidencePackageImportReport item;
        if (!xg_global_evidence_import_package_payload(&scratch, payloads[i], &item))
            goto done;
        total.package_hash = hash_u64(total.package_hash, item.package_hash);
        total.modules_remapped += item.modules_remapped;
        total.modules_added += item.modules_added;
        total.rows_imported += item.rows_imported;
        total.payloads_imported += item.payloads_imported;
    }
    total.package_hash = hash_u64(total.package_hash, validated_hash);
    {
        XgGlobalEvidence old = *target;
        *target = scratch;
        memset(&scratch, 0, sizeof(scratch));
        xg_global_evidence_free(&old);
    }
    if (out_report)
        *out_report = total;
    ok = true;

done:
    xg_global_evidence_free(&scratch);
    return ok;
}

typedef const char *(*XgBitNameFn)(uint32_t bit);

static void dump_named_bitset(FILE *out, uint32_t bits, const uint32_t *catalog,
                              uint32_t catalog_count, XgBitNameFn name_fn) {
    bool first = true;
    uint32_t known = 0;
    fprintf(out, "[");
    for (uint32_t i = 0; i < catalog_count; i++) {
        uint32_t bit = catalog[i];
        known |= bit;
        if ((bits & bit) == 0)
            continue;
        fprintf(out, "%s%s", first ? "" : ",", name_fn ? name_fn(bit) : "unknown");
        first = false;
    }
    if ((bits & ~known) != 0) {
        fprintf(out, "%sunknown:0x%x", first ? "" : ",", bits & ~known);
        first = false;
    }
    if (first)
        fprintf(out, "-");
    fprintf(out, "]");
}

static void dump_cache_key(FILE *out, const XgGlobalEvidence *evidence, uint32_t phase) {
    XgEvidenceCacheKey key = xg_global_evidence_cache_key(evidence, phase);
    fprintf(out,
            "cache-key phase=%s schema=%u module=%u profile=%s compiler=%016" PRIx64
            " profile_hash=%016" PRIx64 " imports=%016" PRIx64 " content=%016" PRIx64
            " key=%016" PRIx64 "\n",
            xg_evidence_cache_phase_name(phase), key.schema_version, key.module_id,
            xg_build_profile_name(key.profile), key.compiler_semver_hash, key.profile_hash,
            key.imported_summary_hash, key.content_hash, xg_evidence_cache_key_hash(&key));
}

static void dump_cache_manifest(FILE *out, const XgGlobalEvidence *evidence) {
    char manifest_text[1400];
    XgEvidenceCacheManifest manifest = xg_global_evidence_cache_manifest(evidence);
    if (!xg_evidence_cache_manifest_format(&manifest, manifest_text, sizeof(manifest_text)))
        return;
    fprintf(out, "%s", manifest_text);
}

XR_FUNC char *xg_global_evidence_dump(const XgGlobalEvidence *evidence) {
    char *buf = NULL;
    size_t bufsz = 0;
    FILE *out;
    uint32_t capability_count = 0;
    const uint32_t *capabilities = xg_capability_catalog(&capability_count);
    uint32_t effect_count = 0;
    const uint32_t *effects = xg_body_effect_catalog(&effect_count);
    uint32_t escape_count = 0;
    const uint32_t *escapes = xg_body_escape_catalog(&escape_count);
    uint32_t interface_use_count = 0;
    const uint32_t *interface_uses = xg_interface_object_use_catalog(&interface_use_count);
    uint32_t metadata_count = 0;
    const uint32_t *metadata = xg_metadata_catalog(&metadata_count);
    uint32_t static_data_count = 0;
    const uint32_t *static_data = xg_static_data_catalog(&static_data_count);

    if (!evidence)
        return NULL;

    out = xr_open_memstream(&buf, &bufsz);
    if (!out)
        return NULL;

    fprintf(out, "xglobal-evidence v1 profile=%s hash=%016" PRIx64 "\n",
            xg_build_profile_name(evidence->key.profile), xg_global_evidence_hash(evidence));
    fprintf(out,
            "build-key module=%u source=%016" PRIx64 " compiler=%016" PRIx64 " profile=%016" PRIx64
            " imports=%016" PRIx64 "\n",
            evidence->key.module_id, evidence->key.source_hash, evidence->key.compiler_semver_hash,
            evidence->key.profile_hash, evidence->key.imported_summary_hash);
    dump_cache_key(out, evidence, XG_EVIDENCE_CACHE_DECLARATIONS);
    dump_cache_key(out, evidence, XG_EVIDENCE_CACHE_SEMANTIC_GRAPH);
    dump_cache_key(out, evidence, XG_EVIDENCE_CACHE_BODY_SUMMARY);
    dump_cache_key(out, evidence, XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE);
    dump_cache_manifest(out, evidence);
    fprintf(out,
            "counts modules=%u decls=%u classes=%u class_fields=%u methods=%u "
            "interface_impls=%u interface_extends=%u "
            "interface_methods=%u interface_object_uses=%u bodies=%u param_storages=%u "
            "callsites=%u link_deps=%u generic_insts=%u "
            "generic_body_uses=%u generic_storages=%u generic_code_sizes=%u "
            "sequence_accesses=%u capacity_ops=%u bulk_ops=%u encoding_ops=%u derives=%u "
            "derived_fields=%u derived_methods=%u json_shapes=%u "
            "json_fields=%u json_accesses=%u json_codecs=%u record_shapes=%u "
            "record_fields=%u record_accesses=%u record_merges=%u options=%u map_shapes=%u "
            "map_entries=%u key_accesses=%u "
            "hash_eqs=%u\n",
            evidence->nmodules, evidence->ndecls, evidence->nclasses, evidence->nclass_fields,
            evidence->nmethods, evidence->ninterface_impls, evidence->ninterface_extends,
            evidence->ninterface_methods, evidence->ninterface_object_uses, evidence->nbodies,
            evidence->nparam_storages, evidence->ncallsites, evidence->nlink_deps,
            evidence->ngeneric_insts, evidence->ngeneric_body_uses, evidence->ngeneric_storages,
            evidence->ngeneric_code_sizes, evidence->nsequence_accesses, evidence->ncapacity_ops,
            evidence->nbulk_ops, evidence->nencoding_ops, evidence->nderives,
            evidence->nderived_fields, evidence->nderived_methods, evidence->njson_shapes,
            evidence->njson_fields, evidence->njson_accesses, evidence->njson_codecs,
            evidence->nrecord_shapes, evidence->nrecord_fields, evidence->nrecord_accesses,
            evidence->nrecord_merges, evidence->noptions_bags, evidence->nmap_shapes,
            evidence->nmap_entries, evidence->nkey_accesses, evidence->nhash_eqs);

    for (uint32_t i = 0; i < evidence->nmodules; i++) {
        const XgModuleSummary *m = &evidence->modules[i];
        fprintf(out,
                "module %u id=%u name=%u canonical=%016" PRIx64 " source=%016" PRIx64
                " kind=%u flags=0x%x\n",
                i, m->module_id, m->name_id, m->canonical_hash, m->source_hash, (unsigned) m->kind,
                m->flags);
    }
    for (uint32_t i = 0; i < evidence->ndecls; i++) {
        const XgDeclSummary *d = &evidence->decls[i];
        fprintf(out,
                "decl %u id=%u module=%u node=%u kind=%s flags=0x%x name=%u type=%u sig=%u "
                "span=%u derive=0x%x storage_flags=0x%x owner=%u mutability=%u address=%u "
                "materialize=%u\n",
                i, d->decl_id, d->module_id, d->source_node_id, xg_decl_kind_name(d->kind),
                d->flags, d->name_id, d->type_key, d->signature_key, d->source_span_id,
                d->derive_flags, d->storage_flags, (unsigned) d->storage_owner,
                (unsigned) d->storage_mutability, (unsigned) d->address_identity,
                (unsigned) d->materialization_kind);
    }
    for (uint32_t i = 0; i < evidence->nclasses; i++) {
        const XgClassSummary *c = &evidence->classes[i];
        fprintf(out,
                "class %u id=%u module=%u decl=%u name=%u parent=%u kind=%s flags=0x%x "
                "fields=%u+%u methods=%u+%u interfaces=%u+%u generic_origin=%u "
                "generic_name=%u generic_type=%u generic_args=%u+%u\n",
                i, c->class_id, c->module_id, c->decl_id, c->name_id, c->parent_class_id,
                xg_decl_kind_name(c->decl_kind ? c->decl_kind : XG_DECL_CLASS), c->flags,
                c->field_start, c->field_count, c->method_start, c->method_count,
                c->interface_start, c->interface_count, c->generic_origin_class_id,
                c->generic_origin_name_id, c->generic_type_key, c->generic_type_arg_key_start,
                (unsigned) c->generic_type_arg_count);
    }
    for (uint32_t i = 0; i < evidence->nclass_fields; i++) {
        const XgClassFieldSummary *f = &evidence->class_fields[i];
        fprintf(out,
                "class-field %u id=%u module=%u node=%u owner=%u name=%u type=%u "
                "target_name=%u target_class=%u target_interface=%u element=%u key=%u value=%u "
                "fixed=%u ordinal=%u slot=%u semantic=%u width=%u flags=0x%x\n",
                i, f->field_id, f->module_id, f->source_node_id, f->owner_class_id, f->name_id,
                f->type_key, f->target_name_id, f->target_class_id, f->target_interface_id,
                f->element_type_key, f->key_type_key, f->value_type_key, f->fixed_length,
                f->decl_ordinal, f->instance_slot, (unsigned) f->semantic_kind,
                (unsigned) f->native_width, f->flags);
    }
    for (uint32_t i = 0; i < evidence->nmethods; i++) {
        const XgMethodSummary *m = &evidence->methods[i];
        fprintf(out,
                "method %u id=%u owner=%u node=%u name=%u sig=%u override_of=%u root=%u depth=%u "
                "defaults=%u flags=0x%x\n",
                i, m->method_id, m->owner_class_id, m->source_node_id, m->name_id, m->signature_key,
                m->override_of, m->root_method_id, m->override_depth, m->default_arg_contract_id,
                m->flags);
    }
    for (uint32_t i = 0; i < evidence->ninterface_impls; i++) {
        const XgInterfaceImplSummary *impl = &evidence->interface_impls[i];
        fprintf(out, "interface-impl %u class=%u interface=%u name=%u type=%u span=%u flags=0x%x\n",
                i, impl->implementor_class_id, impl->interface_id, impl->name_id, impl->type_key,
                impl->source_span_id, impl->flags);
    }
    for (uint32_t i = 0; i < evidence->ninterface_extends; i++) {
        const XgInterfaceExtendsSummary *edge = &evidence->interface_extends[i];
        fprintf(out, "interface-extends %u child=%u parent=%u name=%u type=%u span=%u flags=0x%x\n",
                i, edge->child_interface_id, edge->parent_interface_id, edge->name_id,
                edge->type_key, edge->source_span_id, edge->flags);
    }
    for (uint32_t i = 0; i < evidence->ninterface_methods; i++) {
        const XgInterfaceMethodSummary *m = &evidence->interface_methods[i];
        fprintf(out,
                "interface-method %u id=%u owner=%u name=%u sig=%u ordinal=%u span=%u "
                "flags=0x%x\n",
                i, m->interface_method_id, m->owner_interface_id, m->name_id, m->signature_key,
                m->ordinal, m->source_span_id, m->flags);
    }
    for (uint32_t i = 0; i < evidence->ninterface_object_uses; i++) {
        const XgInterfaceObjectUseSummary *u = &evidence->interface_object_uses[i];
        fprintf(out,
                "interface-object-use %u id=%u interface=%u owner=%u span=%u ordinal=%u type=%u "
                "reason=0x%x",
                i, u->use_id, u->interface_id, u->owner_func_id, u->source_span_id, u->body_ordinal,
                u->type_key, u->reason);
        dump_named_bitset(out, u->reason, interface_uses, interface_use_count,
                          xg_interface_object_use_name);
        fprintf(out, " flags=0x%x\n", u->flags);
    }
    for (uint32_t i = 0; i < evidence->nbodies; i++) {
        const XgBodySummary *b = &evidence->bodies[i];
        fprintf(out,
                "body %u func=%u module=%u node=%u decl=%u class=%u method=%u name=%u sig=%u "
                "span=%u kind=%s hash=%016" PRIx64 " effect=0x%x",
                i, b->func_id, b->module_id, b->source_node_id, b->owner_decl_id, b->owner_class_id,
                b->owner_method_id, b->name_id, b->signature_key, b->source_span_id,
                xg_body_kind_name(b->kind), b->body_hash, b->effect_bits);
        dump_named_bitset(out, b->effect_bits, effects, effect_count, xg_body_effect_name);
        fprintf(out, " escape=0x%x", b->escape_bits);
        dump_named_bitset(out, b->escape_bits, escapes, escape_count, xg_body_escape_name);
        fprintf(out, " caps=0x%x", b->capability_bits);
        dump_named_bitset(out, b->capability_bits, capabilities, capability_count,
                          xg_capability_name);
        fprintf(out, " param_storage=%u params=%u+%u callsites=%u+%u metadata=0x%x",
                b->param_storage_key, b->param_storage_start, b->param_storage_count,
                b->callsite_start, b->callsite_count, b->metadata_use_bits);
        dump_named_bitset(out, b->metadata_use_bits, metadata, metadata_count, xg_metadata_name);
        fprintf(out, " static=0x%x", b->static_data_use_bits);
        dump_named_bitset(out, b->static_data_use_bits, static_data, static_data_count,
                          xg_static_data_name);
        fprintf(out, "\n");
    }
    for (uint32_t i = 0; i < evidence->nparam_storages; i++) {
        const XgParamStorageSummary *p = &evidence->param_storages[i];
        fprintf(out, "param-storage %u id=%u owner=%u index=%u storage=%u flags=0x%x\n", i,
                p->requirement_id, p->owner_func_id, p->param_index, (unsigned) p->storage_owner,
                p->flags);
    }
    for (uint32_t i = 0; i < evidence->ncallsites; i++) {
        const XgCallsiteSummary *c = &evidence->callsites[i];
        fprintf(out,
                "callsite %u id=%u owner=%u node=%u span=%u kind=%s ordinal=%u target=%u "
                "recv_class=%u "
                "recv_iface=%u method=%u method_name=%u method_sig=%u args=%u+%u flags=0x%x\n",
                i, c->callsite_id, c->owner_func_id, c->source_node_id, c->source_span_id,
                xg_callsite_kind_name(c->kind), c->body_ordinal, c->static_target_func_id,
                c->receiver_static_class_id, c->receiver_static_interface_id, c->method_id,
                c->method_name_id, c->method_signature_key, c->arg_type_key_start,
                (unsigned) c->arg_count, c->flags);
    }
    for (uint32_t i = 0; i < evidence->nlink_deps; i++) {
        const XgLinkDependencySummary *dep = &evidence->link_deps[i];
        fprintf(out,
                "link-dep %u id=%u module=%u decl=%u span=%u kind=%s name_id=%u name=%s "
                "flags=0x%x\n",
                i, dep->link_id, dep->module_id, dep->decl_id, dep->source_span_id,
                xg_link_dependency_kind_name(dep->kind), dep->name_id, dep->name, dep->flags);
    }
    for (uint32_t i = 0; i < evidence->ngeneric_insts; i++) {
        const XgGenericInstSummary *inst = &evidence->generic_insts[i];
        fprintf(out,
                "generic-inst %u id=%u module=%u kind=%s origin_decl=%u origin_func=%u "
                "origin_method=%u origin_class=%u specialized_func=%u specialized_class=%u "
                "root_callsite=%u constraint_iface=%u name=%u type=%u type_args=%u+%u "
                "span=%u flags=0x%x\n",
                i, inst->generic_inst_id, inst->module_id, xg_generic_inst_kind_name(inst->kind),
                inst->origin_decl_id, inst->origin_func_id, inst->origin_method_id,
                inst->origin_class_id, inst->specialized_func_id, inst->specialized_class_id,
                inst->root_callsite_id, inst->constraint_interface_id, inst->name_id,
                inst->type_key, inst->type_arg_key_start, (unsigned) inst->type_arg_count,
                inst->source_span_id, inst->flags);
    }
    for (uint32_t i = 0; i < evidence->ngeneric_body_uses; i++) {
        const XgGenericBodyUseSummary *use = &evidence->generic_body_uses[i];
        fprintf(out,
                "generic-body-use %u id=%u inst=%u module=%u owner=%u origin_body=%u "
                "specialized_body=%u root_callsite=%u type=%u type_args=%u+%u size=%u "
                "flags=0x%x hash=%016" PRIx64 "\n",
                i, use->use_id, use->generic_inst_id, use->module_id, use->owner_func_id,
                use->origin_body_func_id, use->specialized_body_func_id, use->root_callsite_id,
                use->type_key, use->type_arg_key_start, (unsigned) use->type_arg_count,
                use->estimated_body_size, use->flags, use->body_use_hash);
    }
    for (uint32_t i = 0; i < evidence->ngeneric_storages; i++) {
        const XgGenericStorageSummary *storage = &evidence->generic_storages[i];
        fprintf(out,
                "generic-storage %u id=%u inst=%u module=%u kind=%s origin_type=%u "
                "specialized_type=%u elem_type=%u key_type=%u value_type=%u container_plan=%u "
                "flags=0x%x hash=%016" PRIx64 "\n",
                i, storage->storage_id, storage->generic_inst_id, storage->module_id,
                xg_generic_storage_kind_name(storage->storage_kind), storage->origin_type_key,
                storage->specialized_type_key, storage->elem_type_key, storage->key_type_key,
                storage->value_type_key, storage->container_plan_id, storage->flags,
                storage->storage_hash);
    }
    for (uint32_t i = 0; i < evidence->ngeneric_code_sizes; i++) {
        const XgGenericCodeSizeSummary *size = &evidence->generic_code_sizes[i];
        fprintf(out,
                "generic-code-size %u id=%u inst=%u module=%u body_use=%u origin=%u "
                "specialized=%u count=%u threshold=%u flags=0x%x\n",
                i, size->code_size_id, size->generic_inst_id, size->module_id, size->body_use_id,
                size->origin_body_size_estimate, size->specialized_body_size_estimate,
                size->instantiation_count, size->threshold, size->flags);
    }
    for (uint32_t i = 0; i < evidence->nsequence_accesses; i++) {
        const XgSequenceAccessSummary *seq = &evidence->sequence_accesses[i];
        fprintf(out,
                "seq-access %u id=%u owner=%u span=%u ordinal=%u kind=%s access=%s "
                "receiver_type=%u elem_type=%u index=%u length=%u flags=0x%x\n",
                i, seq->access_id, seq->owner_func_id, seq->source_span_id, seq->body_ordinal,
                xg_sequence_kind_name(seq->sequence_kind),
                xg_sequence_access_kind_name(seq->access_kind), seq->receiver_type_key,
                seq->elem_type_key, seq->index_expr_id, seq->length_expr_id, seq->flags);
    }
    for (uint32_t i = 0; i < evidence->ncapacity_ops; i++) {
        const XgCapacityOpSummary *cap = &evidence->capacity_ops[i];
        fprintf(out,
                "capacity-op %u id=%u owner=%u span=%u ordinal=%u kind=%s op=%s "
                "receiver_type=%u elem_type=%u count=%u loop=%u flags=0x%x\n",
                i, cap->op_id, cap->owner_func_id, cap->source_span_id, cap->body_ordinal,
                xg_sequence_kind_name(cap->sequence_kind), xg_capacity_op_kind_name(cap->op_kind),
                cap->receiver_type_key, cap->elem_type_key, cap->count_expr_id, cap->loop_id,
                cap->flags);
    }
    for (uint32_t i = 0; i < evidence->nbulk_ops; i++) {
        const XgBulkOpSummary *bulk = &evidence->bulk_ops[i];
        fprintf(out,
                "bulk-op %u id=%u owner=%u span=%u ordinal=%u op=%s elem_type=%u "
                "src_type=%u dst_type=%u length=%u flags=0x%x\n",
                i, bulk->op_id, bulk->owner_func_id, bulk->source_span_id, bulk->body_ordinal,
                xg_bulk_op_kind_name(bulk->op_kind), bulk->elem_type_key, bulk->src_type_key,
                bulk->dst_type_key, bulk->length_expr_id, bulk->flags);
    }
    for (uint32_t i = 0; i < evidence->nencoding_ops; i++) {
        const XgEncodingOpSummary *enc = &evidence->encoding_ops[i];
        fprintf(out,
                "encoding-op %u id=%u owner=%u span=%u ordinal=%u op=%s input_type=%u "
                "output_type=%u flags=0x%x\n",
                i, enc->op_id, enc->owner_func_id, enc->source_span_id, enc->body_ordinal,
                xg_encoding_op_kind_name(enc->op_kind), enc->input_type_key, enc->output_type_key,
                enc->flags);
    }
    for (uint32_t i = 0; i < evidence->nderives; i++) {
        const XgDeriveSummary *d = &evidence->derives[i];
        fprintf(out,
                "derive %u id=%u module=%u decl=%u type=%u kind=%s span=%u fields=%u+%u "
                "methods=%u+%u flags=0x%x hash=%016" PRIx64 "\n",
                i, d->derive_id, d->module_id, d->owner_decl_id, d->type_key,
                xg_derive_kind_name(d->derive_kind), d->source_span_id, d->field_start,
                (unsigned) d->field_count, d->method_start, (unsigned) d->method_count, d->flags,
                d->derive_hash);
    }
    for (uint32_t i = 0; i < evidence->nderived_fields; i++) {
        const XgDerivedFieldSummary *f = &evidence->derived_fields[i];
        fprintf(out,
                "derived-field %u id=%u derive=%u ord=%u name=%u type=%u source_field=%u "
                "flags=0x%x\n",
                i, f->field_id, f->derive_id, (unsigned) f->field_ordinal, f->name_id, f->type_key,
                f->source_field_id, f->flags);
    }
    for (uint32_t i = 0; i < evidence->nderived_methods; i++) {
        const XgDerivedMethodSummary *m = &evidence->derived_methods[i];
        fprintf(out, "derived-method %u id=%u derive=%u kind=%s body=%u sig=%u flags=0x%x\n", i,
                m->method_id, m->derive_id, xg_derived_method_kind_name(m->method_kind),
                m->generated_body_func_id, m->signature_key, m->flags);
    }
    for (uint32_t i = 0; i < evidence->njson_shapes; i++) {
        const XgJsonShapeSummary *s = &evidence->json_shapes[i];
        fprintf(out,
                "json-shape %u id=%u module=%u func=%u type=%u kind=%s span=%u fields=%u+%u "
                "record_shape=%u flags=0x%x hash=%016" PRIx64 "\n",
                i, s->json_shape_id, s->module_id, s->owner_func_id, s->type_key,
                xg_json_shape_kind_name(s->shape_kind), s->source_span_id, s->field_name_start,
                (unsigned) s->field_count, s->record_shape_id, s->flags, s->shape_hash);
    }
    for (uint32_t i = 0; i < evidence->njson_fields; i++) {
        const XgJsonFieldSummary *f = &evidence->json_fields[i];
        fprintf(out, "json-field %u id=%u shape=%u ord=%u name=%u type=%u flags=0x%x\n", i,
                f->field_id, f->shape_id, (unsigned) f->field_ordinal, f->name_id, f->type_key,
                f->flags);
    }
    for (uint32_t i = 0; i < evidence->njson_accesses; i++) {
        const XgJsonAccessSummary *a = &evidence->json_accesses[i];
        fprintf(out,
                "json-access %u id=%u module=%u func=%u shape=%u kind=%s span=%u key=%u "
                "result_type=%u field=%u flags=0x%x\n",
                i, a->json_access_id, a->module_id, a->owner_func_id, a->receiver_shape_id,
                xg_json_access_kind_name(a->access_kind), a->source_span_id, a->key_name_id,
                a->result_type_key, (unsigned) a->field_ordinal, a->flags);
    }
    for (uint32_t i = 0; i < evidence->njson_codecs; i++) {
        const XgJsonCodecSummary *c = &evidence->json_codecs[i];
        fprintf(out,
                "json-codec %u id=%u module=%u func=%u node=%u kind=%s span=%u input_type=%u "
                "target_type=%u input_shape=%u output_shape=%u fields=%u record_shape=%u "
                "flags=0x%x\n",
                i, c->codec_id, c->module_id, c->owner_func_id, c->source_node_id,
                xg_json_codec_kind_name(c->codec_kind), c->source_span_id, c->input_type_key,
                c->target_type_key, c->input_shape_id, c->output_shape_id,
                (unsigned) c->field_count, c->record_shape_id, c->flags);
    }
    for (uint32_t i = 0; i < evidence->nrecord_shapes; i++) {
        const XgRecordShapeSummary *s = &evidence->record_shapes[i];
        fprintf(out,
                "record-shape %u id=%u module=%u func=%u type=%u kind=%s span=%u fields=%u+%u "
                "json_shape=%u flags=0x%x hash=%016" PRIx64 "\n",
                i, s->record_shape_id, s->module_id, s->owner_func_id, s->type_key,
                xg_record_shape_kind_name(s->shape_kind), s->source_span_id, s->field_name_start,
                (unsigned) s->field_count, s->json_shape_id, s->flags, s->shape_hash);
    }
    for (uint32_t i = 0; i < evidence->nrecord_fields; i++) {
        const XgRecordFieldSummary *f = &evidence->record_fields[i];
        fprintf(out,
                "record-field %u id=%u shape=%u ord=%u name=%u type=%u default=%u flags=0x%x\n", i,
                f->field_id, f->shape_id, (unsigned) f->field_ordinal, f->name_id, f->type_key,
                f->default_value_id, f->flags);
    }
    for (uint32_t i = 0; i < evidence->nrecord_accesses; i++) {
        const XgRecordAccessSummary *a = &evidence->record_accesses[i];
        fprintf(out,
                "record-access %u id=%u module=%u func=%u shape=%u kind=%s span=%u field_name=%u "
                "result_type=%u field=%u flags=0x%x\n",
                i, a->record_access_id, a->module_id, a->owner_func_id, a->receiver_shape_id,
                xg_record_access_kind_name(a->access_kind), a->source_span_id, a->field_name_id,
                a->result_type_key, (unsigned) a->field_ordinal, a->flags);
    }
    for (uint32_t i = 0; i < evidence->nrecord_merges; i++) {
        const XgRecordMergeSummary *m = &evidence->record_merges[i];
        fprintf(out,
                "record-merge %u id=%u module=%u func=%u source=%u span=%u base_shape=%u "
                "patch_shape=%u "
                "result_shape=%u base_fields=%u patch_fields=%u result_fields=%u overwrites=%u "
                "copy_table=%u flags=0x%x hash=%016" PRIx64 "\n",
                i, m->merge_id, m->module_id, m->owner_func_id, m->source_node_id,
                m->source_span_id, m->base_shape_id, m->patch_shape_id, m->result_shape_id,
                (unsigned) m->base_field_count, (unsigned) m->patch_field_count,
                (unsigned) m->result_field_count, (unsigned) m->overwrite_count, m->copy_table_id,
                m->flags, m->merge_hash);
    }
    for (uint32_t i = 0; i < evidence->noptions_bags; i++) {
        const XgOptionsBagSummary *o = &evidence->options_bags[i];
        fprintf(out,
                "options-bag %u id=%u module=%u func=%u callsite=%u param_shape=%u "
                "supplied_shape=%u action=%s span=%u supplied_mask=%u default_mask=%u "
                "required_mask=%u supplied=%u defaults=%u required=%u flags=0x%x\n",
                i, o->options_id, o->module_id, o->owner_func_id, o->callsite_id, o->param_shape_id,
                o->supplied_shape_id, xg_options_action_name(o->action), o->source_span_id,
                o->supplied_field_mask_id, o->default_field_mask_id, o->required_field_mask_id,
                (unsigned) o->supplied_count, (unsigned) o->default_count,
                (unsigned) o->required_count, o->flags);
    }
    for (uint32_t i = 0; i < evidence->nmap_shapes; i++) {
        const XgMapShapeSummary *s = &evidence->map_shapes[i];
        fprintf(out,
                "map-shape %u id=%u module=%u func=%u container=%s source=%s span=%u "
                "key_type=%u value_type=%u entries=%u+%u literal_count=%u flags=0x%x "
                "hash=%016" PRIx64 "\n",
                i, s->shape_id, s->module_id, s->owner_func_id,
                xg_map_container_kind_name(s->container_kind), xg_map_shape_source_name(s->source),
                s->source_span_id, s->key_type_key, s->value_type_key, s->entry_start,
                (unsigned) s->entry_count, s->literal_count, s->flags, s->shape_hash);
    }
    for (uint32_t i = 0; i < evidence->nmap_entries; i++) {
        const XgMapEntrySummary *e = &evidence->map_entries[i];
        fprintf(out,
                "map-entry %u id=%u shape=%u ord=%u key_const=%u value_const=%u "
                "key_i64=%" PRId64 " prehash=%016" PRIx64 " flags=0x%x\n",
                i, e->entry_id, e->shape_id, e->entry_ordinal, e->key_const_id, e->value_const_id,
                e->key_i64, e->prehash, e->flags);
    }
    for (uint32_t i = 0; i < evidence->nkey_accesses; i++) {
        const XgKeyAccessSummary *a = &evidence->key_accesses[i];
        fprintf(out,
                "key-access %u id=%u func=%u span=%u ordinal=%u container=%s op=%s shape=%u "
                "receiver_type=%u key_type=%u value_type=%u key_const=%u prehash=%016" PRIx64
                " flags=0x%x\n",
                i, a->access_id, a->owner_func_id, a->source_span_id, a->body_ordinal,
                xg_map_container_kind_name(a->container_kind), xg_key_access_op_name(a->op),
                a->receiver_shape_id, a->receiver_type_key, a->key_type_key, a->value_type_key,
                a->key_const_id, a->key_prehash, a->flags);
    }
    for (uint32_t i = 0; i < evidence->nhash_eqs; i++) {
        const XgHashEqSummary *h = &evidence->hash_eqs[i];
        fprintf(out,
                "hash-eq %u id=%u type=%u kind=%s eq_derive=%u hash_derive=%u eq_func=%u "
                "hash_func=%u flags=0x%x\n",
                i, h->hash_eq_id, h->type_key, xg_hash_eq_kind_name(h->kind), h->eq_derive_id,
                h->hash_derive_id, h->eq_func_id, h->hash_func_id, h->flags);
    }

    if (ferror(out)) {
        (void) xr_close_memstream(out, &buf, &bufsz);
        xr_free(buf);
        return NULL;
    }
    if (xr_close_memstream(out, &buf, &bufsz) != 0)
        return NULL;
    return buf;
}
