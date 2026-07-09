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

static size_t bounded_cstr_len(const char *s, size_t max_len) {
    size_t len = 0;
    if (!s)
        return 0;
    while (len < max_len && s[len])
        len++;
    return len;
}

static uint64_t hash_decl_summary(uint64_t hash, const XgDeclSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->module_id);
    hash = hash_u32(hash, row->decl_id);
    hash = hash_u8(hash, row->kind);
    hash = hash_u32(hash, row->flags);
    hash = hash_u32(hash, row->name_id);
    hash = hash_u32(hash, row->type_key);
    hash = hash_u32(hash, row->signature_key);
    hash = hash_u32(hash, row->source_span_id);
    return hash_u32(hash, row->derive_flags);
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

static uint64_t hash_method_summary(uint64_t hash, const XgMethodSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->method_id);
    hash = hash_u32(hash, row->owner_class_id);
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

static uint64_t hash_body_summary(uint64_t hash, const XgBodySummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->func_id);
    hash = hash_u32(hash, row->module_id);
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
    hash = hash_u32(hash, row->callsite_start);
    hash = hash_u32(hash, row->callsite_count);
    hash = hash_u32(hash, row->metadata_use_bits);
    return hash_u32(hash, row->static_data_use_bits);
}

static uint64_t hash_callsite_summary(uint64_t hash, const XgCallsiteSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
    hash = hash_u32(hash, row->callsite_id);
    hash = hash_u32(hash, row->owner_func_id);
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

static uint64_t hash_record_shape_summary(uint64_t hash, const XgRecordShapeSummary *row) {
    if (!row)
        return hash_u32(hash, 0);
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
        case XG_BODY_MAY_THROW:
            return "throw";
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
        default:
            return "unknown";
    }
}

XR_FUNC const uint32_t *xg_body_effect_catalog(uint32_t *out_count) {
    static const uint32_t effects[] = {
        XG_BODY_MAY_THROW,       XG_BODY_MAY_SUSPEND,  XG_BODY_MAY_ALLOC, XG_BODY_MAY_MUTATE,
        XG_BODY_MAY_CALL_NATIVE, XG_BODY_MAY_READ_MEM, XG_BODY_MAY_CALL,
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
    xr_free(evidence->decls);
    xr_free(evidence->classes);
    xr_free(evidence->methods);
    xr_free(evidence->interface_impls);
    xr_free(evidence->interface_extends);
    xr_free(evidence->interface_methods);
    xr_free(evidence->bodies);
    xr_free(evidence->callsites);
    xr_free(evidence->link_deps);
    xr_free(evidence->generic_insts);
    xr_free(evidence->derives);
    xr_free(evidence->derived_fields);
    xr_free(evidence->derived_methods);
    xr_free(evidence->json_shapes);
    xr_free(evidence->json_accesses);
    xr_free(evidence->record_shapes);
    xr_free(evidence->record_accesses);
    xr_free(evidence->map_shapes);
    xr_free(evidence->map_entries);
    xr_free(evidence->key_accesses);
    xr_free(evidence->hash_eqs);
    memset(evidence, 0, sizeof(*evidence));
}

XR_FUNC bool xg_global_evidence_reserve_decls(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->decls, &evidence->decl_cap, capacity,
                                     sizeof(XgDeclSummary));
}

XR_FUNC bool xg_global_evidence_reserve_classes(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->classes, &evidence->class_cap, capacity,
                                     sizeof(XgClassSummary));
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

XR_FUNC bool xg_global_evidence_reserve_bodies(XgGlobalEvidence *evidence, uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->bodies, &evidence->body_cap, capacity,
                                     sizeof(XgBodySummary));
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

XR_FUNC bool xg_global_evidence_reserve_json_accesses(XgGlobalEvidence *evidence,
                                                      uint32_t capacity) {
    return evidence && reserve_array((void **) &evidence->json_accesses, &evidence->json_access_cap,
                                     capacity, sizeof(XgJsonAccessSummary));
}

XR_FUNC bool xg_global_evidence_reserve_record_shapes(XgGlobalEvidence *evidence,
                                                      uint32_t capacity) {
    return evidence &&
           reserve_array((void **) &evidence->record_shapes, &evidence->record_shape_cap, capacity,
                         sizeof(XgRecordShapeSummary));
}

XR_FUNC bool xg_global_evidence_reserve_record_accesses(XgGlobalEvidence *evidence,
                                                        uint32_t capacity) {
    return evidence &&
           reserve_array((void **) &evidence->record_accesses, &evidence->record_access_cap,
                         capacity, sizeof(XgRecordAccessSummary));
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

static bool xg_class_summary_is_runtime_class(const XgClassSummary *cls) {
    return cls && (cls->decl_kind == 0 || cls->decl_kind == XG_DECL_CLASS);
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
    uint32_t target_index = 0;
    uint32_t target_effects = 0;
    if (!effect_bits ||
        !xg_global_evidence_find_body_index_by_method(evidence, method_id, &target_index))
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
    if (!method || (method->flags & XG_METHOD_NATIVE) != 0)
        return false;
    for (uint32_t i = 0; i < evidence->nclasses; i++) {
        const XgClassSummary *candidate = &evidence->classes[i];
        const XgMethodSummary *target_method;
        if (!xg_class_summary_is_runtime_class(candidate))
            continue;
        if (!xg_global_evidence_class_is_descendant_or_self(evidence, candidate->class_id,
                                                            call->receiver_static_class_id))
            continue;
        target_method = xg_global_evidence_find_method_by_signature_in_hierarchy(
            evidence, candidate->class_id, call->method_name_id, call->method_signature_key);
        if (!target_method || (target_method->flags & XG_METHOD_NATIVE) != 0 ||
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
    enum {
        XG_SMALL_IMPLEMENTOR_LIMIT = 4
    };
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
        if (target_count > XG_SMALL_IMPLEMENTOR_LIMIT)
            return false;
        target_method = xg_global_evidence_find_method_by_signature_in_hierarchy(
            evidence, impl->implementor_class_id, call->method_name_id, call->method_signature_key);
        if (!target_method || (target_method->flags & XG_METHOD_NATIVE) != 0 ||
            !xg_body_effects_compose_method_target(evidence, target_method->method_id, state, memo,
                                                   effect_bits))
            return false;
    }
    return target_count > 0;
}

static bool xg_body_effects_compose_rec(const XgGlobalEvidence *evidence, uint32_t body_index,
                                        uint8_t *state, uint32_t *memo, uint32_t *out_effect_bits) {
    const XgBodySummary *body;
    uint32_t effect_bits;

    if (!evidence || body_index >= evidence->nbodies || !state || !memo || !out_effect_bits)
        return false;
    if (state[body_index] == 1)
        return false;
    if (state[body_index] == 2) {
        *out_effect_bits = memo[body_index];
        return true;
    }

    state[body_index] = 1;
    body = &evidence->bodies[body_index];
    effect_bits = body->effect_bits & ~XG_BODY_MAY_CALL;

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
            if (call->kind == XG_CALL_DIRECT_FUNC) {
                if (call->static_target_func_id == XG_NO_ID ||
                    !xg_global_evidence_find_body_index_by_func(
                        evidence, call->static_target_func_id, &target_index))
                    return false;
                if (!xg_body_effects_compose_rec(evidence, target_index, state, memo,
                                                 &target_effects))
                    return false;
                effect_bits |= target_effects;
            } else if (xg_method_callsite_is_direct_dispatch(evidence, call)) {
                if (!xg_body_effects_compose_method_target(evidence, call->method_id, state, memo,
                                                           &effect_bits))
                    return false;
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
            } else if (call->kind == XG_CALL_METHOD) {
                if (!xg_method_callsite_compose_target_set(evidence, call, state, memo,
                                                           &effect_bits))
                    return false;
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
    hash = hash_mix(hash, &evidence->ndecls, sizeof(evidence->ndecls));
    hash = hash_mix(hash, &evidence->nclasses, sizeof(evidence->nclasses));
    hash = hash_mix(hash, &evidence->nmethods, sizeof(evidence->nmethods));
    hash = hash_mix(hash, &evidence->ninterface_impls, sizeof(evidence->ninterface_impls));
    hash = hash_mix(hash, &evidence->ninterface_extends, sizeof(evidence->ninterface_extends));
    hash = hash_mix(hash, &evidence->ninterface_methods, sizeof(evidence->ninterface_methods));
    hash = hash_mix(hash, &evidence->nbodies, sizeof(evidence->nbodies));
    hash = hash_mix(hash, &evidence->ncallsites, sizeof(evidence->ncallsites));
    hash = hash_mix(hash, &evidence->nlink_deps, sizeof(evidence->nlink_deps));
    hash = hash_mix(hash, &evidence->ngeneric_insts, sizeof(evidence->ngeneric_insts));
    hash = hash_mix(hash, &evidence->nderives, sizeof(evidence->nderives));
    hash = hash_mix(hash, &evidence->nderived_fields, sizeof(evidence->nderived_fields));
    hash = hash_mix(hash, &evidence->nderived_methods, sizeof(evidence->nderived_methods));
    hash = hash_mix(hash, &evidence->njson_shapes, sizeof(evidence->njson_shapes));
    hash = hash_mix(hash, &evidence->njson_accesses, sizeof(evidence->njson_accesses));
    hash = hash_mix(hash, &evidence->nrecord_shapes, sizeof(evidence->nrecord_shapes));
    hash = hash_mix(hash, &evidence->nrecord_accesses, sizeof(evidence->nrecord_accesses));
    hash = hash_mix(hash, &evidence->nmap_shapes, sizeof(evidence->nmap_shapes));
    hash = hash_mix(hash, &evidence->nmap_entries, sizeof(evidence->nmap_entries));
    hash = hash_mix(hash, &evidence->nkey_accesses, sizeof(evidence->nkey_accesses));
    hash = hash_mix(hash, &evidence->nhash_eqs, sizeof(evidence->nhash_eqs));
    for (uint32_t i = 0; i < evidence->ndecls; i++)
        hash = hash_decl_summary(hash, &evidence->decls[i]);
    for (uint32_t i = 0; i < evidence->nclasses; i++)
        hash = hash_class_summary(hash, &evidence->classes[i]);
    for (uint32_t i = 0; i < evidence->nmethods; i++)
        hash = hash_method_summary(hash, &evidence->methods[i]);
    for (uint32_t i = 0; i < evidence->ninterface_impls; i++)
        hash = hash_interface_impl_summary(hash, &evidence->interface_impls[i]);
    for (uint32_t i = 0; i < evidence->ninterface_extends; i++)
        hash = hash_interface_extends_summary(hash, &evidence->interface_extends[i]);
    for (uint32_t i = 0; i < evidence->ninterface_methods; i++)
        hash = hash_interface_method_summary(hash, &evidence->interface_methods[i]);
    for (uint32_t i = 0; i < evidence->nbodies; i++)
        hash = hash_body_summary(hash, &evidence->bodies[i]);
    for (uint32_t i = 0; i < evidence->ncallsites; i++)
        hash = hash_callsite_summary(hash, &evidence->callsites[i]);
    for (uint32_t i = 0; i < evidence->nlink_deps; i++)
        hash = hash_link_dependency_summary(hash, &evidence->link_deps[i]);
    for (uint32_t i = 0; i < evidence->ngeneric_insts; i++)
        hash = hash_generic_inst_summary(hash, &evidence->generic_insts[i]);
    for (uint32_t i = 0; i < evidence->nderives; i++)
        hash = hash_derive_summary(hash, &evidence->derives[i]);
    for (uint32_t i = 0; i < evidence->nderived_fields; i++)
        hash = hash_derived_field_summary(hash, &evidence->derived_fields[i]);
    for (uint32_t i = 0; i < evidence->nderived_methods; i++)
        hash = hash_derived_method_summary(hash, &evidence->derived_methods[i]);
    for (uint32_t i = 0; i < evidence->njson_shapes; i++)
        hash = hash_json_shape_summary(hash, &evidence->json_shapes[i]);
    for (uint32_t i = 0; i < evidence->njson_accesses; i++)
        hash = hash_json_access_summary(hash, &evidence->json_accesses[i]);
    for (uint32_t i = 0; i < evidence->nrecord_shapes; i++)
        hash = hash_record_shape_summary(hash, &evidence->record_shapes[i]);
    for (uint32_t i = 0; i < evidence->nrecord_accesses; i++)
        hash = hash_record_access_summary(hash, &evidence->record_accesses[i]);
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
            hash = hash_u32(hash, evidence->ndecls);
            for (uint32_t i = 0; i < evidence->ndecls; i++)
                hash = hash_decl_summary(hash, &evidence->decls[i]);
            break;
        case XG_EVIDENCE_CACHE_SEMANTIC_GRAPH:
            hash = hash_u32(hash, evidence->ndecls);
            hash = hash_u32(hash, evidence->nclasses);
            hash = hash_u32(hash, evidence->nmethods);
            hash = hash_u32(hash, evidence->ninterface_impls);
            hash = hash_u32(hash, evidence->ninterface_extends);
            hash = hash_u32(hash, evidence->ninterface_methods);
            hash = hash_u32(hash, evidence->nderives);
            hash = hash_u32(hash, evidence->nderived_fields);
            hash = hash_u32(hash, evidence->nderived_methods);
            for (uint32_t i = 0; i < evidence->ndecls; i++)
                hash = hash_decl_summary(hash, &evidence->decls[i]);
            for (uint32_t i = 0; i < evidence->nclasses; i++)
                hash = hash_class_summary(hash, &evidence->classes[i]);
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
            hash = hash_u32(hash, evidence->ncallsites);
            hash = hash_u32(hash, evidence->nlink_deps);
            hash = hash_u32(hash, evidence->ngeneric_insts);
            for (uint32_t i = 0; i < evidence->nbodies; i++)
                hash = hash_body_summary(hash, &evidence->bodies[i]);
            for (uint32_t i = 0; i < evidence->ncallsites; i++)
                hash = hash_callsite_summary(hash, &evidence->callsites[i]);
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
            "counts decls=%u classes=%u methods=%u interface_impls=%u interface_extends=%u "
            "interface_methods=%u bodies=%u callsites=%u link_deps=%u generic_insts=%u "
            "derives=%u derived_fields=%u derived_methods=%u json_shapes=%u "
            "json_accesses=%u record_shapes=%u record_accesses=%u map_shapes=%u map_entries=%u "
            "key_accesses=%u hash_eqs=%u\n",
            evidence->ndecls, evidence->nclasses, evidence->nmethods, evidence->ninterface_impls,
            evidence->ninterface_extends, evidence->ninterface_methods, evidence->nbodies,
            evidence->ncallsites, evidence->nlink_deps, evidence->ngeneric_insts,
            evidence->nderives, evidence->nderived_fields, evidence->nderived_methods,
            evidence->njson_shapes, evidence->njson_accesses, evidence->nrecord_shapes,
            evidence->nrecord_accesses, evidence->nmap_shapes, evidence->nmap_entries,
            evidence->nkey_accesses, evidence->nhash_eqs);

    for (uint32_t i = 0; i < evidence->ndecls; i++) {
        const XgDeclSummary *d = &evidence->decls[i];
        fprintf(out,
                "decl %u id=%u module=%u kind=%s flags=0x%x name=%u type=%u sig=%u span=%u "
                "derive=0x%x\n",
                i, d->decl_id, d->module_id, xg_decl_kind_name(d->kind), d->flags, d->name_id,
                d->type_key, d->signature_key, d->source_span_id, d->derive_flags);
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
    for (uint32_t i = 0; i < evidence->nmethods; i++) {
        const XgMethodSummary *m = &evidence->methods[i];
        fprintf(out,
                "method %u id=%u owner=%u name=%u sig=%u override_of=%u root=%u depth=%u "
                "defaults=%u flags=0x%x\n",
                i, m->method_id, m->owner_class_id, m->name_id, m->signature_key, m->override_of,
                m->root_method_id, m->override_depth, m->default_arg_contract_id, m->flags);
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
    for (uint32_t i = 0; i < evidence->nbodies; i++) {
        const XgBodySummary *b = &evidence->bodies[i];
        fprintf(
            out,
            "body %u func=%u module=%u decl=%u class=%u method=%u name=%u sig=%u span=%u kind=%s "
            "hash=%016" PRIx64 " effect=0x%x",
            i, b->func_id, b->module_id, b->owner_decl_id, b->owner_class_id, b->owner_method_id,
            b->name_id, b->signature_key, b->source_span_id, xg_body_kind_name(b->kind),
            b->body_hash, b->effect_bits);
        dump_named_bitset(out, b->effect_bits, effects, effect_count, xg_body_effect_name);
        fprintf(out, " escape=0x%x", b->escape_bits);
        dump_named_bitset(out, b->escape_bits, escapes, escape_count, xg_body_escape_name);
        fprintf(out, " caps=0x%x", b->capability_bits);
        dump_named_bitset(out, b->capability_bits, capabilities, capability_count,
                          xg_capability_name);
        fprintf(out, " callsites=%u+%u metadata=0x%x", b->callsite_start, b->callsite_count,
                b->metadata_use_bits);
        dump_named_bitset(out, b->metadata_use_bits, metadata, metadata_count, xg_metadata_name);
        fprintf(out, " static=0x%x", b->static_data_use_bits);
        dump_named_bitset(out, b->static_data_use_bits, static_data, static_data_count,
                          xg_static_data_name);
        fprintf(out, "\n");
    }
    for (uint32_t i = 0; i < evidence->ncallsites; i++) {
        const XgCallsiteSummary *c = &evidence->callsites[i];
        fprintf(out,
                "callsite %u id=%u owner=%u span=%u kind=%s ordinal=%u target=%u recv_class=%u "
                "recv_iface=%u method=%u method_name=%u method_sig=%u args=%u+%u flags=0x%x\n",
                i, c->callsite_id, c->owner_func_id, c->source_span_id,
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
                "flags=0x%x hash=%016" PRIx64 "\n",
                i, s->json_shape_id, s->module_id, s->owner_func_id, s->type_key,
                xg_json_shape_kind_name(s->shape_kind), s->source_span_id, s->field_name_start,
                (unsigned) s->field_count, s->flags, s->shape_hash);
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
    for (uint32_t i = 0; i < evidence->nrecord_shapes; i++) {
        const XgRecordShapeSummary *s = &evidence->record_shapes[i];
        fprintf(out,
                "record-shape %u id=%u module=%u func=%u type=%u kind=%s span=%u fields=%u+%u "
                "flags=0x%x hash=%016" PRIx64 "\n",
                i, s->record_shape_id, s->module_id, s->owner_func_id, s->type_key,
                xg_record_shape_kind_name(s->shape_kind), s->source_span_id, s->field_name_start,
                (unsigned) s->field_count, s->flags, s->shape_hash);
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
                "prehash=%016" PRIx64 " flags=0x%x\n",
                i, e->entry_id, e->shape_id, e->entry_ordinal, e->key_const_id, e->value_const_id,
                e->prehash, e->flags);
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
