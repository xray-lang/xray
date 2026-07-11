/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xglobal_producer.c - Whole-program evidence producer from module ASTs
 */

#include "xglobal_producer.h"

#include "../base/xfileio.h"
#include "../base/xhash.h"
#include "../base/xmalloc.h"
#include "../frontend/parser/xast.h"
#include "../frontend/parser/xtype_ref.h"
#include "../module/xmodule_graph.h"
#include "../module/xstdlib_embedded.h"
#include "../shared/xr_derive_flags.h"
#include "../shared/xr_hash_core.h"
#include "../stdlib/xstdlib_metadata.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    XG_SMALL_MAP_LITERAL_MAX = 4
};

typedef struct XgClassNameRow {
    XgModuleId module_id;
    const char *name;
    const char *super_name;
    XgClassId class_id;
    uint32_t summary_index;
} XgClassNameRow;

typedef struct XgFuncNameRow {
    const char *name;
    XgFuncId func_id;
    XgDeclId decl_id;
    uint32_t decl_flags;
} XgFuncNameRow;

typedef struct XgInterfaceNameRow {
    XgModuleId module_id;
    const char *name;
    XgInterfaceId interface_id;
    const InterfaceDeclNode *decl;
} XgInterfaceNameRow;

typedef struct XgStdlibImportRow {
    XgModuleId module_id;
    const char *local_name;
    const char *module_name;
    const char *member_name;
} XgStdlibImportRow;

typedef struct XgLocalType XgLocalType;
typedef struct XgLocalName XgLocalName;

typedef struct XgProducer {
    XgGlobalEvidence *evidence;
    XgClassNameRow *classes;
    uint32_t nclasses;
    uint32_t class_cap;
    XgInterfaceNameRow *interfaces;
    uint32_t ninterfaces;
    uint32_t interface_cap;
    XgFuncNameRow *funcs;
    uint32_t nfuncs;
    uint32_t func_cap;
    XgStdlibImportRow *stdlib_imports;
    uint32_t nstdlib_imports;
    uint32_t stdlib_import_cap;
    struct XgPendingBody *bodies;
    uint32_t nbodies;
    uint32_t body_cap;
    XgFuncId next_func_id;
} XgProducer;

typedef struct XgPendingBody {
    XgFuncId func_id;
    XgModuleId module_id;
    XgDeclId owner_decl_id;
    XgClassId current_class_id;
    XgMethodId owner_method_id;
    uint32_t name_id;
    uint32_t signature_key;
    uint32_t source_node_id;
    uint32_t source_span_id;
    uint8_t kind;
    const AstNode *body;
    const MethodDeclNode *method;
    const FunctionDeclNode *function;
    XgLocalType *captured_locals;
    uint32_t captured_local_count;
    XgLocalName *captured_name_locals;
    uint32_t captured_name_local_count;
} XgPendingBody;

struct XgLocalType {
    const char *name;
    XgClassId class_id;
    XgInterfaceId interface_id;
    uint32_t type_key;
    XgJsonShapeId json_shape_id;
    const ObjectLiteralNode *json_shape_literal;
    XgRecordShapeId record_shape_id;
    const ObjectLiteralNode *record_shape_literal;
    XgMapShapeId map_shape_id;
    uint8_t map_container_kind;
    uint32_t map_receiver_type_key;
    uint32_t map_key_type_key;
    uint32_t map_value_type_key;
    uint8_t sequence_kind;
    uint32_t sequence_elem_type_key;
    uint8_t sequence_elem_map_container_kind;
    uint32_t sequence_elem_map_key_type_key;
    uint32_t sequence_elem_map_value_type_key;
    bool inferred;
};

struct XgLocalName {
    const char *name;
    uint32_t symbol_id;
};

typedef struct XgBodyCollect {
    XgProducer *producer;
    XgGlobalEvidence *evidence;
    XgFuncId owner_func_id;
    XgModuleId module_id;
    XgClassId current_class_id;
    XgLocalType *locals;
    uint32_t nlocals;
    uint32_t local_cap;
    XgLocalName *name_locals;
    uint32_t nname_locals;
    uint32_t name_local_cap;
    uint32_t callsite_start;
    uint32_t callsite_count;
    uint32_t key_access_count;
    uint32_t interface_object_use_count;
    uint32_t sequence_access_count;
    uint32_t capacity_op_count;
    uint32_t bulk_op_count;
    uint32_t encoding_op_count;
    uint32_t effect_bits;
    uint32_t escape_bits;
    uint32_t capability_bits;
    uint32_t metadata_use_bits;
    uint32_t static_data_use_bits;
    const XrTypeRef *return_type;
} XgBodyCollect;

static uint64_t fold_bytes(uint64_t h, const void *data, size_t len) {
    uint64_t part = xr_hash_bytes64(data, len);
    h ^= part + UINT64_C(0x9e3779b97f4a7c15) + (h << 6) + (h >> 2);
    return h ? h : 1;
}

static uint64_t fold_u64(uint64_t h, uint64_t value) {
    return fold_bytes(h, &value, sizeof(value));
}

static uint32_t hash_folded32(uint64_t h) {
    uint32_t folded = (uint32_t) (h ^ (h >> 32));
    return folded ? folded : 1;
}

static uint32_t hash_name32(const char *name) {
    return xg_name_id(name);
}

static uint64_t hash_text64(const char *text) {
    if (!text || !*text)
        return 0;
    return xr_hash_bytes64(text, strlen(text));
}

static uint32_t producer_source_node_id(XgModuleId module_id, const AstNode *node) {
    const AstNode *loc;
    uint32_t line;
    uint32_t column;
    if (!node)
        return 0;
    loc = node;
    if (node->type == AST_CALL_EXPR && node->as.call_expr.callee)
        loc = node->as.call_expr.callee;
    line = loc && loc->line > 0 ? (uint32_t) loc->line : (uint32_t) node->line;
    if (line == 0 && loc && loc->end_line > 0)
        line = (uint32_t) loc->end_line;
    column = loc && loc->column > 0 ? (uint32_t) loc->column : (uint32_t) node->column;
    if (column == 0 && loc && loc->end_column > 0)
        column = (uint32_t) loc->end_column;
    if (column == 0)
        column = 1;
    return xg_stable_source_node_id(module_id, (uint32_t) node->type, line, column);
}

static bool body_callsite_source_seen(const XgBodyCollect *bc, uint32_t source_node_id) {
    if (!bc || !bc->evidence || source_node_id == 0)
        return false;
    for (uint32_t i = 0; i < bc->evidence->ncallsites; i++) {
        const XgCallsiteSummary *call = &bc->evidence->callsites[i];
        if (call->owner_func_id == bc->owner_func_id && call->source_node_id == source_node_id)
            return true;
    }
    return false;
}

static uint32_t producer_unique_callsite_source_node_id(const XgBodyCollect *bc,
                                                        uint32_t source_node_id) {
    uint32_t candidate = source_node_id;
    uint32_t salt = 1;
    if (!bc || !bc->evidence || source_node_id == 0)
        return source_node_id;
    while (body_callsite_source_seen(bc, candidate)) {
        uint64_t h = XR_FNV64_OFFSET_BASIS;
        h = fold_u64(h, source_node_id);
        h = fold_u64(h, bc->owner_func_id);
        h = fold_u64(h, bc->callsite_count);
        h = fold_u64(h, salt++);
        candidate = hash_folded32(h);
    }
    return candidate;
}

static bool producer_body_source_seen(const XgProducer *p, XgModuleId module_id,
                                      uint32_t source_node_id) {
    if (!p || source_node_id == 0)
        return false;
    for (uint32_t i = 0; i < p->nbodies; i++) {
        const XgPendingBody *body = &p->bodies[i];
        if (body->module_id == module_id && body->source_node_id == source_node_id)
            return true;
    }
    if (p->evidence) {
        for (uint32_t i = 0; i < p->evidence->nbodies; i++) {
            const XgBodySummary *body = &p->evidence->bodies[i];
            if (body->module_id == module_id && body->source_node_id == source_node_id)
                return true;
        }
    }
    return false;
}

static uint32_t producer_unique_body_source_node_id(const XgProducer *p, XgModuleId module_id,
                                                    uint32_t source_node_id, XgFuncId func_id,
                                                    uint32_t name_id, uint32_t signature_key) {
    uint32_t candidate = source_node_id;
    uint32_t salt = 1;
    if (!p || source_node_id == 0)
        return source_node_id;
    while (producer_body_source_seen(p, module_id, candidate)) {
        uint64_t h = XR_FNV64_OFFSET_BASIS;
        h = fold_u64(h, module_id);
        h = fold_u64(h, source_node_id);
        h = fold_u64(h, func_id);
        h = fold_u64(h, name_id);
        h = fold_u64(h, signature_key);
        h = fold_u64(h, p->nbodies);
        h = fold_u64(h, salt++);
        candidate = hash_folded32(h);
    }
    return candidate;
}

static bool producer_stdlib_module_known(const char *name) {
    return xr_stdlib_metadata_link_dependency_module_known(name);
}

static bool producer_stdlib_member_is_constant(const char *module, const char *member) {
    if (!module || !member)
        return false;
    for (uint32_t i = 0; i < XR_STDLIB_CONST_DEF_ENTRY_COUNT; i++) {
        const XrStdlibConstDefEntry *entry = &xr_stdlib_const_def_entries[i];
        if (entry->module && entry->name && strcmp(entry->module, module) == 0 &&
            strcmp(entry->name, member) == 0)
            return true;
    }
    return false;
}

static bool producer_add_link_dependency(XgProducer *p, XgModuleId module_id, XgDeclId decl_id,
                                         uint32_t source_span_id, uint8_t kind, const char *name) {
    XgLinkDependencySummary dep;
    size_t len;
    if (!p || !p->evidence || !name || !name[0])
        return true;
    for (uint32_t i = 0; i < p->evidence->nlink_deps; i++) {
        const XgLinkDependencySummary *existing = &p->evidence->link_deps[i];
        if (existing->kind == kind && strcmp(existing->name, name) == 0)
            return true;
    }
    len = strlen(name);
    if (len >= XG_LINK_DEP_NAME_MAX)
        len = XG_LINK_DEP_NAME_MAX - 1;
    memset(&dep, 0, sizeof(dep));
    dep.link_id = (XgLinkId) (p->evidence->nlink_deps + 1);
    dep.module_id = module_id;
    dep.decl_id = decl_id;
    dep.source_span_id = source_span_id;
    dep.kind = kind;
    dep.name_id = hash_name32(name);
    memcpy(dep.name, name, len);
    dep.name[len] = '\0';
    return xg_global_evidence_add_link_dependency(p->evidence, &dep) != NULL;
}

static bool producer_add_stdlib_symbol_dependency(XgProducer *p, XgModuleId module_id,
                                                  uint32_t source_span_id, const char *module,
                                                  const char *member) {
    char symbol[XG_LINK_DEP_NAME_MAX];
    int n;
    if (!module || !module[0] || !member || !member[0])
        return true;
    n = snprintf(symbol, sizeof(symbol), "%s.%s", module, member);
    if (n <= 0 || n >= (int) sizeof(symbol))
        return true;
    return producer_add_link_dependency(p, module_id, XG_NO_ID, source_span_id,
                                        XG_LINK_DEP_STDLIB_SYMBOL, symbol);
}

static uint64_t hash_tref(uint64_t h, const XrTypeRef *t) {
    if (!t)
        return fold_u64(h, 0);
    h = fold_u64(h, t->kind);
    h = fold_u64(h, t->native_width);
    h = fold_u64(h, t->fixed_length);
    h = fold_u64(h, t->extensible ? 1 : 0);
    if (t->name)
        h = fold_bytes(h, t->name, strlen(t->name));
    for (uint8_t i = 0; i < t->nchildren; i++)
        h = hash_tref(h, t->children ? t->children[i] : NULL);
    if (t->field_names) {
        for (uint8_t i = 0; i < t->nchildren; i++) {
            const char *field = t->field_names[i];
            if (field)
                h = fold_bytes(h, field, strlen(field));
        }
    }
    return h;
}

static uint32_t hash_tref32(const XrTypeRef *t) {
    uint64_t h = hash_tref(XR_FNV64_OFFSET_BASIS, t);
    uint32_t folded = (uint32_t) (h ^ (h >> 32));
    return folded ? folded : 1;
}

static uint32_t hash_tref_list32(XrTypeRef **type_args, int type_arg_count) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    if (!type_args || type_arg_count <= 0)
        return 0;
    h = fold_u64(h, (uint64_t) type_arg_count);
    for (int i = 0; i < type_arg_count; i++)
        h = hash_tref(h, type_args[i]);
    return hash_folded32(h);
}

static uint32_t hash_name_list32(const char **names, int name_count) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    if (!names || name_count <= 0)
        return 0;
    h = fold_u64(h, (uint64_t) name_count);
    for (int i = 0; i < name_count; i++) {
        if (names[i])
            h = fold_bytes(h, names[i], strlen(names[i]));
        else
            h = fold_u64(h, 0);
    }
    return hash_folded32(h);
}

static uint32_t hash_synthetic_tref32(uint8_t kind, const char *name, XrTypeRef **children,
                                      int child_count) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    h = fold_u64(h, kind);
    h = fold_u64(h, 0);
    h = fold_u64(h, 0);
    h = fold_u64(h, 0);
    if (name)
        h = fold_bytes(h, name, strlen(name));
    for (int i = 0; i < child_count; i++)
        h = hash_tref(h, children ? children[i] : NULL);
    return hash_folded32(h);
}

static uint32_t hash_synthetic_width_tref32(uint8_t kind, uint8_t native_width) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    h = fold_u64(h, kind);
    h = fold_u64(h, native_width);
    h = fold_u64(h, 0);
    h = fold_u64(h, 0);
    return hash_folded32(h);
}

static uint32_t hash_named_type_key32(const char *name, XrTypeRef **type_args, int type_arg_count) {
    if (!name || !name[0])
        return 0;
    if (type_args && type_arg_count > 0)
        return hash_synthetic_tref32(XR_TREF_GENERIC, name, type_args, type_arg_count);
    return hash_synthetic_tref32(XR_TREF_NAMED, name, NULL, 0);
}

static uint8_t class_field_int_semantic_kind(uint8_t native_width) {
    switch (native_width) {
        case XR_TREF_NW_I8:
            return XG_CLASS_FIELD_TYPE_I8;
        case XR_TREF_NW_U8:
            return XG_CLASS_FIELD_TYPE_U8;
        case XR_TREF_NW_I16:
            return XG_CLASS_FIELD_TYPE_I16;
        case XR_TREF_NW_U16:
            return XG_CLASS_FIELD_TYPE_U16;
        case XR_TREF_NW_I32:
            return XG_CLASS_FIELD_TYPE_I32;
        case XR_TREF_NW_U32:
            return XG_CLASS_FIELD_TYPE_U32;
        case XR_TREF_NW_U64:
            return XG_CLASS_FIELD_TYPE_U64;
        case XR_TREF_NW_ISIZE:
            return XG_CLASS_FIELD_TYPE_ISIZE;
        case XR_TREF_NW_USIZE:
            return XG_CLASS_FIELD_TYPE_USIZE;
        case XR_TREF_NW_I64:
        default:
            return XG_CLASS_FIELD_TYPE_I64;
    }
}

static uint8_t class_field_named_semantic_kind(const char *name) {
    if (!name)
        return XG_CLASS_FIELD_TYPE_DYNAMIC;
    if (strcmp(name, "Array") == 0 || strcmp(name, "Bytes") == 0 || strcmp(name, "ByteSpan") == 0 ||
        strcmp(name, "Span") == 0 || strcmp(name, "View") == 0)
        return XG_CLASS_FIELD_TYPE_ARRAY;
    if (strcmp(name, "Map") == 0)
        return XG_CLASS_FIELD_TYPE_MAP;
    if (strcmp(name, "Set") == 0)
        return XG_CLASS_FIELD_TYPE_SET;
    if (strcmp(name, "string") == 0 || strcmp(name, "String") == 0)
        return XG_CLASS_FIELD_TYPE_STRING;
    return XG_CLASS_FIELD_TYPE_DYNAMIC;
}

static bool class_field_target_is_builtin_name_id(uint32_t name_id) {
    static const char *const names[] = {
        "Array", "Bytes", "ByteSpan", "Span", "View", "Map", "Set", "string", "String",
    };
    if (name_id == 0)
        return false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (name_id == hash_name32(names[i]))
            return true;
    }
    return false;
}

static const XrTypeRef *class_field_target_type(const XrTypeRef *type) {
    const XrTypeRef *target = type;
    while (target && (target->kind == XR_TREF_OPTIONAL || target->kind == XR_TREF_FIXED_ARRAY) &&
           target->children && target->nchildren > 0)
        target = target->children[0];
    return target;
}

static void class_field_fill_type_facts(XgClassFieldSummary *row, const XrTypeRef *type) {
    const XrTypeRef *target;
    if (!row)
        return;
    row->semantic_kind = XG_CLASS_FIELD_TYPE_DYNAMIC;
    if (!type)
        return;
    row->native_width = type->native_width;
    target = class_field_target_type(type);
    if (target && (target->kind == XR_TREF_NAMED || target->kind == XR_TREF_GENERIC) &&
        target->name)
        row->target_name_id = hash_name32(target->name);
    if (type->children && type->nchildren > 0)
        row->element_type_key = hash_tref32(type->children[0]);
    if (type->kind == XR_TREF_GENERIC && type->name && strcmp(type->name, "Map") == 0) {
        row->key_type_key = row->element_type_key;
        if (type->nchildren > 1)
            row->value_type_key = hash_tref32(type->children[1]);
    }
    if (type->kind == XR_TREF_FIXED_ARRAY && type->fixed_length > 0)
        row->fixed_length = (uint32_t) type->fixed_length;
    switch ((XrTypeRefKind) type->kind) {
        case XR_TREF_INT:
            row->semantic_kind = XG_CLASS_FIELD_TYPE_I64;
            break;
        case XR_TREF_INT_WIDTH:
            row->semantic_kind = class_field_int_semantic_kind(type->native_width);
            break;
        case XR_TREF_FLOAT:
            row->semantic_kind = XG_CLASS_FIELD_TYPE_F64;
            break;
        case XR_TREF_FLOAT_WIDTH:
            row->semantic_kind = type->native_width == XR_TREF_NW_F32 ? XG_CLASS_FIELD_TYPE_F32
                                                                      : XG_CLASS_FIELD_TYPE_F64;
            break;
        case XR_TREF_BOOL:
            row->semantic_kind = XG_CLASS_FIELD_TYPE_BOOL;
            break;
        case XR_TREF_CHAR:
            row->semantic_kind = XG_CLASS_FIELD_TYPE_CHAR;
            break;
        case XR_TREF_STRING:
            row->semantic_kind = XG_CLASS_FIELD_TYPE_STRING;
            break;
        case XR_TREF_NAMED:
        case XR_TREF_GENERIC:
            row->semantic_kind = class_field_named_semantic_kind(type->name);
            break;
        case XR_TREF_FIXED_ARRAY:
            row->semantic_kind = XG_CLASS_FIELD_TYPE_FIXED_ARRAY;
            break;
        case XR_TREF_OPTIONAL:
            row->semantic_kind = XG_CLASS_FIELD_TYPE_OPTIONAL;
            row->flags |= XG_CLASS_FIELD_NULLABLE;
            break;
        case XR_TREF_UNION:
            row->semantic_kind = XG_CLASS_FIELD_TYPE_UNION;
            break;
        case XR_TREF_FUNCTION:
            row->semantic_kind = XG_CLASS_FIELD_TYPE_FUNCTION;
            break;
        case XR_TREF_TUPLE:
            row->semantic_kind = XG_CLASS_FIELD_TYPE_TUPLE;
            break;
        case XR_TREF_OBJECT:
            row->semantic_kind = XG_CLASS_FIELD_TYPE_OBJECT;
            break;
        case XR_TREF_TYPE_PARAM:
            row->semantic_kind = XG_CLASS_FIELD_TYPE_TYPE_PARAM;
            break;
        case XR_TREF_UNIT:
            row->semantic_kind = XG_CLASS_FIELD_TYPE_UNIT;
            break;
        case XR_TREF_NULL:
            row->semantic_kind = XG_CLASS_FIELD_TYPE_NULL;
            row->flags |= XG_CLASS_FIELD_NULLABLE;
            break;
        case XR_TREF_UNKNOWN:
        default:
            row->semantic_kind = XG_CLASS_FIELD_TYPE_DYNAMIC;
            break;
    }
}

static bool class_field_semantic_is_owned(uint8_t semantic_kind) {
    switch ((XgClassFieldTypeKind) semantic_kind) {
        case XG_CLASS_FIELD_TYPE_I8:
        case XG_CLASS_FIELD_TYPE_U8:
        case XG_CLASS_FIELD_TYPE_I16:
        case XG_CLASS_FIELD_TYPE_U16:
        case XG_CLASS_FIELD_TYPE_I32:
        case XG_CLASS_FIELD_TYPE_U32:
        case XG_CLASS_FIELD_TYPE_I64:
        case XG_CLASS_FIELD_TYPE_U64:
        case XG_CLASS_FIELD_TYPE_ISIZE:
        case XG_CLASS_FIELD_TYPE_USIZE:
        case XG_CLASS_FIELD_TYPE_F32:
        case XG_CLASS_FIELD_TYPE_F64:
        case XG_CLASS_FIELD_TYPE_BOOL:
        case XG_CLASS_FIELD_TYPE_CHAR:
        case XG_CLASS_FIELD_TYPE_UNIT:
        case XG_CLASS_FIELD_TYPE_NULL:
            return false;
        default:
            return true;
    }
}

static uint8_t class_field_semantic_kind_for_decl(uint8_t decl_kind) {
    switch ((XgDeclKind) decl_kind) {
        case XG_DECL_CLASS:
            return XG_CLASS_FIELD_TYPE_CLASS;
        case XG_DECL_STRUCT:
            return XG_CLASS_FIELD_TYPE_STRUCT;
        case XG_DECL_UNION:
            return XG_CLASS_FIELD_TYPE_FIXED_UNION;
        case XG_DECL_ENUM:
            return XG_CLASS_FIELD_TYPE_ENUM;
        default:
            return XG_CLASS_FIELD_TYPE_DYNAMIC;
    }
}

static uint32_t class_field_flags(const FieldDeclNode *field, uint8_t semantic_kind,
                                  uint32_t type_flags) {
    uint32_t flags = 0;
    if (!field)
        return flags;
    flags |= type_flags;
    if (field->is_static)
        flags |= XG_CLASS_FIELD_STATIC;
    if (field->is_const || field->is_final)
        flags |= XG_CLASS_FIELD_CONST;
    if (field->is_private)
        flags |= XG_CLASS_FIELD_PRIVATE;
    if (field->is_protected)
        flags |= XG_CLASS_FIELD_PROTECTED;
    if (class_field_semantic_is_owned(semantic_kind))
        flags |= XG_CLASS_FIELD_OWNED_REF;
    return flags;
}

static uint32_t hash_generic_inst_type_key(const char *name, XrTypeRef **type_args,
                                           int type_arg_count, uint8_t kind) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    h = fold_u64(h, kind);
    if (name)
        h = fold_bytes(h, name, strlen(name));
    h = fold_u64(h, (uint64_t) type_arg_count);
    for (int i = 0; i < type_arg_count; i++)
        h = hash_tref(h, type_args ? type_args[i] : NULL);
    return hash_folded32(h);
}

static uint32_t hash_generic_inst_name_type_key(const char *name, const char **type_arg_names,
                                                int type_arg_count, uint8_t kind) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    h = fold_u64(h, kind);
    if (name)
        h = fold_bytes(h, name, strlen(name));
    h = fold_u64(h, (uint64_t) type_arg_count);
    for (int i = 0; i < type_arg_count; i++) {
        if (type_arg_names && type_arg_names[i])
            h = fold_bytes(h, type_arg_names[i], strlen(type_arg_names[i]));
        else
            h = fold_u64(h, 0);
    }
    return hash_folded32(h);
}

static uint32_t hash_method_signature_parts(XrTypeRef **param_types, int param_count,
                                            XrTypeRef *return_type, bool is_static,
                                            bool is_constructor) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    h = fold_u64(h, (uint64_t) param_count);
    h = fold_u64(h, is_static ? 1 : 0);
    h = fold_u64(h, is_constructor ? 1 : 0);
    for (int i = 0; i < param_count; i++)
        h = hash_tref(h, param_types ? param_types[i] : NULL);
    h = hash_tref(h, return_type);
    return (uint32_t) (h ^ (h >> 32));
}

static uint32_t hash_method_signature(const MethodDeclNode *m) {
    if (!m)
        return 0;
    return hash_method_signature_parts(m->param_types, m->param_count, m->return_type, m->is_static,
                                       m->is_constructor);
}

static uint32_t hash_interface_method_signature(const InterfaceMethodNode *m) {
    if (!m)
        return 0;
    return hash_method_signature_parts(m->param_types, m->param_count, m->return_type, false,
                                       false);
}

static uint32_t hash_function_signature(const FunctionDeclNode *f) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    if (!f)
        return 0;
    h = fold_u64(h, (uint64_t) f->param_count);
    for (int i = 0; i < f->param_count; i++) {
        XrParamNode *p = f->params ? f->params[i] : NULL;
        if (p && p->name)
            h = fold_bytes(h, p->name, strlen(p->name));
        h = hash_tref(h, p ? p->type : NULL);
    }
    h = hash_tref(h, f->return_type);
    return (uint32_t) (h ^ (h >> 32));
}

static uint64_t hash_ast_shape(const AstNode *node, uint64_t h) {
    if (!node)
        return fold_u64(h, 0);
    h = fold_u64(h, (uint64_t) node->type);
    h = fold_u64(h, (uint64_t) node->line);
    h = fold_u64(h, (uint64_t) node->column);
    switch (node->type) {
        case AST_VARIABLE:
            if (node->as.variable.name)
                h = fold_bytes(h, node->as.variable.name, strlen(node->as.variable.name));
            break;
        case AST_CALL_EXPR:
            h = hash_ast_shape(node->as.call_expr.callee, h);
            h = fold_u64(h, (uint64_t) node->as.call_expr.type_arg_count);
            for (int i = 0; i < node->as.call_expr.type_arg_count; i++)
                h = hash_tref(h, node->as.call_expr.type_args ? node->as.call_expr.type_args[i]
                                                              : NULL);
            h = fold_u64(h, (uint64_t) node->as.call_expr.arg_count);
            for (int i = 0; i < node->as.call_expr.arg_count; i++)
                h = hash_ast_shape(node->as.call_expr.arguments[i], h);
            break;
        case AST_NEW_EXPR:
            if (node->as.new_expr.class_name)
                h = fold_bytes(h, node->as.new_expr.class_name,
                               strlen(node->as.new_expr.class_name));
            h = fold_u64(h, (uint64_t) node->as.new_expr.type_arg_count);
            for (int i = 0; i < node->as.new_expr.type_arg_count; i++)
                h = hash_tref(h,
                              node->as.new_expr.type_args ? node->as.new_expr.type_args[i] : NULL);
            h = fold_u64(h, (uint64_t) node->as.new_expr.arg_count);
            for (int i = 0; i < node->as.new_expr.arg_count; i++)
                h = hash_ast_shape(node->as.new_expr.arguments[i], h);
            break;
        case AST_STRUCT_LITERAL:
            if (node->as.struct_literal.struct_name)
                h = fold_bytes(h, node->as.struct_literal.struct_name,
                               strlen(node->as.struct_literal.struct_name));
            h = fold_u64(h, (uint64_t) node->as.struct_literal.type_arg_count);
            for (int i = 0; i < node->as.struct_literal.type_arg_count; i++)
                h = hash_tref(h, node->as.struct_literal.type_args
                                     ? node->as.struct_literal.type_args[i]
                                     : NULL);
            h = fold_u64(h, (uint64_t) node->as.struct_literal.field_count);
            for (int i = 0; i < node->as.struct_literal.field_count; i++)
                h = hash_ast_shape(node->as.struct_literal.field_values
                                       ? node->as.struct_literal.field_values[i]
                                       : NULL,
                                   h);
            break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++)
                h = hash_ast_shape(node->as.block.statements[i], h);
            break;
        case AST_RETURN_STMT:
            h = fold_u64(h, (uint64_t) node->as.return_stmt.value_count);
            for (int i = 0; i < node->as.return_stmt.value_count; i++)
                h = hash_ast_shape(node->as.return_stmt.values[i], h);
            break;
        case AST_EXPR_STMT:
            h = hash_ast_shape(node->as.expr_stmt, h);
            break;
        default:
            break;
    }
    return h;
}

static bool producer_reserve_classes(XgProducer *p, uint32_t needed) {
    uint32_t new_cap;
    XgClassNameRow *rows;
    if (p->class_cap >= needed)
        return true;
    new_cap = p->class_cap < 8 ? 8 : p->class_cap;
    while (new_cap < needed)
        new_cap *= 2;
    rows = (XgClassNameRow *) xr_realloc(p->classes, (size_t) new_cap * sizeof(*rows));
    if (!rows)
        return false;
    p->classes = rows;
    p->class_cap = new_cap;
    return true;
}

static bool producer_reserve_funcs(XgProducer *p, uint32_t needed) {
    uint32_t new_cap;
    XgFuncNameRow *rows;
    if (p->func_cap >= needed)
        return true;
    new_cap = p->func_cap < 8 ? 8 : p->func_cap;
    while (new_cap < needed)
        new_cap *= 2;
    rows = (XgFuncNameRow *) xr_realloc(p->funcs, (size_t) new_cap * sizeof(*rows));
    if (!rows)
        return false;
    p->funcs = rows;
    p->func_cap = new_cap;
    return true;
}

static XgFuncId producer_next_func_id(XgProducer *p) {
    XgFuncId id = p->next_func_id;
    if (id == XG_NO_ID)
        id = 1;
    p->next_func_id = id + 1;
    return id;
}

static bool producer_register_func(XgProducer *p, const char *name, XgFuncId func_id,
                                   XgDeclId decl_id, uint32_t decl_flags) {
    if (!name || func_id == XG_NO_ID)
        return true;
    if (!producer_reserve_funcs(p, p->nfuncs + 1))
        return false;
    p->funcs[p->nfuncs].name = name;
    p->funcs[p->nfuncs].func_id = func_id;
    p->funcs[p->nfuncs].decl_id = decl_id;
    p->funcs[p->nfuncs].decl_flags = decl_flags;
    p->nfuncs++;
    return true;
}

static XgFuncNameRow *producer_lookup_func_row(const XgProducer *p, const char *name) {
    if (!p || !name)
        return NULL;
    for (uint32_t i = 0; i < p->nfuncs; i++) {
        if (p->funcs[i].name && strcmp(p->funcs[i].name, name) == 0)
            return &p->funcs[i];
    }
    return NULL;
}

static bool producer_reserve_stdlib_imports(XgProducer *p, uint32_t needed) {
    uint32_t new_cap;
    XgStdlibImportRow *rows;
    if (p->stdlib_import_cap >= needed)
        return true;
    new_cap = p->stdlib_import_cap < 8 ? 8 : p->stdlib_import_cap;
    while (new_cap < needed)
        new_cap *= 2;
    rows = (XgStdlibImportRow *) xr_realloc(p->stdlib_imports, (size_t) new_cap * sizeof(*rows));
    if (!rows)
        return false;
    p->stdlib_imports = rows;
    p->stdlib_import_cap = new_cap;
    return true;
}

static bool producer_register_stdlib_import(XgProducer *p, XgModuleId module_id,
                                            const char *local_name, const char *module_name,
                                            const char *member_name) {
    XgStdlibImportRow *row;
    if (!local_name || !local_name[0] || !module_name || !module_name[0])
        return true;
    if (!producer_reserve_stdlib_imports(p, p->nstdlib_imports + 1))
        return false;
    row = &p->stdlib_imports[p->nstdlib_imports++];
    row->module_id = module_id;
    row->local_name = local_name;
    row->module_name = module_name;
    row->member_name = member_name;
    return true;
}

static const XgStdlibImportRow *
producer_lookup_stdlib_import(const XgProducer *p, XgModuleId module_id, const char *local_name) {
    if (!p || !local_name)
        return NULL;
    for (uint32_t i = p->nstdlib_imports; i > 0; i--) {
        const XgStdlibImportRow *row = &p->stdlib_imports[i - 1];
        if (row->module_id == module_id && row->local_name &&
            strcmp(row->local_name, local_name) == 0)
            return row;
    }
    return NULL;
}

static bool producer_reserve_interfaces(XgProducer *p, uint32_t needed) {
    uint32_t new_cap;
    XgInterfaceNameRow *rows;
    if (p->interface_cap >= needed)
        return true;
    new_cap = p->interface_cap < 8 ? 8 : p->interface_cap;
    while (new_cap < needed)
        new_cap *= 2;
    rows = (XgInterfaceNameRow *) xr_realloc(p->interfaces, (size_t) new_cap * sizeof(*rows));
    if (!rows)
        return false;
    p->interfaces = rows;
    p->interface_cap = new_cap;
    return true;
}

static XgInterfaceNameRow *producer_lookup_interface_row(const XgProducer *p, const char *name) {
    if (!p || !name)
        return NULL;
    for (uint32_t i = 0; i < p->ninterfaces; i++) {
        if (p->interfaces[i].name && strcmp(p->interfaces[i].name, name) == 0)
            return &p->interfaces[i];
    }
    return NULL;
}

static XgInterfaceNameRow *producer_lookup_interface_row_by_id(const XgProducer *p,
                                                               XgInterfaceId interface_id) {
    if (!p || interface_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < p->ninterfaces; i++) {
        if (p->interfaces[i].interface_id == interface_id)
            return &p->interfaces[i];
    }
    return NULL;
}

static bool producer_register_interface(XgProducer *p, XgModuleId module_id, const char *name,
                                        const InterfaceDeclNode *decl) {
    if (!name)
        return true;
    for (uint32_t i = 0; i < p->ninterfaces; i++) {
        XgInterfaceNameRow *existing = &p->interfaces[i];
        if (existing->module_id == module_id && existing->name &&
            strcmp(existing->name, name) == 0) {
            existing->decl = decl;
            return true;
        }
    }
    if (!producer_reserve_interfaces(p, p->ninterfaces + 1))
        return false;
    p->interfaces[p->ninterfaces].module_id = module_id;
    p->interfaces[p->ninterfaces].name = name;
    p->interfaces[p->ninterfaces].interface_id = (XgInterfaceId) hash_name32(name);
    p->interfaces[p->ninterfaces].decl = decl;
    p->ninterfaces++;
    return true;
}

static XgInterfaceId producer_lookup_interface(const XgProducer *p, const char *name) {
    XgInterfaceNameRow *row = producer_lookup_interface_row(p, name);
    return row ? row->interface_id : XG_NO_ID;
}

static XgInterfaceId producer_lookup_interface_from_tref(const XgProducer *p, const XrTypeRef *t) {
    return producer_lookup_interface(p, xr_tref_head_name(t));
}

static uint32_t producer_find_interface_method_signature_depth(XgProducer *p,
                                                               XgInterfaceId interface_id,
                                                               uint32_t name_id, uint32_t depth) {
    XgInterfaceNameRow *row;
    const InterfaceDeclNode *iface;
    if (!p || interface_id == XG_NO_ID || name_id == 0 || depth > 64)
        return 0;
    row = producer_lookup_interface_row_by_id(p, interface_id);
    iface = row ? row->decl : NULL;
    if (!iface)
        return 0;
    for (int i = 0; i < iface->method_count; i++) {
        const AstNode *method_node = iface->methods ? iface->methods[i] : NULL;
        const InterfaceMethodNode *method;
        if (!method_node || method_node->type != AST_INTERFACE_METHOD)
            continue;
        method = &method_node->as.interface_method;
        if (hash_name32(method->name) == name_id)
            return hash_interface_method_signature(method);
    }
    for (int i = 0; i < iface->extends_count; i++) {
        XgInterfaceId parent_id =
            producer_lookup_interface_from_tref(p, iface->extends ? iface->extends[i] : NULL);
        uint32_t signature =
            producer_find_interface_method_signature_depth(p, parent_id, name_id, depth + 1);
        if (signature != 0)
            return signature;
    }
    return 0;
}

static uint32_t producer_find_interface_method_signature(XgProducer *p, XgInterfaceId interface_id,
                                                         uint32_t name_id) {
    return producer_find_interface_method_signature_depth(p, interface_id, name_id, 0);
}

static const XgInterfaceMethodSummary *
producer_find_interface_method_summary_depth(const XgProducer *p, XgInterfaceId interface_id,
                                             uint32_t name_id, uint32_t depth) {
    if (!p || !p->evidence || interface_id == XG_NO_ID || name_id == 0)
        return NULL;
    if (depth > 64)
        return NULL;
    for (uint32_t i = 0; i < p->evidence->ninterface_methods; i++) {
        const XgInterfaceMethodSummary *method = &p->evidence->interface_methods[i];
        if (method->owner_interface_id == interface_id && method->name_id == name_id)
            return method;
    }
    for (uint32_t i = 0; i < p->evidence->ninterface_extends; i++) {
        const XgInterfaceExtendsSummary *edge = &p->evidence->interface_extends[i];
        const XgInterfaceMethodSummary *method;
        if (edge->child_interface_id != interface_id)
            continue;
        method = producer_find_interface_method_summary_depth(p, edge->parent_interface_id, name_id,
                                                              depth + 1);
        if (method)
            return method;
    }
    return NULL;
}

static const XgInterfaceMethodSummary *
producer_find_interface_method_summary(const XgProducer *p, XgInterfaceId interface_id,
                                       uint32_t name_id) {
    return producer_find_interface_method_summary_depth(p, interface_id, name_id, 0);
}

static bool producer_reserve_bodies(XgProducer *p, uint32_t needed) {
    uint32_t new_cap;
    XgPendingBody *rows;
    if (p->body_cap >= needed)
        return true;
    new_cap = p->body_cap < 8 ? 8 : p->body_cap;
    while (new_cap < needed)
        new_cap *= 2;
    rows = (XgPendingBody *) xr_realloc(p->bodies, (size_t) new_cap * sizeof(*rows));
    if (!rows)
        return false;
    p->bodies = rows;
    p->body_cap = new_cap;
    return true;
}

static bool producer_enqueue_body(XgProducer *p, XgFuncId func_id, XgModuleId module_id,
                                  XgDeclId owner_decl_id, XgClassId current_class_id,
                                  XgMethodId owner_method_id, uint32_t name_id,
                                  uint32_t signature_key, uint32_t source_node_id,
                                  uint32_t source_span_id, uint8_t kind, const AstNode *body,
                                  const MethodDeclNode *method, const FunctionDeclNode *function) {
    XgPendingBody *row;
    if (!body)
        return true;
    if (!producer_reserve_bodies(p, p->nbodies + 1))
        return false;
    row = &p->bodies[p->nbodies++];
    memset(row, 0, sizeof(*row));
    row->func_id = func_id;
    row->module_id = module_id;
    row->owner_decl_id = owner_decl_id;
    row->current_class_id = current_class_id;
    row->owner_method_id = owner_method_id;
    row->name_id = name_id;
    row->signature_key = signature_key;
    row->source_node_id = source_node_id;
    row->source_span_id = source_span_id;
    row->kind = kind;
    row->body = body;
    row->method = method;
    row->function = function;
    return true;
}

static bool producer_snapshot_body_captures(XgPendingBody *row, const XgBodyCollect *bc) {
    if (!row || !bc)
        return true;
    if (bc->nlocals > 0) {
        row->captured_locals =
            (XgLocalType *) xr_malloc((size_t) bc->nlocals * sizeof(*row->captured_locals));
        if (!row->captured_locals)
            return false;
        memcpy(row->captured_locals, bc->locals,
               (size_t) bc->nlocals * sizeof(*row->captured_locals));
        row->captured_local_count = bc->nlocals;
    }
    if (bc->nname_locals > 0) {
        row->captured_name_locals = (XgLocalName *) xr_malloc((size_t) bc->nname_locals *
                                                              sizeof(*row->captured_name_locals));
        if (!row->captured_name_locals) {
            xr_free(row->captured_locals);
            row->captured_locals = NULL;
            row->captured_local_count = 0;
            return false;
        }
        memcpy(row->captured_name_locals, bc->name_locals,
               (size_t) bc->nname_locals * sizeof(*row->captured_name_locals));
        row->captured_name_local_count = bc->nname_locals;
    }
    return true;
}

static void producer_free_bodies(XgProducer *p) {
    if (!p)
        return;
    for (uint32_t i = 0; i < p->nbodies; i++) {
        xr_free(p->bodies[i].captured_locals);
        xr_free(p->bodies[i].captured_name_locals);
    }
    xr_free(p->bodies);
    p->bodies = NULL;
    p->nbodies = 0;
    p->body_cap = 0;
}

static bool producer_register_class(XgProducer *p, XgModuleId module_id, const char *name,
                                    const char *super_name, XgClassId class_id,
                                    uint32_t summary_index) {
    if (!producer_reserve_classes(p, p->nclasses + 1))
        return false;
    p->classes[p->nclasses].module_id = module_id;
    p->classes[p->nclasses].name = name;
    p->classes[p->nclasses].super_name = super_name;
    p->classes[p->nclasses].class_id = class_id;
    p->classes[p->nclasses].summary_index = summary_index;
    p->nclasses++;
    return true;
}

static XgClassNameRow *producer_lookup_class_row(const XgProducer *p, const char *name) {
    if (!p || !name)
        return NULL;
    for (uint32_t i = 0; i < p->nclasses; i++) {
        if (p->classes[i].name && strcmp(p->classes[i].name, name) == 0)
            return &p->classes[i];
    }
    return NULL;
}

static XgClassNameRow *producer_lookup_class_row_by_id(const XgProducer *p, XgClassId class_id) {
    if (!p || class_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < p->nclasses; i++) {
        if (p->classes[i].class_id == class_id)
            return &p->classes[i];
    }
    return NULL;
}

static XgClassId producer_lookup_class(const XgProducer *p, const char *name) {
    XgClassNameRow *row = producer_lookup_class_row(p, name);
    return row ? row->class_id : XG_NO_ID;
}

static XgClassNameRow *producer_lookup_class_row_scoped(const XgProducer *p, XgModuleId module_id,
                                                        uint32_t name_id,
                                                        bool allow_global_unique) {
    XgClassNameRow *match = NULL;
    if (!p || name_id == 0)
        return NULL;
    for (uint32_t i = 0; i < p->nclasses; i++) {
        XgClassNameRow *row = &p->classes[i];
        if (row->module_id != module_id || hash_name32(row->name) != name_id)
            continue;
        if (match)
            return NULL;
        match = row;
    }
    if (match || !allow_global_unique)
        return match;
    for (uint32_t i = 0; i < p->nclasses; i++) {
        XgClassNameRow *row = &p->classes[i];
        if (hash_name32(row->name) != name_id)
            continue;
        if (match)
            return NULL;
        match = row;
    }
    return match;
}

static XgInterfaceNameRow *producer_lookup_interface_row_scoped(const XgProducer *p,
                                                                XgModuleId module_id,
                                                                uint32_t name_id,
                                                                bool allow_global_unique) {
    XgInterfaceNameRow *match = NULL;
    if (!p || name_id == 0)
        return NULL;
    for (uint32_t i = 0; i < p->ninterfaces; i++) {
        XgInterfaceNameRow *row = &p->interfaces[i];
        if (row->module_id != module_id || hash_name32(row->name) != name_id)
            continue;
        if (match)
            return NULL;
        match = row;
    }
    if (match || !allow_global_unique)
        return match;
    for (uint32_t i = 0; i < p->ninterfaces; i++) {
        XgInterfaceNameRow *row = &p->interfaces[i];
        if (hash_name32(row->name) != name_id)
            continue;
        if (match)
            return NULL;
        match = row;
    }
    return match;
}

static XgDeclId producer_lookup_class_decl_id(const XgProducer *p, XgClassId class_id) {
    XgClassNameRow *row = producer_lookup_class_row_by_id(p, class_id);
    if (!p || !p->evidence || !row || row->summary_index >= p->evidence->nclasses)
        return XG_NO_ID;
    return p->evidence->classes[row->summary_index].decl_id;
}

static XgClassId producer_lookup_class_from_tref(const XgProducer *p, const XrTypeRef *t) {
    return producer_lookup_class(p, xr_tref_head_name(t));
}

static XgMethodSummary *producer_find_class_method(XgGlobalEvidence *ev, const XgClassSummary *cls,
                                                   uint32_t name_id, uint32_t signature_key) {
    if (!ev || !cls || cls->method_start == 0)
        return NULL;
    for (uint32_t i = 0; i < cls->method_count; i++) {
        uint32_t method_index = cls->method_start - 1 + i;
        XgMethodSummary *method = method_index < ev->nmethods ? &ev->methods[method_index] : NULL;
        if (method && method->name_id == name_id && method->signature_key == signature_key &&
            (method->flags & XG_METHOD_STATIC) == 0 && (method->flags & XG_METHOD_CONSTRUCTOR) == 0)
            return method;
    }
    return NULL;
}

static XgMethodSummary *producer_find_class_method_by_name(XgGlobalEvidence *ev,
                                                           const XgClassSummary *cls,
                                                           uint32_t name_id,
                                                           bool allow_constructor) {
    if (!ev || !cls || cls->method_start == 0 || name_id == 0)
        return NULL;
    for (uint32_t i = 0; i < cls->method_count; i++) {
        uint32_t method_index = cls->method_start - 1 + i;
        XgMethodSummary *method = method_index < ev->nmethods ? &ev->methods[method_index] : NULL;
        bool is_constructor = method && (method->flags & XG_METHOD_CONSTRUCTOR) != 0;
        if (method && method->name_id == name_id && (method->flags & XG_METHOD_STATIC) == 0 &&
            (allow_constructor || !is_constructor))
            return method;
    }
    return NULL;
}

static XgMethodSummary *producer_find_parent_method(XgProducer *p, const XgClassSummary *cls,
                                                    uint32_t name_id, uint32_t signature_key) {
    XgClassId parent_id;
    if (!p || !cls)
        return NULL;
    parent_id = cls->parent_class_id;
    while (parent_id != XG_NO_ID) {
        XgClassNameRow *parent_row = producer_lookup_class_row_by_id(p, parent_id);
        XgClassSummary *parent_summary;
        if (!parent_row || parent_row->summary_index >= p->evidence->nclasses)
            return NULL;
        parent_summary = &p->evidence->classes[parent_row->summary_index];
        XgMethodSummary *method =
            producer_find_class_method(p->evidence, parent_summary, name_id, signature_key);
        if (method)
            return method;
        parent_id = parent_summary->parent_class_id;
    }
    return NULL;
}

static XgMethodId producer_rederive_method_root(XgProducer *p, const XgClassSummary *cls,
                                                const XgMethodSummary *method) {
    const XgClassSummary *current = cls;
    XgMethodId root = method ? method->method_id : XG_NO_ID;
    uint32_t depth = 0;

    if (!p || !cls || !method)
        return XG_NO_ID;
    while (current && current->parent_class_id != XG_NO_ID && depth++ < 64) {
        XgClassNameRow *parent_row = producer_lookup_class_row_by_id(p, current->parent_class_id);
        XgClassSummary *parent_summary;
        XgMethodSummary *parent_method;
        if (!parent_row || parent_row->summary_index >= p->evidence->nclasses)
            break;
        parent_summary = &p->evidence->classes[parent_row->summary_index];
        parent_method = producer_find_class_method(p->evidence, parent_summary, method->name_id,
                                                   method->signature_key);
        if (parent_method)
            root = parent_method->method_id;
        current = parent_summary;
    }
    return root;
}

static uint32_t producer_rederive_method_override_depth(XgProducer *p, const XgClassSummary *cls,
                                                        const XgMethodSummary *method) {
    const XgClassSummary *current = cls;
    uint32_t chain_depth = 0;
    uint32_t scan_depth = 0;

    if (!p || !cls || !method)
        return 0;
    while (current && current->parent_class_id != XG_NO_ID && scan_depth++ < 64) {
        XgClassNameRow *parent_row = producer_lookup_class_row_by_id(p, current->parent_class_id);
        XgClassSummary *parent_summary;
        XgMethodSummary *parent_method;
        if (!parent_row || parent_row->summary_index >= p->evidence->nclasses)
            break;
        parent_summary = &p->evidence->classes[parent_row->summary_index];
        parent_method = producer_find_class_method(p->evidence, parent_summary, method->name_id,
                                                   method->signature_key);
        if (parent_method)
            chain_depth++;
        current = parent_summary;
    }
    return chain_depth;
}

static XgMethodSummary *producer_find_method_by_name_in_hierarchy(XgProducer *p, XgClassId class_id,
                                                                  uint32_t name_id,
                                                                  bool allow_constructor) {
    XgClassNameRow *row = producer_lookup_class_row_by_id(p, class_id);
    uint32_t depth = 0;
    while (row && depth++ < 64) {
        XgClassSummary *summary;
        XgMethodSummary *method;
        if (row->summary_index >= p->evidence->nclasses)
            return NULL;
        summary = &p->evidence->classes[row->summary_index];
        method =
            producer_find_class_method_by_name(p->evidence, summary, name_id, allow_constructor);
        if (method)
            return method;
        if (summary->parent_class_id == XG_NO_ID)
            break;
        row = producer_lookup_class_row_by_id(p, summary->parent_class_id);
    }
    return NULL;
}

static bool producer_finalize_class_field_slots_rec(XgProducer *p, uint32_t class_index,
                                                    uint8_t *state, uint32_t *instance_counts) {
    XgClassSummary *summary;
    uint32_t prefix_count = 0;
    uint32_t own_instance_count = 0;
    uint32_t start;
    if (!p || !p->evidence || class_index >= p->evidence->nclasses || !state || !instance_counts)
        return false;
    if (state[class_index] == 2)
        return true;
    if (state[class_index] == 1)
        return false;
    state[class_index] = 1;
    summary = &p->evidence->classes[class_index];
    if (summary->parent_class_id != XG_NO_ID) {
        XgClassNameRow *parent_row = producer_lookup_class_row_by_id(p, summary->parent_class_id);
        if (!parent_row || parent_row->summary_index >= p->evidence->nclasses ||
            !producer_finalize_class_field_slots_rec(p, parent_row->summary_index, state,
                                                     instance_counts))
            return false;
        prefix_count = instance_counts[parent_row->summary_index];
    }
    if (summary->field_count == 0) {
        if (summary->field_start != 0)
            return false;
    } else {
        if (summary->field_start == 0)
            return false;
        start = summary->field_start - 1;
        if (start >= p->evidence->nclass_fields ||
            summary->field_count > p->evidence->nclass_fields - start)
            return false;
        for (uint32_t i = 0; i < summary->field_count; i++) {
            XgClassFieldSummary *field = &p->evidence->class_fields[start + i];
            if (field->owner_class_id != summary->class_id)
                return false;
            if ((field->flags & XG_CLASS_FIELD_STATIC) != 0) {
                field->instance_slot = UINT32_MAX;
                continue;
            }
            field->instance_slot = prefix_count + own_instance_count++;
        }
    }
    if (prefix_count > UINT32_MAX - own_instance_count)
        return false;
    instance_counts[class_index] = prefix_count + own_instance_count;
    state[class_index] = 2;
    return true;
}

static bool producer_finalize_class_field_slots(XgProducer *p) {
    uint8_t *state;
    uint32_t *instance_counts;
    bool ok = true;
    if (!p || !p->evidence)
        return false;
    if (p->evidence->nclasses == 0)
        return true;
    state = (uint8_t *) xr_calloc(p->evidence->nclasses, sizeof(*state));
    instance_counts = (uint32_t *) xr_calloc(p->evidence->nclasses, sizeof(*instance_counts));
    if (!state || !instance_counts) {
        xr_free(state);
        xr_free(instance_counts);
        return false;
    }
    for (uint32_t i = 0; i < p->evidence->nclasses; i++) {
        if (!producer_finalize_class_field_slots_rec(p, i, state, instance_counts)) {
            ok = false;
            break;
        }
    }
    xr_free(state);
    xr_free(instance_counts);
    return ok;
}

static bool producer_finalize_class_field_types(XgProducer *p) {
    if (!p || !p->evidence)
        return false;
    for (uint32_t i = 0; i < p->evidence->nclass_fields; i++) {
        XgClassFieldSummary *field = &p->evidence->class_fields[i];
        XgClassNameRow *class_row = NULL;
        XgInterfaceNameRow *interface_row = NULL;
        const XgDeclSummary *enum_decl = NULL;
        bool enum_ambiguous = false;
        if (field->target_name_id != 0) {
            bool allow_global_unique =
                !class_field_target_is_builtin_name_id(field->target_name_id);
            class_row = producer_lookup_class_row_scoped(p, field->module_id, field->target_name_id,
                                                         allow_global_unique);
            interface_row = producer_lookup_interface_row_scoped(
                p, field->module_id, field->target_name_id, allow_global_unique);
            for (uint32_t j = 0; j < p->evidence->ndecls; j++) {
                const XgDeclSummary *decl = &p->evidence->decls[j];
                if (decl->module_id != field->module_id || decl->kind != XG_DECL_ENUM ||
                    decl->name_id != field->target_name_id)
                    continue;
                if (enum_decl) {
                    enum_ambiguous = true;
                    break;
                }
                enum_decl = decl;
            }
            if (enum_ambiguous)
                enum_decl = NULL;
            if (!enum_decl && !enum_ambiguous && allow_global_unique) {
                for (uint32_t j = 0; j < p->evidence->ndecls; j++) {
                    const XgDeclSummary *decl = &p->evidence->decls[j];
                    if (decl->kind != XG_DECL_ENUM || decl->name_id != field->target_name_id)
                        continue;
                    if (enum_decl) {
                        enum_decl = NULL;
                        break;
                    }
                    enum_decl = decl;
                }
            }
            if ((class_row != NULL) + (interface_row != NULL) + (enum_decl != NULL) > 1) {
                class_row = NULL;
                interface_row = NULL;
                enum_decl = NULL;
            }
        }
        if (class_row) {
            const XgClassSummary *target;
            if (class_row->summary_index >= p->evidence->nclasses)
                return false;
            target = &p->evidence->classes[class_row->summary_index];
            field->target_class_id = class_row->class_id;
            if (field->semantic_kind == XG_CLASS_FIELD_TYPE_DYNAMIC ||
                field->semantic_kind == XG_CLASS_FIELD_TYPE_ARRAY ||
                field->semantic_kind == XG_CLASS_FIELD_TYPE_MAP ||
                field->semantic_kind == XG_CLASS_FIELD_TYPE_SET ||
                field->semantic_kind == XG_CLASS_FIELD_TYPE_STRING)
                field->semantic_kind = class_field_semantic_kind_for_decl(target->decl_kind);
        } else if (interface_row) {
            field->target_interface_id = interface_row->interface_id;
            if (field->semantic_kind == XG_CLASS_FIELD_TYPE_DYNAMIC ||
                field->semantic_kind == XG_CLASS_FIELD_TYPE_ARRAY ||
                field->semantic_kind == XG_CLASS_FIELD_TYPE_MAP ||
                field->semantic_kind == XG_CLASS_FIELD_TYPE_SET ||
                field->semantic_kind == XG_CLASS_FIELD_TYPE_STRING)
                field->semantic_kind = XG_CLASS_FIELD_TYPE_INTERFACE;
        } else if (enum_decl) {
            if (field->semantic_kind == XG_CLASS_FIELD_TYPE_DYNAMIC ||
                field->semantic_kind == XG_CLASS_FIELD_TYPE_ARRAY ||
                field->semantic_kind == XG_CLASS_FIELD_TYPE_MAP ||
                field->semantic_kind == XG_CLASS_FIELD_TYPE_SET ||
                field->semantic_kind == XG_CLASS_FIELD_TYPE_STRING)
                field->semantic_kind = XG_CLASS_FIELD_TYPE_ENUM;
        }
        field->flags &= ~XG_CLASS_FIELD_OWNED_REF;
        if (class_field_semantic_is_owned(field->semantic_kind))
            field->flags |= XG_CLASS_FIELD_OWNED_REF;
    }
    return true;
}

static bool producer_finalize_class_graph(XgProducer *p) {
    if (!p || !p->evidence)
        return false;

    for (uint32_t i = 0; i < p->nclasses; i++) {
        XgClassNameRow *row = &p->classes[i];
        XgClassSummary *summary;
        if (row->summary_index >= p->evidence->nclasses)
            continue;
        summary = &p->evidence->classes[row->summary_index];
        XgClassNameRow *parent_row =
            producer_lookup_class_row_scoped(p, row->module_id, hash_name32(row->super_name), true);
        summary->parent_class_id = parent_row ? parent_row->class_id : XG_NO_ID;
        if (summary->parent_class_id != XG_NO_ID) {
            XgClassNameRow *resolved_parent =
                producer_lookup_class_row_by_id(p, summary->parent_class_id);
            if (resolved_parent && resolved_parent->summary_index < p->evidence->nclasses) {
                p->evidence->classes[resolved_parent->summary_index].flags |= XG_CLASS_HAS_SUBCLASS;
            }
        }
    }

    for (uint32_t i = 0; i < p->evidence->nclasses; i++) {
        XgClassSummary *summary = &p->evidence->classes[i];
        if ((summary->flags & XG_CLASS_HAS_SUBCLASS) == 0)
            summary->flags |= XG_CLASS_INFERRED_FINAL;
        else
            summary->flags &= ~XG_CLASS_INFERRED_FINAL;
    }

    if (!producer_finalize_class_field_types(p) || !producer_finalize_class_field_slots(p))
        return false;

    for (uint32_t i = 0; i < p->evidence->nclasses; i++) {
        XgClassSummary *summary = &p->evidence->classes[i];
        if (summary->method_start == 0 || summary->parent_class_id == XG_NO_ID)
            continue;
        for (uint32_t m = 0; m < summary->method_count; m++) {
            uint32_t method_index = summary->method_start - 1 + m;
            XgMethodSummary *method =
                method_index < p->evidence->nmethods ? &p->evidence->methods[method_index] : NULL;
            XgMethodSummary *parent_method;
            if (!method || (method->flags & XG_METHOD_STATIC) ||
                (method->flags & XG_METHOD_CONSTRUCTOR))
                continue;
            method->root_method_id = producer_rederive_method_root(p, summary, method);
            method->override_depth = producer_rederive_method_override_depth(p, summary, method);
            parent_method =
                producer_find_parent_method(p, summary, method->name_id, method->signature_key);
            if (!parent_method)
                continue;
            method->override_of = parent_method->method_id;
            parent_method->flags |= XG_METHOD_OVERRIDDEN;
        }
    }
    return true;
}

static bool body_reserve_locals(XgBodyCollect *bc, uint32_t needed) {
    uint32_t new_cap;
    XgLocalType *rows;
    if (bc->local_cap >= needed)
        return true;
    new_cap = bc->local_cap < 8 ? 8 : bc->local_cap;
    while (new_cap < needed)
        new_cap *= 2;
    rows = (XgLocalType *) xr_realloc(bc->locals, (size_t) new_cap * sizeof(*rows));
    if (!rows)
        return false;
    bc->locals = rows;
    bc->local_cap = new_cap;
    return true;
}

static bool body_reserve_name_locals(XgBodyCollect *bc, uint32_t needed) {
    uint32_t new_cap;
    XgLocalName *rows;
    if (bc->name_local_cap >= needed)
        return true;
    new_cap = bc->name_local_cap < 8 ? 8 : bc->name_local_cap;
    while (new_cap < needed)
        new_cap *= 2;
    rows = (XgLocalName *) xr_realloc(bc->name_locals, (size_t) new_cap * sizeof(*rows));
    if (!rows)
        return false;
    bc->name_locals = rows;
    bc->name_local_cap = new_cap;
    return true;
}

static bool body_push_name_local(XgBodyCollect *bc, const char *name, uint32_t symbol_id) {
    XgLocalName *row;
    if (!bc || !name || !name[0])
        return true;
    if (symbol_id != 0) {
        for (uint32_t i = bc->nname_locals; i > 0; i--) {
            if (bc->name_locals[i - 1].symbol_id == symbol_id)
                return true;
        }
    }
    if (!body_reserve_name_locals(bc, bc->nname_locals + 1))
        return false;
    row = &bc->name_locals[bc->nname_locals++];
    row->name = name;
    row->symbol_id = symbol_id;
    return true;
}

static bool body_push_local(XgBodyCollect *bc, const char *name, uint32_t symbol_id,
                            XgClassId class_id, XgInterfaceId interface_id, uint32_t type_key,
                            bool inferred) {
    XgLocalType *row;
    if (!bc)
        return true;
    if (!body_push_name_local(bc, name, symbol_id))
        return false;
    if (!name || (class_id == XG_NO_ID && interface_id == XG_NO_ID && type_key == 0))
        return true;
    if (!body_reserve_locals(bc, bc->nlocals + 1))
        return false;
    row = &bc->locals[bc->nlocals++];
    row->name = name;
    row->class_id = class_id;
    row->interface_id = interface_id;
    row->type_key = type_key;
    row->json_shape_id = XG_NO_ID;
    row->json_shape_literal = NULL;
    row->record_shape_id = XG_NO_ID;
    row->record_shape_literal = NULL;
    row->map_shape_id = XG_NO_ID;
    row->map_container_kind = 0;
    row->map_receiver_type_key = 0;
    row->map_key_type_key = 0;
    row->map_value_type_key = 0;
    row->sequence_kind = 0;
    row->sequence_elem_type_key = 0;
    row->sequence_elem_map_container_kind = 0;
    row->sequence_elem_map_key_type_key = 0;
    row->sequence_elem_map_value_type_key = 0;
    row->inferred = inferred;
    return true;
}

static XgLocalType *body_find_local(XgBodyCollect *bc, const char *name) {
    if (!bc || !name)
        return NULL;
    for (uint32_t i = bc->nlocals; i > 0; i--) {
        XgLocalType *row = &bc->locals[i - 1];
        if (row->name && strcmp(row->name, name) == 0)
            return row;
    }
    return NULL;
}

static XgClassId body_lookup_local_class(XgBodyCollect *bc, const char *name) {
    XgLocalType *row = body_find_local(bc, name);
    return row ? row->class_id : XG_NO_ID;
}

static XgInterfaceId body_lookup_local_interface(XgBodyCollect *bc, const char *name) {
    XgLocalType *row = body_find_local(bc, name);
    return row ? row->interface_id : XG_NO_ID;
}

static bool body_has_name_local(XgBodyCollect *bc, const char *name) {
    if (!bc || !name)
        return false;
    for (uint32_t i = bc->nname_locals; i > 0; i--) {
        const XgLocalName *row = &bc->name_locals[i - 1];
        if (row->name && strcmp(row->name, name) == 0)
            return true;
    }
    return false;
}

static bool body_has_symbol_local(XgBodyCollect *bc, const char *name, uint32_t symbol_id) {
    if (!bc || !name)
        return false;
    for (uint32_t i = bc->nname_locals; i > 0; i--) {
        const XgLocalName *row = &bc->name_locals[i - 1];
        if (symbol_id != 0 && row->symbol_id != 0 && row->symbol_id == symbol_id)
            return true;
        if (row->name && strcmp(row->name, name) == 0)
            return true;
    }
    return false;
}

static void body_note_variable_read(XgBodyCollect *bc, const VariableNode *var) {
    if (!bc || !var || !var->name)
        return;
    if (body_has_symbol_local(bc, var->name, var->symbol_id))
        return;
    if (producer_lookup_func_row(bc->producer, var->name) ||
        producer_lookup_class(bc->producer, var->name) != XG_NO_ID ||
        producer_lookup_interface(bc->producer, var->name) != XG_NO_ID ||
        producer_stdlib_module_known(var->name))
        return;
    bc->effect_bits |= XG_BODY_MAY_READ_MEM;
}

static void body_assign_local(XgBodyCollect *bc, const char *name, uint32_t symbol_id,
                              XgClassId class_id, XgInterfaceId interface_id, uint32_t type_key) {
    XgLocalType *row = body_find_local(bc, name);
    if (class_id == XG_NO_ID && interface_id == XG_NO_ID && type_key == 0)
        return;
    if (!row) {
        (void) body_push_local(bc, name, symbol_id, class_id, interface_id, type_key, true);
        return;
    }
    if (!row->inferred)
        return;
    row->class_id = class_id;
    row->interface_id = interface_id;
    row->type_key = type_key;
    row->json_shape_id = XG_NO_ID;
    row->json_shape_literal = NULL;
    row->record_shape_id = XG_NO_ID;
    row->record_shape_literal = NULL;
    row->map_shape_id = XG_NO_ID;
    row->map_container_kind = 0;
    row->map_receiver_type_key = 0;
    row->map_key_type_key = 0;
    row->map_value_type_key = 0;
}

typedef struct XgCaptureScan {
    const XgLocalName *outer_locals;
    uint32_t nouter_locals;
    XgLocalName *locals;
    uint32_t nlocals;
    uint32_t local_cap;
    bool has_capture;
} XgCaptureScan;

static bool capture_reserve_locals(XgCaptureScan *scan, uint32_t needed) {
    uint32_t new_cap;
    XgLocalName *rows;
    if (scan->local_cap >= needed)
        return true;
    new_cap = scan->local_cap < 8 ? 8 : scan->local_cap;
    while (new_cap < needed)
        new_cap *= 2;
    rows = (XgLocalName *) xr_realloc(scan->locals, (size_t) new_cap * sizeof(*rows));
    if (!rows)
        return false;
    scan->locals = rows;
    scan->local_cap = new_cap;
    return true;
}

static bool capture_push_local(XgCaptureScan *scan, const char *name, uint32_t symbol_id) {
    XgLocalName *row;
    if (!scan || !name || !name[0])
        return true;
    if (!capture_reserve_locals(scan, scan->nlocals + 1))
        return false;
    row = &scan->locals[scan->nlocals++];
    row->name = name;
    row->symbol_id = symbol_id;
    return true;
}

static bool capture_local_matches(const XgLocalName *row, const char *name, uint32_t symbol_id) {
    if (!row)
        return false;
    if (symbol_id != 0 && row->symbol_id != 0)
        return row->symbol_id == symbol_id;
    return name && row->name && strcmp(row->name, name) == 0;
}

static bool capture_has_inner_local(const XgCaptureScan *scan, const char *name,
                                    uint32_t symbol_id) {
    if (!scan || !name)
        return false;
    for (uint32_t i = scan->nlocals; i > 0; i--) {
        if (capture_local_matches(&scan->locals[i - 1], name, symbol_id))
            return true;
    }
    return false;
}

static bool capture_has_outer_local(const XgCaptureScan *scan, const char *name,
                                    uint32_t symbol_id) {
    if (!scan || !name)
        return false;
    for (uint32_t i = scan->nouter_locals; i > 0; i--) {
        if (capture_local_matches(&scan->outer_locals[i - 1], name, symbol_id))
            return true;
    }
    return false;
}

static void capture_mark_name(XgCaptureScan *scan, const char *name, uint32_t symbol_id) {
    if (!scan || !name || !name[0])
        return;
    if (capture_has_inner_local(scan, name, symbol_id))
        return;
    if (capture_has_outer_local(scan, name, symbol_id))
        scan->has_capture = true;
}

static void capture_scan_node(XgCaptureScan *scan, const AstNode *node);

static void capture_scan_function_params(XgCaptureScan *scan, const FunctionDeclNode *fn) {
    if (!scan || !fn)
        return;
    for (int i = 0; i < fn->param_count; i++) {
        const XrParamNode *param = fn->params ? fn->params[i] : NULL;
        (void) capture_push_local(scan, param ? param->name : NULL, param ? param->symbol_id : 0);
    }
}

static void capture_scan_node_list(XgCaptureScan *scan, AstNode *const *nodes, int count) {
    if (!scan || !nodes)
        return;
    for (int i = 0; i < count; i++)
        capture_scan_node(scan, nodes[i]);
}

static void capture_scan_function_expr(XgCaptureScan *scan, const FunctionDeclNode *fn) {
    uint32_t base_locals;
    if (!scan || !fn)
        return;
    base_locals = scan->nlocals;
    (void) capture_push_local(scan, fn->name, fn->symbol_id);
    capture_scan_function_params(scan, fn);
    capture_scan_node(scan, fn->body);
    scan->nlocals = base_locals;
}

static void capture_scan_node(XgCaptureScan *scan, const AstNode *node) {
    uint32_t base_locals;
    if (!scan || !node || scan->has_capture)
        return;
    switch (node->type) {
        case AST_PROGRAM:
            capture_scan_node_list(scan, node->as.program.statements, node->as.program.count);
            break;
        case AST_BLOCK:
            base_locals = scan->nlocals;
            capture_scan_node_list(scan, node->as.block.statements, node->as.block.count);
            scan->nlocals = base_locals;
            break;
        case AST_EXPR_STMT:
            capture_scan_node(scan, node->as.expr_stmt);
            break;
        case AST_PRINT_STMT:
            capture_scan_node_list(scan, node->as.print_stmt.exprs, node->as.print_stmt.expr_count);
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
        case AST_SHARED_DECL:
            capture_scan_node(scan, node->as.var_decl.initializer);
            (void) capture_push_local(scan, node->as.var_decl.name, node->as.var_decl.symbol_id);
            break;
        case AST_VARIABLE:
            capture_mark_name(scan, node->as.variable.name, node->as.variable.symbol_id);
            break;
        case AST_ASSIGNMENT:
            capture_scan_node(scan, node->as.assignment.value);
            capture_mark_name(scan, node->as.assignment.name, node->as.assignment.symbol_id);
            break;
        case AST_COMPOUND_ASSIGNMENT:
            capture_scan_node(scan, node->as.compound_assignment.object);
            capture_scan_node(scan, node->as.compound_assignment.value);
            capture_mark_name(scan, node->as.compound_assignment.name,
                              node->as.compound_assignment.symbol_id);
            break;
        case AST_INC:
        case AST_DEC:
            capture_mark_name(scan, node->as.inc.name, node->as.inc.symbol_id);
            break;
        case AST_RETURN_STMT:
            capture_scan_node_list(scan, node->as.return_stmt.values,
                                   node->as.return_stmt.value_count);
            break;
        case AST_CALL_EXPR:
            capture_scan_node(scan, node->as.call_expr.callee);
            capture_scan_node_list(scan, node->as.call_expr.arguments,
                                   node->as.call_expr.arg_count);
            break;
        case AST_FUNCTION_EXPR:
            capture_scan_function_expr(scan, &node->as.function_expr);
            break;
        case AST_SUPER_CALL:
            capture_scan_node_list(scan, node->as.super_call.arguments,
                                   node->as.super_call.arg_count);
            break;
        case AST_MEMBER_ACCESS:
            capture_scan_node(scan, node->as.member_access.object);
            break;
        case AST_MEMBER_SET:
            capture_scan_node(scan, node->as.member_set.object);
            capture_scan_node(scan, node->as.member_set.value);
            break;
        case AST_INDEX_GET:
            capture_scan_node(scan, node->as.index_get.array);
            capture_scan_node(scan, node->as.index_get.index);
            break;
        case AST_INDEX_SET:
            capture_scan_node(scan, node->as.index_set.array);
            capture_scan_node(scan, node->as.index_set.index);
            capture_scan_node(scan, node->as.index_set.value);
            break;
        case AST_SLICE_EXPR:
            capture_scan_node(scan, node->as.slice_expr.source);
            capture_scan_node(scan, node->as.slice_expr.start);
            capture_scan_node(scan, node->as.slice_expr.end);
            break;
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
        case AST_NULLISH_COALESCE:
            capture_scan_node(scan, node->as.binary.left);
            capture_scan_node(scan, node->as.binary.right);
            break;
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
        case AST_FORCE_UNWRAP:
            capture_scan_node(scan, node->as.unary.operand);
            break;
        case AST_GROUPING:
            capture_scan_node(scan, node->as.grouping);
            break;
        case AST_TERNARY:
            capture_scan_node(scan, node->as.ternary.condition);
            capture_scan_node(scan, node->as.ternary.true_expr);
            capture_scan_node(scan, node->as.ternary.false_expr);
            break;
        case AST_AS_EXPR:
            capture_scan_node(scan, node->as.as_expr.expr);
            break;
        case AST_IS_EXPR:
            capture_scan_node(scan, node->as.is_expr.expr);
            break;
        case AST_COMPTIME_EXPR:
            capture_scan_node(scan, node->as.comptime_expr.expr);
            break;
        case AST_ARRAY_LITERAL:
            if (node->as.array_literal.is_repeat) {
                capture_scan_node(scan, node->as.array_literal.repeat_value);
                capture_scan_node(scan, node->as.array_literal.repeat_count);
            } else {
                capture_scan_node_list(scan, node->as.array_literal.elements,
                                       node->as.array_literal.count);
            }
            break;
        case AST_TUPLE_LITERAL:
            capture_scan_node_list(scan, node->as.tuple_literal.elements,
                                   node->as.tuple_literal.count);
            break;
        case AST_OBJECT_LITERAL:
            capture_scan_node_list(scan, node->as.object_literal.keys,
                                   node->as.object_literal.count);
            capture_scan_node_list(scan, node->as.object_literal.values,
                                   node->as.object_literal.count);
            break;
        case AST_MAP_LITERAL:
            capture_scan_node_list(scan, node->as.map_literal.keys, node->as.map_literal.count);
            capture_scan_node_list(scan, node->as.map_literal.values, node->as.map_literal.count);
            break;
        case AST_SET_LITERAL:
            capture_scan_node_list(scan, node->as.set_literal.elements, node->as.set_literal.count);
            break;
        case AST_STRUCT_LITERAL:
            capture_scan_node_list(scan, node->as.struct_literal.field_values,
                                   node->as.struct_literal.field_count);
            break;
        case AST_NEW_EXPR:
            capture_scan_node_list(scan, node->as.new_expr.arguments, node->as.new_expr.arg_count);
            break;
        case AST_THROW_STMT:
            capture_scan_node(scan, node->as.throw_stmt.expression);
            break;
        case AST_AWAIT_EXPR:
            capture_scan_node(scan, node->as.await_expr.expr);
            capture_scan_node(scan, node->as.await_expr.timeout);
            capture_scan_node(scan, node->as.await_expr.into);
            break;
        case AST_YIELD_STMT:
            capture_scan_node(scan, node->as.yield_stmt.value);
            break;
        case AST_GO_EXPR:
            capture_scan_node(scan, node->as.go_expr.expr);
            break;
        case AST_CHANNEL_NEW:
            capture_scan_node(scan, node->as.channel_new.buffer_size);
            break;
        case AST_SELECT_STMT:
            capture_scan_node_list(scan, node->as.select_stmt.cases,
                                   node->as.select_stmt.case_count);
            break;
        case AST_SELECT_CASE:
            base_locals = scan->nlocals;
            capture_scan_node(scan, node->as.select_case.channel);
            capture_scan_node(scan, node->as.select_case.value);
            (void) capture_push_local(scan, node->as.select_case.var_name,
                                      node->as.select_case.var_symbol_id);
            capture_scan_node(scan, node->as.select_case.body);
            scan->nlocals = base_locals;
            break;
        case AST_DEFER_STMT:
            capture_scan_node(scan, node->as.defer_stmt.expr);
            break;
        case AST_SCOPE_BLOCK:
            capture_scan_node(scan, node->as.scope_block.body);
            break;
        case AST_MOVE_EXPR:
            capture_scan_node(scan, node->as.move_expr.expr);
            break;
        case AST_UNSAFE_EXPR:
            capture_scan_node(scan, node->as.unsafe_expr.operand);
            break;
        case AST_IF_STMT:
            capture_scan_node(scan, node->as.if_stmt.condition);
            capture_scan_node(scan, node->as.if_stmt.then_branch);
            capture_scan_node(scan, node->as.if_stmt.else_branch);
            break;
        case AST_WHILE_STMT:
            capture_scan_node(scan, node->as.while_stmt.condition);
            capture_scan_node(scan, node->as.while_stmt.body);
            break;
        case AST_FOR_STMT:
            base_locals = scan->nlocals;
            capture_scan_node(scan, node->as.for_stmt.initializer);
            capture_scan_node(scan, node->as.for_stmt.condition);
            capture_scan_node(scan, node->as.for_stmt.increment);
            capture_scan_node(scan, node->as.for_stmt.body);
            scan->nlocals = base_locals;
            break;
        case AST_FOR_IN_STMT:
            base_locals = scan->nlocals;
            capture_scan_node(scan, node->as.for_in_stmt.collection);
            (void) capture_push_local(scan, node->as.for_in_stmt.item_name,
                                      node->as.for_in_stmt.item_symbol_id);
            capture_scan_node(scan, node->as.for_in_stmt.body);
            scan->nlocals = base_locals;
            break;
        case AST_TRY_CATCH:
            capture_scan_node(scan, node->as.try_catch.try_body);
            for (int i = 0; i < node->as.try_catch.catch_count; i++) {
                const XrCatchClause *cc =
                    node->as.try_catch.catch_clauses ? node->as.try_catch.catch_clauses[i] : NULL;
                if (!cc)
                    continue;
                base_locals = scan->nlocals;
                (void) capture_push_local(scan, cc->var_name, cc->symbol_id);
                capture_scan_node(scan, cc->body);
                scan->nlocals = base_locals;
            }
            break;
        default:
            break;
    }
}

static bool body_function_expr_captures_current_locals(XgBodyCollect *bc,
                                                       const FunctionDeclNode *fn) {
    XgCaptureScan scan;
    if (!bc || !fn || bc->nname_locals == 0)
        return false;
    memset(&scan, 0, sizeof(scan));
    scan.outer_locals = bc->name_locals;
    scan.nouter_locals = bc->nname_locals;
    capture_scan_function_expr(&scan, fn);
    xr_free(scan.locals);
    return scan.has_capture;
}

static XgClassId body_resolve_expr_class(XgBodyCollect *bc, const AstNode *expr) {
    const AstNode *callee;
    if (!bc || !expr)
        return XG_NO_ID;
    switch (expr->type) {
        case AST_THIS_EXPR:
            return bc->current_class_id;
        case AST_VARIABLE:
            return body_lookup_local_class(bc, expr->as.variable.name);
        case AST_NEW_EXPR:
            return producer_lookup_class(bc->producer, expr->as.new_expr.class_name);
        case AST_CALL_EXPR:
            callee = expr->as.call_expr.callee;
            if (callee && callee->type == AST_VARIABLE)
                return producer_lookup_class(bc->producer, callee->as.variable.name);
            return XG_NO_ID;
        case AST_AS_EXPR:
            return producer_lookup_class_from_tref(bc->producer, expr->as.as_expr.type);
        case AST_GROUPING:
            return body_resolve_expr_class(bc, expr->as.grouping);
        default:
            return XG_NO_ID;
    }
}

static XgInterfaceId body_resolve_expr_interface(XgBodyCollect *bc, const AstNode *expr) {
    if (!bc || !expr)
        return XG_NO_ID;
    switch (expr->type) {
        case AST_VARIABLE:
            return body_lookup_local_interface(bc, expr->as.variable.name);
        case AST_AS_EXPR:
            return producer_lookup_interface_from_tref(bc->producer, expr->as.as_expr.type);
        case AST_GROUPING:
            return body_resolve_expr_interface(bc, expr->as.grouping);
        default:
            return XG_NO_ID;
    }
}

static uint32_t body_capabilities_for_builtin_constructor(const char *name) {
    if (!name || !name[0])
        return 0;
    if (strcmp(name, "Channel") == 0)
        return XG_CAP_CHANNEL | XG_CAP_COROUTINE | XG_CAP_OBJECTS;
    if (strcmp(name, "Atomic") == 0)
        return XG_CAP_ATOMIC | XG_CAP_OBJECTS;
    if (strcmp(name, "WorkQueue") == 0)
        return XG_CAP_WORK_QUEUE | XG_CAP_COROUTINE | XG_CAP_OBJECTS;
    if (strcmp(name, "ResultGroup") == 0)
        return XG_CAP_RESULT_GROUP | XG_CAP_COROUTINE | XG_CAP_OBJECTS;
    if (strcmp(name, "CountdownLatch") == 0)
        return XG_CAP_COUNTDOWN_LATCH | XG_CAP_COROUTINE | XG_CAP_OBJECTS;
    if (strcmp(name, "Semaphore") == 0)
        return XG_CAP_SEMAPHORE | XG_CAP_COROUTINE | XG_CAP_OBJECTS;
    if (strcmp(name, "EventCount") == 0)
        return XG_CAP_EVENT_COUNT | XG_CAP_COROUTINE | XG_CAP_OBJECTS;
    return 0;
}

static uint32_t body_capabilities_for_type_ref(const XrTypeRef *type) {
    return body_capabilities_for_builtin_constructor(xr_tref_head_name(type));
}

static uint32_t attrs_derive_flags(XrAttribute **attrs, int count) {
    uint32_t flags = 0;
    for (int i = 0; i < count; i++) {
        if (attrs[i] && attrs[i]->kind == ATTR_DERIVE)
            flags |= attrs[i]->derive_flags;
    }
    return flags;
}

static bool derive_kind_from_flag(uint32_t flag, uint8_t *out_kind) {
    if (!out_kind)
        return false;
    switch (flag) {
        case XR_DERIVE_JSON:
            *out_kind = XG_DERIVE_JSON;
            return true;
        case XR_DERIVE_INSPECT:
            *out_kind = XG_DERIVE_INSPECT;
            return true;
        case XR_DERIVE_EQ:
            *out_kind = XG_DERIVE_EQ;
            return true;
        case XR_DERIVE_HASH:
            *out_kind = XG_DERIVE_HASH;
            return true;
        case XR_DERIVE_CLONE:
            *out_kind = XG_DERIVE_CLONE;
            return true;
        default:
            return false;
    }
}

static uint32_t derived_field_flags(const FieldDeclNode *field) {
    uint32_t flags = 0;
    if (!field)
        return 0;
    if (field->is_private)
        flags |= XG_DERIVED_FIELD_PRIVATE;
    else if (field->is_protected)
        flags |= XG_DERIVED_FIELD_PROTECTED;
    else
        flags |= XG_DERIVED_FIELD_PUBLIC;
    if (field->is_static)
        flags |= XG_DERIVED_FIELD_STATIC;
    if (field->is_final || field->is_const)
        flags |= XG_DERIVED_FIELD_READONLY;
    return flags;
}

static uint64_t hash_derive_decl(uint32_t type_key, uint8_t derive_kind, const ClassDeclNode *cls) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    h = fold_u64(h, type_key);
    h = fold_u64(h, derive_kind);
    if (!cls || cls->field_count <= 0 || !cls->fields)
        return h ? h : 1;
    h = fold_u64(h, (uint64_t) cls->field_count);
    for (int i = 0; i < cls->field_count; i++) {
        const AstNode *field_node = cls->fields[i];
        const FieldDeclNode *field =
            field_node && field_node->type == AST_FIELD_DECL ? &field_node->as.field_decl : NULL;
        h = fold_u64(h, (uint64_t) i);
        h = fold_u64(h, field ? hash_name32(field->name) : 0);
        h = fold_u64(h, field ? hash_tref32(field->field_type) : 0);
        h = fold_u64(h, derived_field_flags(field));
    }
    return h ? h : 1;
}

static XgFieldId producer_find_class_field_id(const XgProducer *p, XgClassId owner_class_id,
                                              uint32_t decl_ordinal) {
    if (!p || !p->evidence || owner_class_id == XG_NO_ID)
        return XG_NO_ID;
    for (uint32_t i = 0; i < p->evidence->nclass_fields; i++) {
        const XgClassFieldSummary *field = &p->evidence->class_fields[i];
        if (field->owner_class_id == owner_class_id && field->decl_ordinal == decl_ordinal)
            return field->field_id;
    }
    return XG_NO_ID;
}

static bool producer_add_derive_fields(XgProducer *p, XgDeriveId derive_id,
                                       const ClassDeclNode *cls, XgClassId owner_class_id) {
    if (!p || !p->evidence || !cls || cls->field_count <= 0)
        return true;
    for (int i = 0; i < cls->field_count; i++) {
        const AstNode *field_node = cls->fields ? cls->fields[i] : NULL;
        const FieldDeclNode *field =
            field_node && field_node->type == AST_FIELD_DECL ? &field_node->as.field_decl : NULL;
        XgDerivedFieldSummary row;
        memset(&row, 0, sizeof(row));
        row.field_id = (XgDerivedFieldId) (p->evidence->nderived_fields + 1);
        row.derive_id = derive_id;
        row.field_ordinal = (uint16_t) (i < UINT16_MAX ? i : UINT16_MAX);
        row.name_id = field ? hash_name32(field->name) : 0;
        row.type_key = field ? hash_tref32(field->field_type) : 0;
        row.source_field_id = producer_find_class_field_id(p, owner_class_id, (uint32_t) i);
        row.flags = derived_field_flags(field);
        if (!xg_global_evidence_add_derived_field(p->evidence, &row))
            return false;
    }
    return true;
}

static bool producer_add_decl_derives(XgProducer *p, XgModuleId module_id, XgDeclId owner_decl_id,
                                      uint32_t source_span_id, const char *type_name,
                                      uint32_t derive_flags, const ClassDeclNode *cls,
                                      XgClassId owner_class_id) {
    static const uint32_t ordered_flags[] = {
        XR_DERIVE_JSON, XR_DERIVE_INSPECT, XR_DERIVE_EQ, XR_DERIVE_HASH, XR_DERIVE_CLONE,
    };
    uint32_t type_key = hash_named_type_key32(type_name, NULL, 0);
    if (!p || !p->evidence || derive_flags == 0)
        return true;
    for (uint32_t i = 0; i < (uint32_t) (sizeof(ordered_flags) / sizeof(ordered_flags[0])); i++) {
        uint32_t flag = ordered_flags[i];
        uint8_t kind = 0;
        XgDeriveSummary row;
        XgDeriveId derive_id;
        if ((derive_flags & flag) == 0 || !derive_kind_from_flag(flag, &kind))
            continue;
        derive_id = (XgDeriveId) (p->evidence->nderives + 1);
        memset(&row, 0, sizeof(row));
        row.derive_id = derive_id;
        row.module_id = module_id;
        row.owner_decl_id = owner_decl_id;
        row.source_span_id = source_span_id;
        row.type_key = type_key;
        row.derive_kind = kind;
        row.field_start = cls && cls->field_count > 0 ? p->evidence->nderived_fields + 1 : 0;
        row.field_count =
            cls && cls->field_count > 0
                ? (uint16_t) (cls->field_count < UINT16_MAX ? cls->field_count : UINT16_MAX)
                : 0;
        row.flags = XG_DERIVE_OPT_IN;
        row.derive_hash = hash_derive_decl(type_key, kind, cls);
        if (!xg_global_evidence_add_derive(p->evidence, &row))
            return false;
        if (!producer_add_derive_fields(p, derive_id, cls, owner_class_id))
            return false;
    }
    return true;
}

static XrAttribute *attrs_find(XrAttribute **attrs, int count, AttributeKind kind) {
    for (int i = 0; i < count; i++) {
        if (attrs[i] && attrs[i]->kind == kind)
            return attrs[i];
    }
    return NULL;
}

static bool body_member_receiver_is_module(const MemberAccessNode *member, const char *name) {
    if (!member || !member->object || member->object->type != AST_VARIABLE || !name)
        return false;
    return strcmp(member->object->as.variable.name, name) == 0;
}

static const XgStdlibImportRow *body_stdlib_import_for_expr(XgBodyCollect *bc,
                                                            const AstNode *expr) {
    if (!bc || !expr)
        return NULL;
    if (expr->type == AST_GROUPING)
        return body_stdlib_import_for_expr(bc, expr->as.grouping);
    if (expr->type != AST_VARIABLE)
        return NULL;
    return producer_lookup_stdlib_import(bc->producer, bc->module_id, expr->as.variable.name);
}

static const char *body_stdlib_module_for_expr(XgBodyCollect *bc, const AstNode *expr) {
    const XgStdlibImportRow *row = body_stdlib_import_for_expr(bc, expr);
    if (row)
        return row->member_name ? NULL : row->module_name;
    if (!bc || !expr)
        return NULL;
    if (expr->type == AST_GROUPING)
        return body_stdlib_module_for_expr(bc, expr->as.grouping);
    if (expr->type == AST_VARIABLE && expr->as.variable.name &&
        !body_has_name_local(bc, expr->as.variable.name) &&
        producer_stdlib_module_known(expr->as.variable.name))
        return expr->as.variable.name;
    return NULL;
}

static bool body_call_is_sys_thread_spawn(const CallExprNode *call) {
    if (!call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    const MemberAccessNode *spawn = &call->callee->as.member_access;
    if (!spawn->name || strcmp(spawn->name, "spawn") != 0 || !spawn->object ||
        spawn->object->type != AST_MEMBER_ACCESS)
        return false;
    const MemberAccessNode *thread = &spawn->object->as.member_access;
    if (!thread->name || strcmp(thread->name, "Thread") != 0 || !thread->object ||
        thread->object->type != AST_VARIABLE)
        return false;
    const char *module_name = thread->object->as.variable.name;
    return module_name && strcmp(module_name, "sys") == 0;
}

static uint32_t body_capabilities_for_builtin_member_constructor(const MemberAccessNode *member) {
    if (!body_member_receiver_is_module(member, "sync"))
        return 0;
    return body_capabilities_for_builtin_constructor(member->name);
}

static XgClassId body_parent_class_id(XgBodyCollect *bc) {
    XgClassNameRow *row;
    if (!bc || bc->current_class_id == XG_NO_ID)
        return XG_NO_ID;
    row = producer_lookup_class_row_by_id(bc->producer, bc->current_class_id);
    if (!row || row->summary_index >= bc->evidence->nclasses)
        return XG_NO_ID;
    return bc->evidence->classes[row->summary_index].parent_class_id;
}

static XgInterfaceId body_first_constraint_interface(XgBodyCollect *bc, XrTypeRef **type_args,
                                                     int type_arg_count) {
    if (!bc || !type_args || type_arg_count <= 0)
        return XG_NO_ID;
    for (int i = 0; i < type_arg_count; i++) {
        XgInterfaceId interface_id =
            producer_lookup_interface_from_tref(bc->producer, type_args[i]);
        if (interface_id != XG_NO_ID)
            return interface_id;
    }
    return XG_NO_ID;
}

static uint32_t body_class_type_key(XgBodyCollect *bc, XgClassId class_id) {
    XgClassNameRow *row;
    if (!bc || class_id == XG_NO_ID)
        return 0;
    row = producer_lookup_class_row_by_id(bc->producer, class_id);
    return row ? hash_named_type_key32(row->name, NULL, 0) : 0;
}

static uint32_t body_interface_type_key(XgBodyCollect *bc, XgInterfaceId interface_id) {
    XgInterfaceNameRow *row;
    if (!bc || interface_id == XG_NO_ID)
        return 0;
    row = producer_lookup_interface_row_by_id(bc->producer, interface_id);
    return row ? hash_named_type_key32(row->name, NULL, 0) : 0;
}

static bool producer_add_interface_object_use_row(XgProducer *p, XgInterfaceId interface_id,
                                                  XgFuncId owner_func_id, uint32_t source_span_id,
                                                  uint32_t body_ordinal, uint32_t type_key,
                                                  uint32_t reason) {
    XgInterfaceObjectUseSummary use;
    if (!p || !p->evidence || interface_id == XG_NO_ID || reason == 0)
        return true;
    if (type_key == 0)
        type_key = interface_id;
    for (uint32_t i = 0; i < p->evidence->ninterface_object_uses; i++) {
        XgInterfaceObjectUseSummary *existing = &p->evidence->interface_object_uses[i];
        if (existing->interface_id == interface_id && existing->owner_func_id == owner_func_id &&
            existing->source_span_id == source_span_id && existing->body_ordinal == body_ordinal &&
            existing->type_key == type_key) {
            existing->reason |= reason;
            return true;
        }
    }
    memset(&use, 0, sizeof(use));
    use.use_id = (XgInterfaceObjectUseId) (p->evidence->ninterface_object_uses + 1);
    use.interface_id = interface_id;
    use.owner_func_id = owner_func_id;
    use.source_span_id = source_span_id;
    use.body_ordinal = body_ordinal;
    use.type_key = type_key;
    use.reason = reason;
    return xg_global_evidence_add_interface_object_use(p->evidence, &use) != NULL;
}

static bool producer_add_interface_object_uses_for_type_ref(
    XgProducer *p, XgFuncId owner_func_id, uint32_t source_span_id, uint32_t *body_ordinal,
    const XrTypeRef *type, uint32_t base_reason, uint32_t storage_type_key) {
    XgInterfaceId direct_interface;
    uint32_t type_key;
    if (!p || !type)
        return true;
    type_key = storage_type_key != 0 ? storage_type_key : hash_tref32(type);

    if (type->kind == XR_TREF_GENERIC && type->name && type->children && type->nchildren > 0 &&
        strcmp(type->name, "Array") == 0) {
        const XrTypeRef *elem = type->children[0];
        XgInterfaceId elem_interface = producer_lookup_interface_from_tref(p, elem);
        if (elem_interface != XG_NO_ID) {
            uint32_t ordinal = body_ordinal ? ++(*body_ordinal) : 0;
            return producer_add_interface_object_use_row(
                p, elem_interface, owner_func_id, source_span_id, ordinal, hash_tref32(type),
                base_reason | XG_INTERFACE_OBJECT_USE_ARRAY);
        }
        return producer_add_interface_object_uses_for_type_ref(
            p, owner_func_id, source_span_id, body_ordinal, elem,
            base_reason | XG_INTERFACE_OBJECT_USE_ARRAY, hash_tref32(type));
    }

    if (type->kind == XR_TREF_FIXED_ARRAY && type->children && type->nchildren > 0) {
        return producer_add_interface_object_uses_for_type_ref(
            p, owner_func_id, source_span_id, body_ordinal, type->children[0],
            base_reason | XG_INTERFACE_OBJECT_USE_ARRAY, hash_tref32(type));
    }

    direct_interface = producer_lookup_interface_from_tref(p, type);
    if (direct_interface != XG_NO_ID) {
        uint32_t ordinal = body_ordinal ? ++(*body_ordinal) : 0;
        return producer_add_interface_object_use_row(p, direct_interface, owner_func_id,
                                                     source_span_id, ordinal, type_key,
                                                     base_reason | XG_INTERFACE_OBJECT_USE_VALUE);
    }

    switch ((XrTypeRefKind) type->kind) {
        case XR_TREF_OPTIONAL:
            return type->children && type->nchildren > 0
                       ? producer_add_interface_object_uses_for_type_ref(
                             p, owner_func_id, source_span_id, body_ordinal, type->children[0],
                             base_reason, type_key)
                       : true;
        case XR_TREF_UNION:
        case XR_TREF_TUPLE:
            for (uint8_t i = 0; i < type->nchildren; i++) {
                if (!producer_add_interface_object_uses_for_type_ref(
                        p, owner_func_id, source_span_id, body_ordinal,
                        type->children ? type->children[i] : NULL, base_reason, 0))
                    return false;
            }
            return true;
        case XR_TREF_OBJECT:
            for (uint8_t i = 0; i < type->nchildren; i++) {
                if (!producer_add_interface_object_uses_for_type_ref(
                        p, owner_func_id, source_span_id, body_ordinal,
                        type->children ? type->children[i] : NULL,
                        base_reason | XG_INTERFACE_OBJECT_USE_FIELD, 0))
                    return false;
            }
            return true;
        case XR_TREF_FUNCTION:
            if (type->children && type->nchildren > 0) {
                for (uint8_t i = 0; i + 1 < type->nchildren; i++) {
                    if (!producer_add_interface_object_uses_for_type_ref(
                            p, owner_func_id, source_span_id, body_ordinal, type->children[i],
                            base_reason | XG_INTERFACE_OBJECT_USE_PARAM, 0))
                        return false;
                }
                return producer_add_interface_object_uses_for_type_ref(
                    p, owner_func_id, source_span_id, body_ordinal,
                    type->children[type->nchildren - 1],
                    base_reason | XG_INTERFACE_OBJECT_USE_RETURN, 0);
            }
            return true;
        default:
            return true;
    }
}

static bool body_add_interface_object_uses_for_type_ref(XgBodyCollect *bc, const XrTypeRef *type,
                                                        uint32_t base_reason,
                                                        uint32_t source_span_id) {
    if (!bc || !type)
        return true;
    return producer_add_interface_object_uses_for_type_ref(
        bc->producer, bc->owner_func_id, source_span_id, &bc->interface_object_use_count, type,
        base_reason, 0);
}

static bool body_add_interface_capture_uses(XgBodyCollect *bc, uint32_t source_span_id) {
    if (!bc)
        return true;
    for (uint32_t i = 0; i < bc->nlocals; i++) {
        const XgLocalType *local = &bc->locals[i];
        uint32_t type_key;
        uint32_t ordinal;
        if (local->interface_id == XG_NO_ID)
            continue;
        type_key = local->type_key != 0 ? local->type_key
                                        : body_interface_type_key(bc, local->interface_id);
        ordinal = ++bc->interface_object_use_count;
        if (!producer_add_interface_object_use_row(
                bc->producer, local->interface_id, bc->owner_func_id, source_span_id, ordinal,
                type_key, XG_INTERFACE_OBJECT_USE_CAPTURE | XG_INTERFACE_OBJECT_USE_VALUE))
            return false;
    }
    return true;
}

static uint32_t body_unknown_arg_type_key(const AstNode *expr) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    static const char tag[] = "xg_arg_unknown_type";
    h = fold_bytes(h, tag, sizeof(tag) - 1);
    h = fold_u64(h, expr ? (uint64_t) expr->type : 0);
    return hash_folded32(h);
}

static const XgPendingBody *producer_find_function_body(const XgProducer *p, XgFuncId func_id) {
    if (!p || func_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < p->nbodies; i++) {
        const XgPendingBody *body = &p->bodies[i];
        if (body->func_id == func_id && body->function)
            return body;
    }
    return NULL;
}

static const XgPendingBody *producer_find_method_body(const XgProducer *p, XgMethodId method_id);

static const XrTypeRef *body_call_return_type_ref(XgBodyCollect *bc, const CallExprNode *call) {
    const AstNode *callee;
    const XgPendingBody *body;
    if (!bc || !call || !call->callee)
        return NULL;
    callee = call->callee;
    if (callee->type == AST_VARIABLE && callee->as.variable.name) {
        XgFuncNameRow *target = producer_lookup_func_row(bc->producer, callee->as.variable.name);
        body = producer_find_function_body(bc->producer, target ? target->func_id : XG_NO_ID);
        return body && body->function ? body->function->return_type : NULL;
    }
    if (callee->type == AST_MEMBER_ACCESS && callee->as.member_access.name) {
        const MemberAccessNode *member = &callee->as.member_access;
        XgClassId receiver_class = body_resolve_expr_class(bc, member->object);
        XgMethodSummary *method = producer_find_method_by_name_in_hierarchy(
            bc->producer, receiver_class, hash_name32(member->name), false);
        body = producer_find_method_body(bc->producer, method ? method->method_id : XG_NO_ID);
        return body && body->method ? body->method->return_type : NULL;
    }
    return NULL;
}

static uint32_t body_expr_type_key(XgBodyCollect *bc, const AstNode *expr) {
    if (!bc || !expr)
        return 0;
    switch (expr->type) {
        case AST_LITERAL_INT:
            return hash_synthetic_tref32(XR_TREF_INT, NULL, NULL, 0);
        case AST_LITERAL_FLOAT:
            return hash_synthetic_tref32(XR_TREF_FLOAT, NULL, NULL, 0);
        case AST_LITERAL_STRING:
            return hash_synthetic_tref32(XR_TREF_STRING, NULL, NULL, 0);
        case AST_LITERAL_CHAR:
            return hash_synthetic_tref32(XR_TREF_CHAR, NULL, NULL, 0);
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
            return hash_synthetic_tref32(XR_TREF_BOOL, NULL, NULL, 0);
        case AST_LITERAL_NULL:
            return hash_synthetic_tref32(XR_TREF_NULL, NULL, NULL, 0);
        case AST_LITERAL_BIGINT:
            return hash_named_type_key32("BigInt", NULL, 0);
        case AST_LITERAL_REGEX:
            return hash_named_type_key32("Regex", NULL, 0);
        case AST_VARIABLE: {
            XgLocalType *row = body_find_local(bc, expr->as.variable.name);
            if (!row)
                return 0;
            if (row->type_key != 0)
                return row->type_key;
            if (row->class_id != XG_NO_ID)
                return body_class_type_key(bc, row->class_id);
            if (row->interface_id != XG_NO_ID)
                return body_interface_type_key(bc, row->interface_id);
            return 0;
        }
        case AST_THIS_EXPR:
            return body_class_type_key(bc, bc->current_class_id);
        case AST_AS_EXPR:
            return hash_tref32(expr->as.as_expr.type);
        case AST_GROUPING:
            return body_expr_type_key(bc, expr->as.grouping);
        case AST_COMPTIME_EXPR:
            return body_expr_type_key(bc, expr->as.comptime_expr.expr);
        case AST_MOVE_EXPR:
            return body_expr_type_key(bc, expr->as.move_expr.expr);
        case AST_UNSAFE_EXPR:
            return body_expr_type_key(bc, expr->as.unsafe_expr.operand);
        case AST_FORCE_UNWRAP:
            return body_expr_type_key(bc, expr->as.unary.operand);
        case AST_CALL_EXPR: {
            const CallExprNode *call = &expr->as.call_expr;
            const AstNode *callee = call->callee;
            if (callee && callee->type == AST_MEMBER_ACCESS && call->type_arg_count > 0 &&
                call->type_args && call->type_args[0]) {
                const MemberAccessNode *member = &callee->as.member_access;
                if (member->name && strcmp(member->name, "decode") == 0 && member->object &&
                    member->object->type == AST_VARIABLE && member->object->as.variable.name &&
                    strcmp(member->object->as.variable.name, "Json") == 0)
                    return hash_tref32(call->type_args[0]);
            }
            {
                const XrTypeRef *return_type = body_call_return_type_ref(bc, call);
                if (return_type)
                    return hash_tref32(return_type);
            }
            break;
        }
        case AST_NEW_EXPR:
            return hash_named_type_key32(expr->as.new_expr.class_name, expr->as.new_expr.type_args,
                                         expr->as.new_expr.type_arg_count);
        case AST_STRUCT_LITERAL:
            return hash_named_type_key32(expr->as.struct_literal.struct_name,
                                         expr->as.struct_literal.type_args,
                                         expr->as.struct_literal.type_arg_count);
        default:
            break;
    }
    return 0;
}

static uint32_t body_call_arg_type_key_start(XgBodyCollect *bc, AstNode **arguments,
                                             int arg_count) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    if (!arguments || arg_count <= 0)
        return 0;
    h = fold_u64(h, (uint64_t) arg_count);
    for (int i = 0; i < arg_count; i++) {
        uint32_t key = body_expr_type_key(bc, arguments[i]);
        if (key == 0)
            key = body_unknown_arg_type_key(arguments[i]);
        h = fold_u64(h, key);
    }
    return hash_folded32(h);
}

static uint32_t body_options_count_mask_id(XgCallsiteId callsite_id, const char *role,
                                           uint16_t start, uint16_t count) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    if (count == 0)
        return 0;
    h = fold_u64(h, hash_name32(role));
    h = fold_u64(h, callsite_id);
    h = fold_u64(h, start);
    h = fold_u64(h, count);
    return hash_folded32(h);
}

static uint32_t body_options_shape_type_key(const char *role, const XgCallsiteSummary *call,
                                            uint32_t arg_type_key_start, uint16_t field_count) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    h = fold_u64(h, hash_name32(role));
    h = fold_u64(h, call ? call->method_signature_key : 0);
    h = fold_u64(h, arg_type_key_start);
    h = fold_u64(h, field_count);
    return hash_folded32(h);
}

static uint64_t body_options_shape_hash(const char *role, const XgCallsiteSummary *call,
                                        uint32_t type_key, uint16_t field_count) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    h = fold_u64(h, hash_name32(role));
    h = fold_u64(h, call ? call->callsite_id : 0);
    h = fold_u64(h, call ? call->source_span_id : 0);
    h = fold_u64(h, type_key);
    h = fold_u64(h, field_count);
    return h ? h : 1;
}

static void body_add_options_bag_callsite(XgBodyCollect *bc, const CallExprNode *call,
                                          const XgCallsiteSummary *callsite) {
    if (!bc || !bc->evidence || !call || !callsite || call->default_arg_param_count <= 0)
        return;
    uint16_t param_count =
        (uint16_t) (call->default_arg_param_count < UINT16_MAX ? call->default_arg_param_count
                                                               : UINT16_MAX);
    uint16_t supplied_count =
        (uint16_t) (call->supplied_arg_count < 0
                        ? 0
                        : (call->supplied_arg_count < UINT16_MAX ? call->supplied_arg_count
                                                                 : UINT16_MAX));
    uint16_t default_count =
        (uint16_t) (call->default_arg_count < 0
                        ? 0
                        : (call->default_arg_count < UINT16_MAX ? call->default_arg_count
                                                                : UINT16_MAX));
    uint16_t required_count =
        (uint16_t) (call->required_arg_count < 0
                        ? 0
                        : (call->required_arg_count < UINT16_MAX ? call->required_arg_count
                                                                 : UINT16_MAX));
    if (param_count == 0 || supplied_count > param_count || default_count > param_count)
        return;

    XgRecordShapeId param_shape_id = (XgRecordShapeId) (bc->evidence->nrecord_shapes + 1);
    XgRecordShapeId supplied_shape_id = (XgRecordShapeId) (bc->evidence->nrecord_shapes + 2);
    uint32_t supplied_type_key =
        body_call_arg_type_key_start(bc, call->arguments, (int) supplied_count);
    XgRecordShapeSummary param_shape;
    XgRecordShapeSummary supplied_shape;
    XgOptionsBagSummary options;

    memset(&param_shape, 0, sizeof(param_shape));
    param_shape.record_shape_id = param_shape_id;
    param_shape.module_id = bc->module_id;
    param_shape.owner_func_id = bc->owner_func_id;
    param_shape.source_span_id = callsite->source_span_id;
    param_shape.type_key = body_options_shape_type_key("options:param", callsite,
                                                       callsite->arg_type_key_start, param_count);
    param_shape.field_name_start =
        body_options_count_mask_id(callsite->callsite_id, "options:param-fields", 0, param_count);
    param_shape.field_count = param_count;
    param_shape.shape_kind = XG_RECORD_SHAPE_OPTIONS;
    param_shape.flags =
        XG_RECORD_SHAPE_SEALED | XG_RECORD_SHAPE_STATIC_KEYS | XG_RECORD_SHAPE_HAS_OPTIONS;
    param_shape.shape_hash =
        body_options_shape_hash("options:param", callsite, param_shape.type_key, param_count);

    memset(&supplied_shape, 0, sizeof(supplied_shape));
    supplied_shape.record_shape_id = supplied_shape_id;
    supplied_shape.module_id = bc->module_id;
    supplied_shape.owner_func_id = bc->owner_func_id;
    supplied_shape.source_span_id = callsite->source_span_id;
    supplied_shape.type_key = body_options_shape_type_key("options:supplied", callsite,
                                                          supplied_type_key, supplied_count);
    supplied_shape.field_name_start = body_options_count_mask_id(
        callsite->callsite_id, "options:supplied-fields", 0, supplied_count);
    supplied_shape.field_count = supplied_count;
    supplied_shape.shape_kind = XG_RECORD_SHAPE_LITERAL;
    supplied_shape.flags = XG_RECORD_SHAPE_SEALED | XG_RECORD_SHAPE_STATIC_KEYS;
    supplied_shape.shape_hash = body_options_shape_hash("options:supplied", callsite,
                                                        supplied_shape.type_key, supplied_count);

    if (!xg_global_evidence_add_record_shape(bc->evidence, &param_shape))
        return;
    if (!xg_global_evidence_add_record_shape(bc->evidence, &supplied_shape))
        return;

    memset(&options, 0, sizeof(options));
    options.options_id = (XgOptionsId) (bc->evidence->noptions_bags + 1);
    options.module_id = bc->module_id;
    options.owner_func_id = bc->owner_func_id;
    options.callsite_id = callsite->callsite_id;
    options.param_shape_id = param_shape_id;
    options.supplied_shape_id = supplied_shape_id;
    options.source_span_id = callsite->source_span_id;
    options.supplied_field_mask_id = body_options_count_mask_id(
        callsite->callsite_id, "options:supplied-mask", 0, supplied_count);
    options.default_field_mask_id = body_options_count_mask_id(
        callsite->callsite_id, "options:default-mask", supplied_count, default_count);
    options.required_field_mask_id = body_options_count_mask_id(
        callsite->callsite_id, "options:required-mask", 0, required_count);
    options.supplied_count = supplied_count;
    options.default_count = default_count;
    options.required_count = required_count;
    options.flags = XG_OPTIONS_CALLSITE_PROVEN;
    if (default_count == 0) {
        options.action = XG_OPTIONS_DEFAULT_ELIDED;
        options.flags |= XG_OPTIONS_ALL_SUPPLIED;
    } else {
        options.action = XG_OPTIONS_DEFAULT_FILL_TABLE;
        options.flags |= XG_OPTIONS_NEEDS_DEFAULTS;
    }
    (void) xg_global_evidence_add_options_bag(bc->evidence, &options);
}

static bool body_type_ref_is_json(const XrTypeRef *type) {
    return type && type->kind == XR_TREF_NAMED && type->name && strcmp(type->name, "Json") == 0;
}

static uint32_t body_uint8_type_key(void) {
    return hash_synthetic_width_tref32(XR_TREF_INT_WIDTH, XR_TREF_NW_U8);
}

static uint32_t body_char_type_key(void) {
    return hash_synthetic_tref32(XR_TREF_CHAR, NULL, NULL, 0);
}

static bool body_type_ref_sequence_parts(const XrTypeRef *type, uint8_t *out_sequence_kind,
                                         uint32_t *out_elem_type_key) {
    if (out_sequence_kind)
        *out_sequence_kind = 0;
    if (out_elem_type_key)
        *out_elem_type_key = 0;
    if (!type)
        return false;
    if (type->kind == XR_TREF_STRING) {
        if (out_sequence_kind)
            *out_sequence_kind = XG_SEQ_STRING;
        if (out_elem_type_key)
            *out_elem_type_key = body_char_type_key();
        return true;
    }
    if (type->kind == XR_TREF_NAMED && type->name) {
        if (strcmp(type->name, "Bytes") == 0) {
            if (out_sequence_kind)
                *out_sequence_kind = XG_SEQ_BYTES;
            if (out_elem_type_key)
                *out_elem_type_key = body_uint8_type_key();
            return true;
        }
        if (strcmp(type->name, "ByteSpan") == 0) {
            if (out_sequence_kind)
                *out_sequence_kind = XG_SEQ_BYTE_SPAN;
            if (out_elem_type_key)
                *out_elem_type_key = body_uint8_type_key();
            return true;
        }
        if (strcmp(type->name, "StringBuilder") == 0) {
            if (out_sequence_kind)
                *out_sequence_kind = XG_SEQ_STRING_BUILDER;
            if (out_elem_type_key)
                *out_elem_type_key = body_char_type_key();
            return true;
        }
    }
    if (type->kind == XR_TREF_GENERIC && type->name && type->children && type->nchildren > 0) {
        if (strcmp(type->name, "Array") == 0) {
            if (out_sequence_kind)
                *out_sequence_kind = XG_SEQ_ARRAY;
            if (out_elem_type_key)
                *out_elem_type_key = hash_tref32(type->children[0]);
            return true;
        }
        if (strcmp(type->name, "Span") == 0) {
            if (out_sequence_kind)
                *out_sequence_kind = XG_SEQ_SPAN;
            if (out_elem_type_key)
                *out_elem_type_key = hash_tref32(type->children[0]);
            return true;
        }
    }
    return false;
}

static const XrTypeRef *body_type_ref_sequence_elem_type_ref(const XrTypeRef *type) {
    if (!type || type->kind != XR_TREF_GENERIC || !type->name || !type->children ||
        type->nchildren == 0)
        return NULL;
    if (strcmp(type->name, "Array") == 0 || strcmp(type->name, "Span") == 0)
        return type->children[0];
    return NULL;
}

static bool body_type_key_is_pod_array_lane(uint32_t type_key) {
    static const uint8_t int_widths[] = {XR_TREF_NW_I64,   XR_TREF_NW_BOOL, XR_TREF_NW_I8,
                                         XR_TREF_NW_I16,   XR_TREF_NW_I32,  XR_TREF_NW_U8,
                                         XR_TREF_NW_U16,   XR_TREF_NW_U32,  XR_TREF_NW_U64,
                                         XR_TREF_NW_ISIZE, XR_TREF_NW_USIZE};
    static const uint8_t float_widths[] = {XR_TREF_NW_F64, XR_TREF_NW_F32};
    if (type_key == hash_synthetic_tref32(XR_TREF_INT, NULL, NULL, 0) ||
        type_key == hash_synthetic_tref32(XR_TREF_FLOAT, NULL, NULL, 0) ||
        type_key == hash_synthetic_tref32(XR_TREF_BOOL, NULL, NULL, 0) ||
        type_key == hash_synthetic_tref32(XR_TREF_CHAR, NULL, NULL, 0))
        return true;
    for (uint32_t i = 0; i < sizeof(int_widths) / sizeof(int_widths[0]); i++) {
        if (type_key == hash_synthetic_width_tref32(XR_TREF_INT_WIDTH, int_widths[i]))
            return true;
    }
    for (uint32_t i = 0; i < sizeof(float_widths) / sizeof(float_widths[0]); i++) {
        if (type_key == hash_synthetic_width_tref32(XR_TREF_FLOAT_WIDTH, float_widths[i]))
            return true;
    }
    return false;
}

static uint64_t body_generic_storage_hash(uint8_t storage_kind, uint32_t origin_type_key,
                                          uint32_t specialized_type_key, uint32_t elem_type_key) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    h = fold_u64(h, storage_kind);
    h = fold_u64(h, origin_type_key);
    h = fold_u64(h, specialized_type_key);
    h = fold_u64(h, elem_type_key);
    return h ? h : 1;
}

static void body_add_generic_array_storage(XgBodyCollect *bc, const XrTypeRef *type,
                                           uint32_t source_span_id) {
    XrTypeRef *elem_type;
    XrTypeRef *type_args[1];
    XgGenericInstSummary inst;
    XgGenericStorageSummary storage;
    uint32_t elem_type_key;
    uint32_t origin_type_key;
    uint32_t specialized_type_key;
    if (!bc || !bc->evidence || !type || type->kind != XR_TREF_GENERIC || !type->name ||
        strcmp(type->name, "Array") != 0 || !type->children || type->nchildren == 0)
        return;
    elem_type = type->children[0];
    elem_type_key = hash_tref32(elem_type);
    if (!body_type_key_is_pod_array_lane(elem_type_key))
        return;

    type_args[0] = elem_type;
    origin_type_key = hash_named_type_key32("Array", NULL, 0);
    specialized_type_key = hash_named_type_key32("Array", type_args, 1);

    memset(&inst, 0, sizeof(inst));
    inst.generic_inst_id = (XgGenericInstId) (bc->evidence->ngeneric_insts + 1);
    inst.module_id = bc->module_id;
    inst.name_id = hash_name32("Array");
    inst.type_key = hash_generic_inst_type_key("Array", type_args, 1, XG_GENERIC_INST_CONTAINER);
    inst.type_arg_key_start = hash_tref_list32(type_args, 1);
    inst.type_arg_count = 1;
    inst.source_span_id = source_span_id;
    inst.kind = XG_GENERIC_INST_CONTAINER;
    inst.flags = XG_GENERIC_INST_CONCRETE_TYPES | XG_GENERIC_INST_CONCRETE_STORAGE;
    if (!xg_global_evidence_add_generic_inst(bc->evidence, &inst))
        return;

    memset(&storage, 0, sizeof(storage));
    storage.storage_id = (XgGenericStorageId) (bc->evidence->ngeneric_storages + 1);
    storage.generic_inst_id = inst.generic_inst_id;
    storage.module_id = bc->module_id;
    storage.storage_kind = XG_GENERIC_STORAGE_ARRAY;
    storage.origin_type_key = origin_type_key;
    storage.specialized_type_key = specialized_type_key;
    storage.elem_type_key = elem_type_key;
    storage.flags = XG_GENERIC_STORAGE_TYPED_INLINE | XG_GENERIC_STORAGE_POD;
    storage.storage_hash = body_generic_storage_hash(storage.storage_kind, origin_type_key,
                                                     specialized_type_key, elem_type_key);
    (void) xg_global_evidence_add_generic_storage(bc->evidence, &storage);
}

static bool body_type_ref_map_parts(const XrTypeRef *type, uint8_t *out_container_kind,
                                    uint32_t *out_key_type_key, uint32_t *out_value_type_key) {
    if (out_container_kind)
        *out_container_kind = 0;
    if (out_key_type_key)
        *out_key_type_key = 0;
    if (out_value_type_key)
        *out_value_type_key = 0;
    if (!type || type->kind != XR_TREF_GENERIC || !type->name || !type->children)
        return false;
    if (strcmp(type->name, "Map") == 0 && type->nchildren >= 2) {
        if (out_container_kind)
            *out_container_kind = XG_MAP_CONTAINER_MAP;
        if (out_key_type_key)
            *out_key_type_key = hash_tref32(type->children[0]);
        if (out_value_type_key)
            *out_value_type_key = hash_tref32(type->children[1]);
        return true;
    }
    if (strcmp(type->name, "Set") == 0 && type->nchildren >= 1) {
        if (out_container_kind)
            *out_container_kind = XG_MAP_CONTAINER_SET;
        if (out_key_type_key)
            *out_key_type_key = hash_tref32(type->children[0]);
        return true;
    }
    return false;
}

static const ObjectLiteralNode *body_static_object_literal(const AstNode *node) {
    if (!node)
        return NULL;
    while (node && node->type == AST_GROUPING)
        node = node->as.grouping;
    return node && node->type == AST_OBJECT_LITERAL ? &node->as.object_literal : NULL;
}

static const char *body_object_literal_static_key(const ObjectLiteralNode *obj, int index) {
    AstNode *key;
    if (!obj || index < 0 || index >= obj->count)
        return NULL;
    if (obj->computed && obj->computed[index])
        return NULL;
    key = obj->keys ? obj->keys[index] : NULL;
    if (!key)
        return NULL;
    if (key->type == AST_LITERAL_STRING)
        return key->as.literal.raw_value.string_val;
    if (key->type == AST_VARIABLE)
        return key->as.variable.name;
    return NULL;
}

static bool body_object_literal_entry_is_spread(const ObjectLiteralNode *obj, int index) {
    AstNode *value;
    if (!obj || index < 0 || index >= obj->count)
        return false;
    value = obj->values ? obj->values[index] : NULL;
    return value && value->type == AST_SPREAD_EXPR;
}

static bool body_record_literal_has_spread(const ObjectLiteralNode *obj) {
    if (!obj)
        return false;
    for (int i = 0; i < obj->count; i++) {
        if (body_object_literal_entry_is_spread(obj, i))
            return true;
    }
    return false;
}

static int body_object_literal_static_field_index(const ObjectLiteralNode *obj, const char *name) {
    if (!obj || !name)
        return -1;
    for (int i = 0; i < obj->count; i++) {
        const char *key = body_object_literal_static_key(obj, i);
        if (key && strcmp(key, name) == 0)
            return i;
    }
    return -1;
}

static const char *body_static_string_key(const AstNode *expr) {
    while (expr && expr->type == AST_GROUPING)
        expr = expr->as.grouping;
    if (!expr || expr->type != AST_LITERAL_STRING)
        return NULL;
    return expr->as.literal.raw_value.string_val;
}

static uint64_t body_json_shape_hash(const ObjectLiteralNode *obj) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    int count = obj ? obj->count : 0;
    h = fold_u64(h, (uint64_t) count);
    for (int i = 0; i < count; i++) {
        const char *key = body_object_literal_static_key(obj, i);
        uint32_t name_id = key ? hash_name32(key) : 0;
        h = fold_u64(h, name_id);
    }
    return h ? h : 1;
}

static const XrTypeRef *body_type_alias_record_type_ref(const TypeAliasNode *alias) {
    if (!alias || !alias->resolved_type || alias->resolved_type->kind != XR_TREF_OBJECT)
        return NULL;
    return alias->resolved_type;
}

static int body_type_alias_record_field_count(const TypeAliasNode *alias) {
    const XrTypeRef *record_type;
    if (!alias)
        return 0;
    if (alias->field_count > 0 && alias->field_names)
        return alias->field_count;
    record_type = body_type_alias_record_type_ref(alias);
    if (!record_type || !record_type->field_names || !record_type->children)
        return 0;
    return (int) record_type->nchildren;
}

static const char *body_type_alias_record_field_name(const TypeAliasNode *alias, int index) {
    const XrTypeRef *record_type;
    if (!alias || index < 0)
        return NULL;
    if (alias->field_names && index < alias->field_count)
        return alias->field_names[index];
    record_type = body_type_alias_record_type_ref(alias);
    if (!record_type || !record_type->field_names || index >= (int) record_type->nchildren)
        return NULL;
    return record_type->field_names[index];
}

static const XrTypeRef *body_type_alias_record_field_type(const TypeAliasNode *alias, int index) {
    const XrTypeRef *record_type;
    if (!alias || index < 0)
        return NULL;
    if (alias->field_types && index < alias->field_count)
        return alias->field_types[index];
    record_type = body_type_alias_record_type_ref(alias);
    if (!record_type || !record_type->children || index >= (int) record_type->nchildren)
        return NULL;
    return record_type->children[index];
}

static bool body_type_alias_record_field_optional(const TypeAliasNode *alias, int index) {
    return alias && alias->field_optional && index >= 0 && index < alias->field_count &&
           alias->field_optional[index];
}

static bool body_type_alias_record_field_readonly(const TypeAliasNode *alias, int index) {
    const XrTypeRef *record_type = body_type_alias_record_type_ref(alias);
    return record_type && record_type->field_readonly && index >= 0 &&
           index < (int) record_type->nchildren && record_type->field_readonly[index];
}

static uint64_t body_type_alias_record_shape_hash(const TypeAliasNode *alias) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    int count = body_type_alias_record_field_count(alias);
    h = fold_u64(h, (uint64_t) count);
    for (int i = 0; i < count; i++) {
        const char *name = body_type_alias_record_field_name(alias, i);
        uint32_t name_id = name ? hash_name32(name) : 0;
        const XrTypeRef *field_type = body_type_alias_record_field_type(alias, i);
        uint32_t type_key = field_type ? hash_tref32(field_type) : 0;
        bool optional = body_type_alias_record_field_optional(alias, i);
        bool readonly = body_type_alias_record_field_readonly(alias, i);
        h = fold_u64(h, name_id);
        h = fold_u64(h, type_key);
        h = fold_u64(h, optional ? 1 : 0);
        h = fold_u64(h, readonly ? 1 : 0);
    }
    return h ? h : 1;
}

static uint32_t body_type_alias_field_name_start(const TypeAliasNode *alias) {
    return (uint32_t) (body_type_alias_record_shape_hash(alias) & UINT32_MAX);
}

static uint32_t body_type_alias_record_type_key(const TypeAliasNode *alias) {
    const XrTypeRef *record_type = body_type_alias_record_type_ref(alias);
    if (record_type)
        return hash_tref32(record_type);
    return alias && alias->name ? hash_named_type_key32(alias->name, NULL, 0) : 0;
}

static const XgJsonShapeSummary *
body_find_json_shape_for_type_key(XgBodyCollect *bc, uint32_t type_key, uint8_t shape_kind) {
    if (!bc || !bc->evidence || type_key == 0)
        return NULL;
    for (uint32_t i = 0; i < bc->evidence->njson_shapes; i++) {
        const XgJsonShapeSummary *shape = &bc->evidence->json_shapes[i];
        if (shape->type_key == type_key && shape->shape_kind == shape_kind)
            return shape;
    }
    return NULL;
}

static const XgRecordShapeSummary *
body_find_record_shape_for_type_key(XgBodyCollect *bc, uint32_t type_key, uint8_t shape_kind) {
    if (!bc || !bc->evidence || type_key == 0)
        return NULL;
    for (uint32_t i = 0; i < bc->evidence->nrecord_shapes; i++) {
        const XgRecordShapeSummary *shape = &bc->evidence->record_shapes[i];
        if (shape->type_key == type_key && shape->shape_kind == shape_kind)
            return shape;
    }
    return NULL;
}

static uint32_t body_expr_type_key(XgBodyCollect *bc, const AstNode *expr);

static void body_add_json_fields_for_literal(XgBodyCollect *bc, XgJsonShapeId shape_id,
                                             const ObjectLiteralNode *obj) {
    if (!bc || !bc->evidence || shape_id == XG_NO_ID || !obj)
        return;
    for (int i = 0; i < obj->count; i++) {
        const char *key = body_object_literal_static_key(obj, i);
        XgJsonFieldSummary field;
        if (!key)
            continue;
        memset(&field, 0, sizeof(field));
        field.field_id = (XgJsonFieldId) (bc->evidence->njson_fields + 1);
        field.shape_id = shape_id;
        field.field_ordinal = (uint16_t) (i < UINT16_MAX ? i : UINT16_MAX);
        field.name_id = hash_name32(key);
        field.type_key = obj->values ? body_expr_type_key(bc, obj->values[i]) : 0;
        field.flags = XG_JSON_FIELD_STATIC_KEY;
        if (field.type_key != 0)
            field.flags |= XG_JSON_FIELD_TYPED;
        (void) xg_global_evidence_add_json_field(bc->evidence, &field);
    }
}

static void body_add_json_fields_for_type_alias(XgGlobalEvidence *evidence, XgJsonShapeId shape_id,
                                                const TypeAliasNode *alias) {
    int field_count = body_type_alias_record_field_count(alias);
    if (!evidence || shape_id == XG_NO_ID || !alias)
        return;
    for (int i = 0; i < field_count; i++) {
        const char *name = body_type_alias_record_field_name(alias, i);
        const XrTypeRef *field_type = body_type_alias_record_field_type(alias, i);
        XgJsonFieldSummary field;
        if (!name)
            continue;
        memset(&field, 0, sizeof(field));
        field.field_id = (XgJsonFieldId) (evidence->njson_fields + 1);
        field.shape_id = shape_id;
        field.field_ordinal = (uint16_t) (i < UINT16_MAX ? i : UINT16_MAX);
        field.name_id = hash_name32(name);
        field.type_key = field_type ? hash_tref32(field_type) : 0;
        field.flags = XG_JSON_FIELD_STATIC_KEY | XG_JSON_FIELD_RECORD_BRIDGE;
        if (field.type_key != 0)
            field.flags |= XG_JSON_FIELD_TYPED;
        (void) xg_global_evidence_add_json_field(evidence, &field);
    }
}

static XgJsonShapeId body_add_json_shape_for_literal(XgBodyCollect *bc,
                                                     const ObjectLiteralNode *obj,
                                                     uint32_t source_span_id, uint32_t type_key) {
    XgJsonShapeSummary row;
    bool has_computed = false;
    if (!bc || !bc->evidence || !obj)
        return XG_NO_ID;
    for (int i = 0; i < obj->count; i++) {
        if (!body_object_literal_static_key(obj, i)) {
            has_computed = true;
            break;
        }
    }
    memset(&row, 0, sizeof(row));
    row.json_shape_id = (XgJsonShapeId) (bc->evidence->njson_shapes + 1);
    row.module_id = bc->module_id;
    row.owner_func_id = bc->owner_func_id;
    row.source_span_id = source_span_id;
    row.type_key = type_key;
    row.field_name_start = (uint32_t) (body_json_shape_hash(obj) & UINT32_MAX);
    row.field_count = (uint16_t) (obj->count < UINT16_MAX ? obj->count : UINT16_MAX);
    row.shape_kind = has_computed ? XG_JSON_SHAPE_OPEN : XG_JSON_SHAPE_SHAPED;
    row.flags = XG_JSON_SHAPE_MUTABLE;
    if (!has_computed)
        row.flags |= XG_JSON_SHAPE_STATIC_KEYS;
    else
        row.flags |= XG_JSON_SHAPE_HAS_COMPUTED_KEYS;
    row.shape_hash = body_json_shape_hash(obj);
    if (!xg_global_evidence_add_json_shape(bc->evidence, &row))
        return XG_NO_ID;
    body_add_json_fields_for_literal(bc, row.json_shape_id, obj);
    return row.json_shape_id;
}

static bool body_find_unique_return_json_literal(const AstNode *node,
                                                 const ObjectLiteralNode **out_literal,
                                                 bool *out_seen) {
    if (!node || !out_literal || !out_seen)
        return true;
    switch (node->type) {
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++) {
                if (!body_find_unique_return_json_literal(node->as.block.statements[i], out_literal,
                                                          out_seen))
                    return false;
            }
            return true;
        case AST_RETURN_STMT: {
            const ObjectLiteralNode *literal;
            if (node->as.return_stmt.value_count != 1 || !node->as.return_stmt.values)
                return false;
            literal = body_static_object_literal(node->as.return_stmt.values[0]);
            if (!literal)
                return false;
            if (*out_seen && *out_literal != literal)
                return false;
            *out_literal = literal;
            *out_seen = true;
            return true;
        }
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            return true;
        default:
            return true;
    }
}

static const ObjectLiteralNode *
body_unique_function_json_return_literal(const XgPendingBody *body) {
    const ObjectLiteralNode *literal = NULL;
    bool seen = false;
    if (!body || !body->function || !body_type_ref_is_json(body->function->return_type))
        return NULL;
    if (!body_find_unique_return_json_literal(body->body, &literal, &seen))
        return NULL;
    return seen ? literal : NULL;
}

static XgJsonShapeId body_lookup_call_json_return_shape(XgBodyCollect *bc, const AstNode *expr,
                                                        const ObjectLiteralNode **out_literal) {
    const AstNode *callee;
    XgFuncNameRow *target;
    const XgPendingBody *body;
    const ObjectLiteralNode *literal;
    uint32_t type_key;
    if (out_literal)
        *out_literal = NULL;
    if (!bc || !expr)
        return XG_NO_ID;
    switch (expr->type) {
        case AST_GROUPING:
            return body_lookup_call_json_return_shape(bc, expr->as.grouping, out_literal);
        case AST_MOVE_EXPR:
            return body_lookup_call_json_return_shape(bc, expr->as.move_expr.expr, out_literal);
        case AST_UNSAFE_EXPR:
            return body_lookup_call_json_return_shape(bc, expr->as.unsafe_expr.operand,
                                                      out_literal);
        case AST_FORCE_UNWRAP:
            return body_lookup_call_json_return_shape(bc, expr->as.unary.operand, out_literal);
        default:
            break;
    }
    if (expr->type != AST_CALL_EXPR)
        return XG_NO_ID;
    callee = expr->as.call_expr.callee;
    if (!callee || callee->type != AST_VARIABLE || !callee->as.variable.name)
        return XG_NO_ID;
    target = producer_lookup_func_row(bc->producer, callee->as.variable.name);
    body = producer_find_function_body(bc->producer, target ? target->func_id : XG_NO_ID);
    literal = body_unique_function_json_return_literal(body);
    if (!literal)
        return XG_NO_ID;
    type_key = body && body->function && body->function->return_type
                   ? hash_tref32(body->function->return_type)
                   : hash_named_type_key32("Json", NULL, 0);
    if (out_literal)
        *out_literal = literal;
    return body_add_json_shape_for_literal(bc, literal, (uint32_t) expr->line, type_key);
}

static XgJsonShapeId body_add_json_record_bridge_shape_for_type_alias(XgGlobalEvidence *evidence,
                                                                      XgModuleId module_id,
                                                                      const TypeAliasNode *alias,
                                                                      uint32_t source_span_id) {
    XgJsonShapeSummary row;
    uint32_t type_key;
    int field_count = body_type_alias_record_field_count(alias);
    if (!evidence || !alias || !alias->name || field_count <= 0)
        return XG_NO_ID;
    type_key = body_type_alias_record_type_key(alias);
    memset(&row, 0, sizeof(row));
    row.json_shape_id = (XgJsonShapeId) (evidence->njson_shapes + 1);
    row.module_id = module_id;
    row.owner_func_id = XG_NO_ID;
    row.source_span_id = source_span_id;
    row.type_key = type_key;
    row.field_name_start = body_type_alias_field_name_start(alias);
    row.field_count = (uint16_t) (field_count < UINT16_MAX ? field_count : UINT16_MAX);
    row.shape_kind = XG_JSON_SHAPE_RECORD_BRIDGE;
    row.flags = XG_JSON_SHAPE_STATIC_KEYS | XG_JSON_SHAPE_RECORD_BRIDGEABLE;
    row.shape_hash = body_type_alias_record_shape_hash(alias);
    if (!xg_global_evidence_add_json_shape(evidence, &row))
        return XG_NO_ID;
    body_add_json_fields_for_type_alias(evidence, row.json_shape_id, alias);
    return row.json_shape_id;
}

static void body_bind_json_shape_local(XgBodyCollect *bc, const char *name, XgJsonShapeId shape_id,
                                       const ObjectLiteralNode *literal) {
    XgLocalType *row;
    if (!bc || !name || shape_id == XG_NO_ID)
        return;
    row = body_find_local(bc, name);
    if (!row)
        return;
    row->json_shape_id = shape_id;
    row->json_shape_literal = literal;
}

static void body_clear_json_shape_local(XgBodyCollect *bc, const char *name) {
    XgLocalType *row;
    if (!bc || !name)
        return;
    row = body_find_local(bc, name);
    if (!row)
        return;
    row->json_shape_id = XG_NO_ID;
    row->json_shape_literal = NULL;
}

static XgJsonShapeId body_lookup_local_json_shape(XgBodyCollect *bc, const AstNode *expr,
                                                  const ObjectLiteralNode **out_literal) {
    XgLocalType *row;
    if (out_literal)
        *out_literal = NULL;
    if (!bc || !expr)
        return XG_NO_ID;
    switch (expr->type) {
        case AST_GROUPING:
            return body_lookup_local_json_shape(bc, expr->as.grouping, out_literal);
        case AST_MOVE_EXPR:
            return body_lookup_local_json_shape(bc, expr->as.move_expr.expr, out_literal);
        case AST_UNSAFE_EXPR:
            return body_lookup_local_json_shape(bc, expr->as.unsafe_expr.operand, out_literal);
        case AST_FORCE_UNWRAP:
            return body_lookup_local_json_shape(bc, expr->as.unary.operand, out_literal);
        default:
            break;
    }
    if (expr->type != AST_VARIABLE || !expr->as.variable.name)
        return XG_NO_ID;
    row = body_find_local(bc, expr->as.variable.name);
    if (!row || row->json_shape_id == XG_NO_ID)
        return XG_NO_ID;
    if (out_literal)
        *out_literal = row->json_shape_literal;
    return row->json_shape_id;
}

static XgJsonShapeId body_lookup_json_shape(XgBodyCollect *bc, const AstNode *expr,
                                            const ObjectLiteralNode **out_literal) {
    XgJsonShapeId shape_id = body_lookup_local_json_shape(bc, expr, out_literal);
    if (shape_id != XG_NO_ID)
        return shape_id;
    return body_lookup_call_json_return_shape(bc, expr, out_literal);
}

static int body_json_shape_static_field_index(XgBodyCollect *bc, XgJsonShapeId shape_id,
                                              const ObjectLiteralNode *literal, const char *name) {
    uint32_t name_id;
    if (!bc || !bc->evidence || shape_id == XG_NO_ID || !name)
        return -1;
    if (literal)
        return body_object_literal_static_field_index(literal, name);
    name_id = hash_name32(name);
    if (name_id == 0)
        return -1;
    for (uint32_t i = 0; i < bc->evidence->njson_fields; i++) {
        const XgJsonFieldSummary *field = &bc->evidence->json_fields[i];
        if (field->shape_id == shape_id && field->name_id == name_id)
            return field->field_ordinal;
    }
    return -1;
}

static bool body_local_type_is_json(const XgLocalType *row) {
    return row && row->type_key == hash_named_type_key32("Json", NULL, 0);
}

static bool body_expr_is_json_without_shape(XgBodyCollect *bc, const AstNode *expr) {
    XgLocalType *row;
    if (!bc || !expr)
        return false;
    switch (expr->type) {
        case AST_GROUPING:
            return body_expr_is_json_without_shape(bc, expr->as.grouping);
        case AST_MOVE_EXPR:
            return body_expr_is_json_without_shape(bc, expr->as.move_expr.expr);
        case AST_UNSAFE_EXPR:
            return body_expr_is_json_without_shape(bc, expr->as.unsafe_expr.operand);
        case AST_FORCE_UNWRAP:
            return body_expr_is_json_without_shape(bc, expr->as.unary.operand);
        case AST_CALL_EXPR:
            return body_type_ref_is_json(body_call_return_type_ref(bc, &expr->as.call_expr));
        default:
            break;
    }
    if (expr->type != AST_VARIABLE || !expr->as.variable.name)
        return false;
    row = body_find_local(bc, expr->as.variable.name);
    return row && row->json_shape_id == XG_NO_ID && body_local_type_is_json(row);
}

static void body_add_json_member_access(XgBodyCollect *bc, const AstNode *node, bool mutating) {
    const ObjectLiteralNode *literal = NULL;
    XgJsonShapeId shape_id;
    const AstNode *receiver;
    const char *name;
    int field_index;
    XgJsonAccessSummary row;
    if (!bc || !node)
        return;
    if (node->type == AST_MEMBER_ACCESS) {
        name = node->as.member_access.name;
        receiver = node->as.member_access.object;
        shape_id = body_lookup_json_shape(bc, receiver, &literal);
    } else if (node->type == AST_MEMBER_SET) {
        name = node->as.member_set.member;
        receiver = node->as.member_set.object;
        shape_id = body_lookup_json_shape(bc, receiver, &literal);
    } else {
        return;
    }
    if (!name)
        return;
    field_index =
        shape_id != XG_NO_ID ? body_json_shape_static_field_index(bc, shape_id, literal, name) : -1;
    if (shape_id == XG_NO_ID && !body_expr_is_json_without_shape(bc, receiver))
        return;
    if (shape_id != XG_NO_ID && field_index < 0)
        return;
    memset(&row, 0, sizeof(row));
    row.json_access_id = (XgJsonAccessId) (bc->evidence->njson_accesses + 1);
    row.module_id = bc->module_id;
    row.owner_func_id = bc->owner_func_id;
    row.receiver_shape_id = shape_id;
    row.source_span_id = (uint32_t) node->line;
    row.key_name_id = hash_name32(name);
    row.result_type_key = 0;
    row.field_ordinal = field_index >= 0 ? (uint16_t) field_index : UINT16_MAX;
    row.access_kind = mutating ? XG_JSON_ACCESS_FIELD_SET : XG_JSON_ACCESS_FIELD_GET;
    row.flags = XG_JSON_ACCESS_STATIC_KEY;
    if (shape_id != XG_NO_ID)
        row.flags |= XG_JSON_ACCESS_RECEIVER_SHAPE_PROVEN;
    if (mutating)
        row.flags |= XG_JSON_ACCESS_MUTATING;
    (void) xg_global_evidence_add_json_access(bc->evidence, &row);
}

static void body_add_json_index_access(XgBodyCollect *bc, const AstNode *node, bool mutating) {
    const ObjectLiteralNode *literal = NULL;
    const AstNode *receiver;
    const AstNode *key;
    const char *static_key;
    XgJsonShapeId shape_id;
    int field_index = -1;
    XgJsonAccessSummary row;
    if (!bc || !node)
        return;
    if (node->type == AST_INDEX_GET) {
        receiver = node->as.index_get.array;
        key = node->as.index_get.index;
    } else if (node->type == AST_INDEX_SET) {
        receiver = node->as.index_set.array;
        key = node->as.index_set.index;
    } else {
        return;
    }
    static_key = body_static_string_key(key);
    shape_id = body_lookup_json_shape(bc, receiver, &literal);
    if (shape_id == XG_NO_ID) {
        if (!static_key || !body_expr_is_json_without_shape(bc, receiver))
            return;
    } else if (static_key) {
        const XgJsonShapeSummary *shape =
            xg_global_evidence_find_json_shape(bc->evidence, shape_id);
        if (!shape)
            return;
        field_index = body_json_shape_static_field_index(bc, shape_id, literal, static_key);
        if (field_index < 0)
            return;
    }
    if (!static_key && shape_id == XG_NO_ID)
        return;
    memset(&row, 0, sizeof(row));
    row.json_access_id = (XgJsonAccessId) (bc->evidence->njson_accesses + 1);
    row.module_id = bc->module_id;
    row.owner_func_id = bc->owner_func_id;
    row.receiver_shape_id = shape_id;
    row.source_span_id = (uint32_t) node->line;
    row.key_name_id = static_key ? hash_name32(static_key) : 0;
    row.result_type_key = 0;
    row.field_ordinal = field_index >= 0 ? (uint16_t) field_index : UINT16_MAX;
    row.access_kind = mutating ? XG_JSON_ACCESS_INDEX_SET : XG_JSON_ACCESS_INDEX_GET;
    row.flags = 0;
    if (static_key)
        row.flags |= XG_JSON_ACCESS_STATIC_KEY;
    else
        row.flags |= XG_JSON_ACCESS_COMPUTED_KEY;
    if (shape_id != XG_NO_ID)
        row.flags |= XG_JSON_ACCESS_RECEIVER_SHAPE_PROVEN;
    if (mutating)
        row.flags |= XG_JSON_ACCESS_MUTATING;
    (void) xg_global_evidence_add_json_access(bc->evidence, &row);
}

static const char *body_json_static_method_name(const AstNode *callee) {
    const MemberAccessNode *member;
    if (!callee || callee->type != AST_MEMBER_ACCESS)
        return NULL;
    member = &callee->as.member_access;
    if (!member->name || !member->object || member->object->type != AST_VARIABLE ||
        !member->object->as.variable.name)
        return NULL;
    return strcmp(member->object->as.variable.name, "Json") == 0 ? member->name : NULL;
}

static void body_add_json_codec_call(XgBodyCollect *bc, const AstNode *node) {
    const CallExprNode *call;
    const char *method;
    XgJsonCodecSummary row;
    const AstNode *arg0 = NULL;
    if (!bc || !bc->evidence || !node || node->type != AST_CALL_EXPR)
        return;
    call = &node->as.call_expr;
    method = body_json_static_method_name(call->callee);
    if (!method)
        return;

    memset(&row, 0, sizeof(row));
    row.codec_id = (XgJsonCodecId) (bc->evidence->njson_codecs + 1);
    row.module_id = bc->module_id;
    row.owner_func_id = bc->owner_func_id;
    row.source_span_id = (uint32_t) node->line;
    if (call->arg_count > 0)
        arg0 = call->arguments[0];
    if (arg0)
        row.input_type_key = body_expr_type_key(bc, arg0);

    if (strcmp(method, "parse") == 0) {
        row.codec_kind = XG_JSON_CODEC_PARSE;
        if (arg0 && arg0->type == AST_LITERAL_STRING)
            row.flags |= XG_JSON_CODEC_STATIC_TEXT;
    } else if (strcmp(method, "decode") == 0) {
        row.codec_kind = XG_JSON_CODEC_DECODE;
        if (call->type_arg_count > 0 && call->type_args && call->type_args[0]) {
            row.target_type_key = hash_tref32(call->type_args[0]);
            row.flags |= XG_JSON_CODEC_HAS_TARGET_TYPE;
            const XgJsonShapeSummary *target_shape = body_find_json_shape_for_type_key(
                bc, row.target_type_key, XG_JSON_SHAPE_RECORD_BRIDGE);
            if (target_shape) {
                row.output_shape_id = target_shape->json_shape_id;
                row.field_count = target_shape->field_count;
                row.flags |= XG_JSON_CODEC_HAS_OUTPUT_SHAPE;
            }
        }
        if (arg0) {
            const ObjectLiteralNode *literal = NULL;
            row.input_shape_id = body_lookup_local_json_shape(bc, arg0, &literal);
            if (row.input_shape_id != XG_NO_ID) {
                const XgJsonShapeSummary *shape =
                    xg_global_evidence_find_json_shape(bc->evidence, row.input_shape_id);
                row.flags |= XG_JSON_CODEC_HAS_INPUT_SHAPE;
                if (shape && row.field_count == 0)
                    row.field_count = shape->field_count;
            }
        }
    } else if (strcmp(method, "encode") == 0) {
        row.codec_kind = XG_JSON_CODEC_ENCODE;
        if (arg0) {
            const ObjectLiteralNode *literal = NULL;
            row.input_shape_id = body_lookup_local_json_shape(bc, arg0, &literal);
            if (row.input_shape_id != XG_NO_ID) {
                const XgJsonShapeSummary *shape =
                    xg_global_evidence_find_json_shape(bc->evidence, row.input_shape_id);
                row.flags |= XG_JSON_CODEC_HAS_INPUT_SHAPE;
                if (shape)
                    row.field_count = shape->field_count;
            }
        }
    } else if (strcmp(method, "stringify") == 0) {
        row.codec_kind = XG_JSON_CODEC_STRINGIFY;
        if (arg0) {
            const ObjectLiteralNode *literal = NULL;
            row.input_shape_id = body_lookup_local_json_shape(bc, arg0, &literal);
            if (row.input_shape_id != XG_NO_ID) {
                const XgJsonShapeSummary *shape =
                    xg_global_evidence_find_json_shape(bc->evidence, row.input_shape_id);
                row.flags |= XG_JSON_CODEC_HAS_INPUT_SHAPE;
                if (shape)
                    row.field_count = shape->field_count;
            }
        }
    } else {
        return;
    }

    (void) xg_global_evidence_add_json_codec(bc->evidence, &row);
}

static uint32_t body_record_type_key(const ObjectLiteralNode *obj) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    static const char tag[] = "Record";
    h = fold_bytes(h, tag, sizeof(tag) - 1);
    h = fold_u64(h, body_json_shape_hash(obj));
    return hash_folded32(h);
}

static XgRecordShapeId body_lookup_local_record_shape(XgBodyCollect *bc, const AstNode *expr,
                                                      const ObjectLiteralNode **out_literal);

static int record_shape_key_index(const char **keys, uint32_t count, const char *key) {
    if (!keys || !key)
        return -1;
    for (uint32_t i = 0; i < count; i++) {
        if (keys[i] && strcmp(keys[i], key) == 0)
            return (int) i;
    }
    return -1;
}

static bool record_shape_add_key(const char **keys, uint32_t capacity, uint32_t *count,
                                 const char *key) {
    if (!keys || !count || !key)
        return false;
    if (record_shape_key_index(keys, *count, key) >= 0)
        return true;
    if (*count >= capacity)
        return false;
    keys[(*count)++] = key;
    return true;
}

static bool record_shape_count_candidate_keys(XgBodyCollect *bc, const ObjectLiteralNode *obj,
                                              uint32_t *count, uint32_t depth) {
    if (!obj || !count || depth > 16)
        return false;
    for (int i = 0; i < obj->count; i++) {
        if (body_object_literal_entry_is_spread(obj, i)) {
            const ObjectLiteralNode *source_literal = NULL;
            AstNode *spread = obj->values[i];
            if (!spread ||
                body_lookup_local_record_shape(bc, spread->as.spread_expr.expr, &source_literal) ==
                    XG_NO_ID ||
                !source_literal)
                return false;
            if (!record_shape_count_candidate_keys(bc, source_literal, count, depth + 1))
                return false;
        } else if (!body_object_literal_static_key(obj, i)) {
            return false;
        } else {
            (*count)++;
        }
    }
    return true;
}

static bool record_shape_collect_keys(XgBodyCollect *bc, const ObjectLiteralNode *obj,
                                      const char **keys, uint32_t capacity, uint32_t *count,
                                      bool *has_spread, uint32_t depth) {
    if (!obj || (!keys && capacity > 0) || !count || depth > 16)
        return false;
    for (int i = 0; i < obj->count; i++) {
        if (body_object_literal_entry_is_spread(obj, i)) {
            const ObjectLiteralNode *source_literal = NULL;
            AstNode *spread = obj->values[i];
            if (has_spread)
                *has_spread = true;
            if (!spread ||
                body_lookup_local_record_shape(bc, spread->as.spread_expr.expr, &source_literal) ==
                    XG_NO_ID ||
                !source_literal)
                return false;
            if (!record_shape_collect_keys(bc, source_literal, keys, capacity, count, has_spread,
                                           depth + 1))
                return false;
        } else {
            const char *key = body_object_literal_static_key(obj, i);
            if (!key || !record_shape_add_key(keys, capacity, count, key))
                return false;
        }
    }
    return true;
}

static bool record_shape_collect_literal_keys(XgBodyCollect *bc, const ObjectLiteralNode *obj,
                                              const char ***out_keys, uint32_t *out_count,
                                              bool *out_has_spread, uint64_t *out_hash) {
    uint32_t capacity = 0;
    uint32_t count = 0;
    bool has_spread = false;
    const char **keys = NULL;
    uint64_t hash = XR_FNV64_OFFSET_BASIS;
    static const char tag[] = "RecordShape";
    if (!out_keys || !out_count || !out_has_spread || !out_hash)
        return false;
    *out_keys = NULL;
    *out_count = 0;
    *out_has_spread = false;
    *out_hash = 0;
    if (!record_shape_count_candidate_keys(bc, obj, &capacity, 0))
        return false;
    if (capacity > 0) {
        keys = (const char **) xr_calloc((size_t) capacity, sizeof(*keys));
        if (!keys)
            return false;
    }
    if (!record_shape_collect_keys(bc, obj, keys, capacity, &count, &has_spread, 0)) {
        xr_free(keys);
        return false;
    }
    hash = fold_bytes(hash, tag, sizeof(tag) - 1);
    for (uint32_t i = 0; i < count; i++) {
        const char *key = keys[i] ? keys[i] : "";
        hash = fold_bytes(hash, key, strlen(key));
    }
    *out_keys = keys;
    *out_count = count;
    *out_has_spread = has_spread;
    *out_hash = hash;
    return true;
}

static bool record_patch_collect_literal_keys(const ObjectLiteralNode *obj, const char ***out_keys,
                                              uint32_t *out_count, uint64_t *out_hash) {
    uint32_t capacity = 0;
    uint32_t count = 0;
    const char **keys = NULL;
    uint64_t hash = XR_FNV64_OFFSET_BASIS;
    static const char tag[] = "RecordPatchShape";
    if (!out_keys || !out_count || !out_hash)
        return false;
    *out_keys = NULL;
    *out_count = 0;
    *out_hash = 0;
    if (!obj)
        return false;
    for (int i = 0; i < obj->count; i++) {
        if (body_object_literal_entry_is_spread(obj, i))
            continue;
        if (!body_object_literal_static_key(obj, i))
            return false;
        capacity++;
    }
    if (capacity > 0) {
        keys = (const char **) xr_calloc((size_t) capacity, sizeof(*keys));
        if (!keys)
            return false;
    }
    for (int i = 0; i < obj->count; i++) {
        const char *key;
        if (body_object_literal_entry_is_spread(obj, i))
            continue;
        key = body_object_literal_static_key(obj, i);
        if (!key || !record_shape_add_key(keys, capacity, &count, key)) {
            xr_free(keys);
            return false;
        }
    }
    hash = fold_bytes(hash, tag, sizeof(tag) - 1);
    for (uint32_t i = 0; i < count; i++) {
        const char *key = keys[i] ? keys[i] : "";
        hash = fold_bytes(hash, key, strlen(key));
    }
    *out_keys = keys;
    *out_count = count;
    *out_hash = hash;
    return true;
}

static uint16_t record_patch_overwrite_count(const ObjectLiteralNode *source_literal,
                                             const char **patch_keys, uint32_t patch_key_count) {
    uint32_t count = 0;
    if (!source_literal || !patch_keys)
        return 0;
    for (uint32_t i = 0; i < patch_key_count; i++) {
        if (patch_keys[i] &&
            body_object_literal_static_field_index(source_literal, patch_keys[i]) >= 0)
            count++;
    }
    return (uint16_t) (count < UINT16_MAX ? count : UINT16_MAX);
}

static void body_add_record_fields_for_keys(XgBodyCollect *bc, XgRecordShapeId shape_id,
                                            const char **keys, uint32_t key_count,
                                            uint32_t base_flags) {
    if (!bc || !bc->evidence || shape_id == XG_NO_ID || (!keys && key_count > 0))
        return;
    for (uint32_t i = 0; i < key_count; i++) {
        XgRecordFieldSummary field;
        if (!keys[i])
            continue;
        memset(&field, 0, sizeof(field));
        field.field_id = (XgRecordFieldId) (bc->evidence->nrecord_fields + 1);
        field.shape_id = shape_id;
        field.field_ordinal = (uint16_t) (i < UINT16_MAX ? i : UINT16_MAX);
        field.name_id = hash_name32(keys[i]);
        field.flags = base_flags | XG_RECORD_FIELD_STATIC_KEY;
        (void) xg_global_evidence_add_record_field(bc->evidence, &field);
    }
}

static void body_add_record_fields_for_type_alias(XgGlobalEvidence *evidence,
                                                  XgRecordShapeId shape_id,
                                                  const TypeAliasNode *alias) {
    int field_count = body_type_alias_record_field_count(alias);
    if (!evidence || shape_id == XG_NO_ID || !alias)
        return;
    for (int i = 0; i < field_count; i++) {
        const char *name = body_type_alias_record_field_name(alias, i);
        const XrTypeRef *field_type = body_type_alias_record_field_type(alias, i);
        XgRecordFieldSummary field;
        if (!name)
            continue;
        memset(&field, 0, sizeof(field));
        field.field_id = (XgRecordFieldId) (evidence->nrecord_fields + 1);
        field.shape_id = shape_id;
        field.field_ordinal = (uint16_t) (i < UINT16_MAX ? i : UINT16_MAX);
        field.name_id = hash_name32(name);
        field.type_key = field_type ? hash_tref32(field_type) : 0;
        field.flags = XG_RECORD_FIELD_STATIC_KEY;
        if (body_type_alias_record_field_optional(alias, i))
            field.flags |= XG_RECORD_FIELD_OPTIONAL;
        else
            field.flags |= XG_RECORD_FIELD_REQUIRED;
        if (body_type_alias_record_field_readonly(alias, i))
            field.flags |= XG_RECORD_FIELD_READONLY;
        (void) xg_global_evidence_add_record_field(evidence, &field);
    }
}

static XgRecordShapeId body_add_record_patch_shape(XgBodyCollect *bc, uint32_t source_span_id,
                                                   const char **patch_keys,
                                                   uint32_t patch_key_count, uint64_t patch_hash) {
    XgRecordShapeSummary row;
    if (!bc || !bc->evidence)
        return XG_NO_ID;
    memset(&row, 0, sizeof(row));
    row.record_shape_id = (XgRecordShapeId) (bc->evidence->nrecord_shapes + 1);
    row.module_id = bc->module_id;
    row.owner_func_id = bc->owner_func_id;
    row.source_span_id = source_span_id;
    row.type_key = hash_folded32(fold_u64(patch_hash, patch_key_count));
    row.field_name_start = (uint32_t) (patch_hash & UINT32_MAX);
    row.field_count = (uint16_t) (patch_key_count < UINT16_MAX ? patch_key_count : UINT16_MAX);
    row.shape_kind = XG_RECORD_SHAPE_PATCH;
    row.flags =
        XG_RECORD_SHAPE_SEALED | XG_RECORD_SHAPE_STATIC_KEYS | XG_RECORD_SHAPE_JSON_BRIDGEABLE;
    row.shape_hash = patch_hash;
    if (!xg_global_evidence_add_record_shape(bc->evidence, &row))
        return XG_NO_ID;
    body_add_record_fields_for_keys(bc, row.record_shape_id, patch_keys, patch_key_count, 0);
    return row.record_shape_id;
}

static void body_add_record_merge_rows_for_literal(XgBodyCollect *bc, const ObjectLiteralNode *obj,
                                                   XgRecordShapeId result_shape_id,
                                                   uint32_t result_field_count,
                                                   uint64_t result_shape_hash,
                                                   uint32_t source_span_id) {
    const char **patch_keys = NULL;
    uint32_t patch_key_count = 0;
    uint64_t patch_hash = 0;
    XgRecordShapeId patch_shape_id = XG_NO_ID;
    const XgRecordShapeSummary *patch_shape = NULL;
    const XgRecordShapeSummary *result_shape = NULL;
    if (!bc || !bc->evidence || !obj || result_shape_id == XG_NO_ID)
        return;
    if (!record_patch_collect_literal_keys(obj, &patch_keys, &patch_key_count, &patch_hash))
        return;
    result_shape = xg_global_evidence_find_record_shape(bc->evidence, result_shape_id);
    if (!result_shape)
        goto done;
    for (int i = 0; i < obj->count; i++) {
        AstNode *spread;
        const ObjectLiteralNode *source_literal = NULL;
        XgRecordShapeId base_shape_id;
        const XgRecordShapeSummary *base_shape;
        XgRecordMergeSummary row;
        uint16_t overwrites;
        uint64_t merge_hash;
        static const char tag[] = "RecordMerge";
        if (!body_object_literal_entry_is_spread(obj, i))
            continue;
        spread = obj->values[i];
        if (!spread)
            continue;
        base_shape_id =
            body_lookup_local_record_shape(bc, spread->as.spread_expr.expr, &source_literal);
        if (base_shape_id == XG_NO_ID || !source_literal)
            continue;
        if (patch_shape_id == XG_NO_ID) {
            patch_shape_id = body_add_record_patch_shape(bc, source_span_id, patch_keys,
                                                         patch_key_count, patch_hash);
            patch_shape = xg_global_evidence_find_record_shape(bc->evidence, patch_shape_id);
            if (!patch_shape)
                break;
        }
        base_shape = xg_global_evidence_find_record_shape(bc->evidence, base_shape_id);
        if (!base_shape)
            continue;
        overwrites = record_patch_overwrite_count(source_literal, patch_keys, patch_key_count);
        merge_hash = XR_FNV64_OFFSET_BASIS;
        merge_hash = fold_bytes(merge_hash, tag, sizeof(tag) - 1);
        merge_hash = fold_u64(merge_hash, base_shape->shape_hash);
        merge_hash = fold_u64(merge_hash, patch_hash);
        merge_hash = fold_u64(merge_hash, result_shape_hash);
        merge_hash = fold_u64(merge_hash, overwrites);
        memset(&row, 0, sizeof(row));
        row.merge_id = (XgRecordMergeId) (bc->evidence->nrecord_merges + 1);
        row.module_id = bc->module_id;
        row.owner_func_id = bc->owner_func_id;
        row.source_span_id = source_span_id;
        row.base_shape_id = base_shape_id;
        row.patch_shape_id = patch_shape_id;
        row.result_shape_id = result_shape_id;
        row.base_field_count = base_shape->field_count;
        row.patch_field_count = patch_shape->field_count;
        row.result_field_count =
            (uint16_t) (result_field_count < UINT16_MAX ? result_field_count : UINT16_MAX);
        row.overwrite_count = overwrites;
        row.copy_table_id = (uint32_t) (merge_hash & UINT32_MAX);
        row.flags = XG_RECORD_MERGE_BASE_SHAPE_PROVEN | XG_RECORD_MERGE_PATCH_SHAPE_PROVEN |
                    XG_RECORD_MERGE_RESULT_SHAPE_PROVEN;
        if (overwrites != 0)
            row.flags |= XG_RECORD_MERGE_OVERWRITES;
        row.merge_hash = merge_hash;
        (void) xg_global_evidence_add_record_merge(bc->evidence, &row);
    }

done:
    xr_free(patch_keys);
}

static XgRecordShapeId body_add_record_shape_for_literal(XgBodyCollect *bc,
                                                         const ObjectLiteralNode *obj,
                                                         uint32_t source_span_id,
                                                         uint32_t type_key) {
    XgRecordShapeSummary row;
    const char **shape_keys = NULL;
    uint32_t shape_key_count = 0;
    bool has_spread = false;
    uint64_t shape_hash = 0;
    if (!bc || !bc->evidence || !obj)
        return XG_NO_ID;
    if (!record_shape_collect_literal_keys(bc, obj, &shape_keys, &shape_key_count, &has_spread,
                                           &shape_hash))
        return XG_NO_ID;
    memset(&row, 0, sizeof(row));
    row.record_shape_id = (XgRecordShapeId) (bc->evidence->nrecord_shapes + 1);
    row.module_id = bc->module_id;
    row.owner_func_id = bc->owner_func_id;
    row.source_span_id = source_span_id;
    row.type_key = type_key ? type_key : body_record_type_key(obj);
    row.field_name_start = (uint32_t) (shape_hash & UINT32_MAX);
    row.field_count = (uint16_t) (shape_key_count < UINT16_MAX ? shape_key_count : UINT16_MAX);
    row.shape_kind = has_spread ? XG_RECORD_SHAPE_SPREAD : XG_RECORD_SHAPE_LITERAL;
    row.flags =
        XG_RECORD_SHAPE_SEALED | XG_RECORD_SHAPE_STATIC_KEYS | XG_RECORD_SHAPE_JSON_BRIDGEABLE;
    if (has_spread)
        row.flags |= XG_RECORD_SHAPE_HAS_SPREAD;
    row.shape_hash = shape_hash;
    if (!xg_global_evidence_add_record_shape(bc->evidence, &row)) {
        xr_free(shape_keys);
        return XG_NO_ID;
    }
    if (has_spread)
        body_add_record_merge_rows_for_literal(bc, obj, row.record_shape_id, shape_key_count,
                                               shape_hash, source_span_id);
    body_add_record_fields_for_keys(bc, row.record_shape_id, shape_keys, shape_key_count,
                                    XG_RECORD_FIELD_REQUIRED);
    xr_free(shape_keys);
    return row.record_shape_id;
}

static XgRecordShapeId body_add_record_shape_for_type_alias(XgGlobalEvidence *evidence,
                                                            XgModuleId module_id,
                                                            const TypeAliasNode *alias,
                                                            uint32_t source_span_id) {
    XgRecordShapeSummary row;
    uint32_t type_key;
    int field_count = body_type_alias_record_field_count(alias);
    if (!evidence || !alias || !alias->name || field_count <= 0)
        return XG_NO_ID;
    type_key = body_type_alias_record_type_key(alias);
    memset(&row, 0, sizeof(row));
    row.record_shape_id = (XgRecordShapeId) (evidence->nrecord_shapes + 1);
    row.module_id = module_id;
    row.owner_func_id = XG_NO_ID;
    row.source_span_id = source_span_id;
    row.type_key = type_key;
    row.field_name_start = body_type_alias_field_name_start(alias);
    row.field_count = (uint16_t) (field_count < UINT16_MAX ? field_count : UINT16_MAX);
    row.shape_kind = XG_RECORD_SHAPE_STATIC;
    row.flags =
        XG_RECORD_SHAPE_SEALED | XG_RECORD_SHAPE_STATIC_KEYS | XG_RECORD_SHAPE_JSON_BRIDGEABLE;
    row.shape_hash = body_type_alias_record_shape_hash(alias);
    if (!xg_global_evidence_add_record_shape(evidence, &row))
        return XG_NO_ID;
    body_add_record_fields_for_type_alias(evidence, row.record_shape_id, alias);
    return row.record_shape_id;
}

static void body_bind_record_shape_local(XgBodyCollect *bc, const char *name,
                                         XgRecordShapeId shape_id,
                                         const ObjectLiteralNode *literal) {
    XgLocalType *row;
    if (!bc || !name || shape_id == XG_NO_ID)
        return;
    row = body_find_local(bc, name);
    if (!row)
        return;
    row->record_shape_id = shape_id;
    row->record_shape_literal = literal;
}

static void body_bind_record_bridge_shapes_for_type_key(XgBodyCollect *bc, const char *name,
                                                        uint32_t type_key) {
    const XgJsonShapeSummary *json_shape;
    const XgRecordShapeSummary *record_shape;
    if (!bc || !name || type_key == 0)
        return;
    json_shape = body_find_json_shape_for_type_key(bc, type_key, XG_JSON_SHAPE_RECORD_BRIDGE);
    if (json_shape)
        body_bind_json_shape_local(bc, name, json_shape->json_shape_id, NULL);
    record_shape = body_find_record_shape_for_type_key(bc, type_key, XG_RECORD_SHAPE_STATIC);
    if (record_shape && (record_shape->flags & XG_RECORD_SHAPE_JSON_BRIDGEABLE) != 0)
        body_bind_record_shape_local(bc, name, record_shape->record_shape_id, NULL);
}

static void body_clear_record_shape_local(XgBodyCollect *bc, const char *name) {
    XgLocalType *row;
    if (!bc || !name)
        return;
    row = body_find_local(bc, name);
    if (!row)
        return;
    row->record_shape_id = XG_NO_ID;
    row->record_shape_literal = NULL;
}

static XgRecordShapeId body_lookup_local_record_shape(XgBodyCollect *bc, const AstNode *expr,
                                                      const ObjectLiteralNode **out_literal) {
    XgLocalType *row;
    if (out_literal)
        *out_literal = NULL;
    if (!bc || !expr)
        return XG_NO_ID;
    switch (expr->type) {
        case AST_GROUPING:
            return body_lookup_local_record_shape(bc, expr->as.grouping, out_literal);
        case AST_MOVE_EXPR:
            return body_lookup_local_record_shape(bc, expr->as.move_expr.expr, out_literal);
        case AST_UNSAFE_EXPR:
            return body_lookup_local_record_shape(bc, expr->as.unsafe_expr.operand, out_literal);
        case AST_FORCE_UNWRAP:
            return body_lookup_local_record_shape(bc, expr->as.unary.operand, out_literal);
        default:
            break;
    }
    if (expr->type != AST_VARIABLE || !expr->as.variable.name)
        return XG_NO_ID;
    row = body_find_local(bc, expr->as.variable.name);
    if (!row || row->record_shape_id == XG_NO_ID)
        return XG_NO_ID;
    if (out_literal)
        *out_literal = row->record_shape_literal;
    return row->record_shape_id;
}

static void body_add_record_member_access(XgBodyCollect *bc, const AstNode *node, bool mutating) {
    const ObjectLiteralNode *literal = NULL;
    XgRecordShapeId shape_id;
    const char *name;
    int field_index;
    XgRecordAccessSummary row;
    if (!bc || !node)
        return;
    if (node->type == AST_MEMBER_ACCESS) {
        name = node->as.member_access.name;
        shape_id = body_lookup_local_record_shape(bc, node->as.member_access.object, &literal);
    } else if (node->type == AST_MEMBER_SET) {
        name = node->as.member_set.member;
        shape_id = body_lookup_local_record_shape(bc, node->as.member_set.object, &literal);
    } else {
        return;
    }
    if (shape_id == XG_NO_ID || !name)
        return;
    field_index = body_object_literal_static_field_index(literal, name);
    if (field_index < 0)
        return;
    memset(&row, 0, sizeof(row));
    row.record_access_id = (XgRecordAccessId) (bc->evidence->nrecord_accesses + 1);
    row.module_id = bc->module_id;
    row.owner_func_id = bc->owner_func_id;
    row.receiver_shape_id = shape_id;
    row.source_span_id = (uint32_t) node->line;
    row.field_name_id = hash_name32(name);
    row.result_type_key = 0;
    row.field_ordinal = (uint16_t) field_index;
    row.access_kind = mutating ? XG_RECORD_ACCESS_FIELD_SET : XG_RECORD_ACCESS_FIELD_GET;
    row.flags = XG_RECORD_ACCESS_STATIC_FIELD | XG_RECORD_ACCESS_RECEIVER_SHAPE_PROVEN;
    if (mutating)
        row.flags |= XG_RECORD_ACCESS_MUTATING;
    (void) xg_global_evidence_add_record_access(bc->evidence, &row);
}

static uint32_t body_const_expr_id(const AstNode *expr) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    if (!expr)
        return 0;
    h = fold_u64(h, (uint64_t) expr->type);
    switch (expr->type) {
        case AST_LITERAL_STRING:
            return xg_name_id(expr->as.literal.raw_value.string_val);
        case AST_LITERAL_INT:
            h = fold_u64(h, expr->as.literal.int_bits);
            h = fold_u64(h, expr->as.literal.int_overflows_i64 ? 1 : 0);
            return hash_folded32(h);
        case AST_LITERAL_CHAR:
            h = fold_u64(h, expr->as.literal.raw_value.char_val);
            return hash_folded32(h);
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
            h = fold_u64(h, expr->as.literal.raw_value.bool_val ? 1 : 0);
            return hash_folded32(h);
        case AST_LITERAL_NULL:
            h = fold_u64(h, 0);
            return hash_folded32(h);
        default:
            return 0;
    }
}

static uint32_t body_map_runtime_hash_f64(double value) {
    uint64_t bits;
    if (value == 0.0)
        value = 0.0;
    memcpy(&bits, &value, sizeof(bits));
    return (uint32_t) xr_hash_core_mix_u64(bits);
}

static uint64_t body_map_const_prehash(const AstNode *expr) {
    const char *s;
    uint32_t string_hash;
    if (!expr)
        return 0;
    switch (expr->type) {
        case AST_LITERAL_STRING:
            s = expr->as.literal.raw_value.string_val;
            if (!s)
                return 0;
            string_hash = xr_hash_core_str_hash_bytes(s, strlen(s));
            return (uint32_t) xr_hash_core_mix_u64((uint64_t) string_hash);
        case AST_LITERAL_INT:
            return (uint32_t) xr_hash_core_mix_u64((uint64_t) expr->as.literal.int_bits);
        case AST_LITERAL_FLOAT:
            return body_map_runtime_hash_f64(expr->as.literal.raw_value.float_val);
        case AST_LITERAL_CHAR:
            return (uint32_t) xr_hash_core_mix_u64((uint64_t) expr->as.literal.raw_value.char_val);
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
            return (uint32_t) xr_hash_core_mix_u64(
                (uint64_t) (expr->as.literal.raw_value.bool_val ? 1 : 0));
        case AST_LITERAL_NULL:
            return (uint32_t) xr_hash_core_mix_u64(UINT64_C(0x9e3779b97f4a7c15));
        default:
            return 0;
    }
}

static bool body_map_key_type_is_dense_int(uint32_t key_type_key) {
    static const uint8_t int_widths[] = {
        XR_TREF_NW_I64, XR_TREF_NW_I8,  XR_TREF_NW_I16, XR_TREF_NW_I32,   XR_TREF_NW_U8,
        XR_TREF_NW_U16, XR_TREF_NW_U32, XR_TREF_NW_U64, XR_TREF_NW_ISIZE, XR_TREF_NW_USIZE,
    };
    if (key_type_key == hash_synthetic_tref32(XR_TREF_INT, NULL, NULL, 0))
        return true;
    for (uint32_t i = 0; i < sizeof(int_widths) / sizeof(int_widths[0]); i++) {
        if (key_type_key == hash_synthetic_width_tref32(XR_TREF_INT_WIDTH, int_widths[i]))
            return true;
    }
    return false;
}

static bool body_map_key_type_is_bool(uint32_t key_type_key) {
    return key_type_key == hash_synthetic_tref32(XR_TREF_BOOL, NULL, NULL, 0);
}

static bool body_map_value_type_supports_bool_direct(uint32_t value_type_key) {
    return value_type_key == hash_synthetic_tref32(XR_TREF_INT, NULL, NULL, 0) ||
           value_type_key == hash_synthetic_width_tref32(XR_TREF_INT_WIDTH, XR_TREF_NW_I64) ||
           value_type_key == hash_synthetic_width_tref32(XR_TREF_FLOAT_WIDTH, XR_TREF_NW_F32);
}

static bool body_map_literal_has_bool_domain(AstNode **keys, int count, uint32_t key_type_key) {
    if (!keys || count <= 0 || !body_map_key_type_is_bool(key_type_key))
        return false;
    for (int i = 0; i < count; i++) {
        const AstNode *key = keys[i];
        if (!key || (key->type != AST_LITERAL_TRUE && key->type != AST_LITERAL_FALSE))
            return false;
    }
    return true;
}

static bool body_map_literal_has_dense_i64_domain(AstNode **keys, int count,
                                                  uint32_t key_type_key) {
    if (!keys || count <= 0 || !body_map_key_type_is_dense_int(key_type_key))
        return false;
    for (int i = 0; i < count; i++) {
        const AstNode *key = keys[i];
        if (!key || key->type != AST_LITERAL_INT || key->as.literal.int_overflows_i64)
            return false;
        if (key->as.literal.raw_value.int_val != (int64_t) i)
            return false;
    }
    return true;
}

static uint32_t body_map_receiver_type_key(uint8_t container_kind, uint32_t key_type_key,
                                           uint32_t value_type_key) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    const char *name = container_kind == XG_MAP_CONTAINER_SET ? "Set" : "Map";
    h = fold_bytes(h, name, strlen(name));
    h = fold_u64(h, key_type_key);
    h = fold_u64(h, value_type_key);
    return hash_folded32(h);
}

static uint64_t body_map_shape_hash(uint8_t container_kind, uint32_t key_type_key,
                                    uint32_t value_type_key, const uint32_t *const_ids, int count) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    h = fold_u64(h, container_kind);
    h = fold_u64(h, key_type_key);
    h = fold_u64(h, value_type_key);
    h = fold_u64(h, (uint64_t) (count > 0 ? count : 0));
    for (int i = 0; i < count; i++)
        h = fold_u64(h, const_ids ? const_ids[i] : 0);
    return h ? h : 1;
}

static bool body_map_key_type_has_builtin_hash_eq(uint32_t key_type_key) {
    static const uint8_t builtin_kinds[] = {
        XR_TREF_INT, XR_TREF_FLOAT, XR_TREF_STRING, XR_TREF_CHAR, XR_TREF_BOOL,
    };
    static const uint8_t int_widths[] = {
        XR_TREF_NW_I64, XR_TREF_NW_I8,  XR_TREF_NW_I16, XR_TREF_NW_I32,   XR_TREF_NW_U8,
        XR_TREF_NW_U16, XR_TREF_NW_U32, XR_TREF_NW_U64, XR_TREF_NW_ISIZE, XR_TREF_NW_USIZE,
    };
    static const uint8_t float_widths[] = {
        XR_TREF_NW_F64,
        XR_TREF_NW_F32,
    };
    if (key_type_key == 0)
        return false;
    for (uint32_t i = 0; i < sizeof(builtin_kinds) / sizeof(builtin_kinds[0]); i++) {
        if (key_type_key == hash_synthetic_tref32((uint8_t) builtin_kinds[i], NULL, NULL, 0))
            return true;
    }
    for (uint32_t i = 0; i < sizeof(int_widths) / sizeof(int_widths[0]); i++) {
        if (key_type_key == hash_synthetic_width_tref32(XR_TREF_INT_WIDTH, int_widths[i]))
            return true;
    }
    for (uint32_t i = 0; i < sizeof(float_widths) / sizeof(float_widths[0]); i++) {
        if (key_type_key == hash_synthetic_width_tref32(XR_TREF_FLOAT_WIDTH, float_widths[i]))
            return true;
    }
    return false;
}

static void body_ensure_builtin_hash_eq(XgBodyCollect *bc, uint32_t key_type_key) {
    XgHashEqSummary row;
    if (!bc || !bc->evidence || !body_map_key_type_has_builtin_hash_eq(key_type_key))
        return;
    if (xg_global_evidence_find_hash_eq(bc->evidence, key_type_key))
        return;
    memset(&row, 0, sizeof(row));
    row.hash_eq_id = (XgHashEqId) (bc->evidence->nhash_eqs + 1);
    row.type_key = key_type_key;
    row.kind = XG_HASH_EQ_BUILTIN;
    row.flags = XG_HASH_EQ_NO_ALLOC | XG_HASH_EQ_NO_THROW | XG_HASH_EQ_PURE | XG_HASH_EQ_FINAL;
    (void) xg_global_evidence_add_hash_eq(bc->evidence, &row);
}

static bool body_type_ref_is_named(const XrTypeRef *type, const char *name) {
    return type && type->kind == XR_TREF_NAMED && type->name && name &&
           strcmp(type->name, name) == 0;
}

static bool body_type_ref_is_int(const XrTypeRef *type) {
    return type && type->kind == XR_TREF_INT;
}

static bool body_type_ref_is_bool(const XrTypeRef *type) {
    return type && type->kind == XR_TREF_BOOL;
}

static const XgClassSummary *body_find_class_by_type_key(XgBodyCollect *bc, uint32_t type_key,
                                                         const char **out_name) {
    if (out_name)
        *out_name = NULL;
    if (!bc || !bc->producer || !bc->evidence || type_key == 0)
        return NULL;
    for (uint32_t i = 0; i < bc->producer->nclasses; i++) {
        const XgClassNameRow *row = &bc->producer->classes[i];
        if (!row->name || row->summary_index >= bc->evidence->nclasses)
            continue;
        if (hash_named_type_key32(row->name, NULL, 0) != type_key)
            continue;
        if (out_name)
            *out_name = row->name;
        return &bc->evidence->classes[row->summary_index];
    }
    return NULL;
}

static bool body_class_implements_hashable(XgBodyCollect *bc, const XgClassSummary *cls) {
    uint32_t hashable_id = hash_name32("Hashable");
    if (!bc || !bc->evidence || !cls)
        return false;
    for (uint32_t i = 0; i < bc->evidence->ninterface_impls; i++) {
        const XgInterfaceImplSummary *impl = &bc->evidence->interface_impls[i];
        if (impl->implementor_class_id == cls->class_id &&
            (impl->name_id == hashable_id || impl->interface_id == (XgInterfaceId) hashable_id))
            return true;
    }
    return false;
}

static const XgPendingBody *producer_find_method_body(const XgProducer *p, XgMethodId method_id) {
    if (!p || method_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < p->nbodies; i++) {
        if (p->bodies[i].owner_method_id == method_id)
            return &p->bodies[i];
    }
    return NULL;
}

static bool body_hash_method_valid(const MethodDeclNode *method) {
    return method && !method->is_static && !method->is_private && !method->is_constructor &&
           method->param_count == 0 && body_type_ref_is_int(method->return_type);
}

static bool body_eq_method_valid(const MethodDeclNode *method, const char *type_name) {
    return method && !method->is_static && !method->is_private && !method->is_constructor &&
           method->is_operator && method->param_count == 1 &&
           body_type_ref_is_named(method->param_types ? method->param_types[0] : NULL, type_name) &&
           body_type_ref_is_bool(method->return_type);
}

static void body_ensure_user_hash_eq(XgBodyCollect *bc, uint32_t key_type_key) {
    const char *type_name = NULL;
    const XgClassSummary *cls = body_find_class_by_type_key(bc, key_type_key, &type_name);
    XgMethodSummary *eq_method;
    XgMethodSummary *hash_method;
    const XgPendingBody *eq_body;
    const XgPendingBody *hash_body;
    XgHashEqSummary row;
    if (!bc || !bc->producer || !bc->evidence || !cls || !type_name)
        return;
    if (xg_global_evidence_find_hash_eq(bc->evidence, key_type_key))
        return;
    if (!body_class_implements_hashable(bc, cls))
        return;
    eq_method = producer_find_method_by_name_in_hierarchy(bc->producer, cls->class_id,
                                                          hash_name32("=="), false);
    hash_method = producer_find_method_by_name_in_hierarchy(bc->producer, cls->class_id,
                                                            hash_name32("hash"), false);
    eq_body = producer_find_method_body(bc->producer, eq_method ? eq_method->method_id : XG_NO_ID);
    hash_body =
        producer_find_method_body(bc->producer, hash_method ? hash_method->method_id : XG_NO_ID);
    if (!eq_body || !hash_body || !body_eq_method_valid(eq_body->method, type_name) ||
        !body_hash_method_valid(hash_body->method))
        return;
    memset(&row, 0, sizeof(row));
    row.hash_eq_id = (XgHashEqId) (bc->evidence->nhash_eqs + 1);
    row.type_key = key_type_key;
    row.kind = XG_HASH_EQ_USER_METHOD;
    row.eq_func_id = eq_body->func_id;
    row.hash_func_id = hash_body->func_id;
    row.flags = XG_HASH_EQ_NO_ALLOC | XG_HASH_EQ_NO_THROW | XG_HASH_EQ_PURE;
    if ((cls->flags & (XG_CLASS_EXPLICIT_FINAL | XG_CLASS_INFERRED_FINAL)) != 0)
        row.flags |= XG_HASH_EQ_FINAL;
    (void) xg_global_evidence_add_hash_eq(bc->evidence, &row);
}

static void body_ensure_hash_eq(XgBodyCollect *bc, uint32_t key_type_key) {
    body_ensure_builtin_hash_eq(bc, key_type_key);
    body_ensure_user_hash_eq(bc, key_type_key);
}

static XgMapShapeId
body_add_map_shape_for_literal(XgBodyCollect *bc, const AstNode *node,
                               const XrTypeRef *type_annotation, uint32_t source_span_id,
                               uint32_t *out_receiver_type_key, uint32_t *out_key_type_key,
                               uint32_t *out_value_type_key, uint8_t *out_container_kind) {
    XgMapShapeSummary shape;
    uint8_t container_kind;
    uint8_t annotated_container_kind = 0;
    int count;
    AstNode **keys;
    AstNode **values;
    uint32_t key_type_key = 0;
    uint32_t value_type_key = 0;
    uint32_t receiver_type_key;
    uint32_t const_stack[16];
    uint32_t *const_ids = const_stack;
    bool const_ids_heap = false;
    if (out_receiver_type_key)
        *out_receiver_type_key = 0;
    if (out_key_type_key)
        *out_key_type_key = 0;
    if (out_value_type_key)
        *out_value_type_key = 0;
    if (out_container_kind)
        *out_container_kind = 0;
    if (!bc || !bc->evidence || !node ||
        (node->type != AST_MAP_LITERAL && node->type != AST_SET_LITERAL))
        return XG_NO_ID;
    container_kind = node->type == AST_SET_LITERAL ? XG_MAP_CONTAINER_SET : XG_MAP_CONTAINER_MAP;
    (void) body_type_ref_map_parts(type_annotation, &annotated_container_kind, &key_type_key,
                                   &value_type_key);
    if (annotated_container_kind != 0 && annotated_container_kind != container_kind) {
        key_type_key = 0;
        value_type_key = 0;
    }
    count = node->type == AST_SET_LITERAL ? node->as.set_literal.count : node->as.map_literal.count;
    keys =
        node->type == AST_SET_LITERAL ? node->as.set_literal.elements : node->as.map_literal.keys;
    values = node->type == AST_SET_LITERAL ? NULL : node->as.map_literal.values;
    if (count > (int) (sizeof(const_stack) / sizeof(const_stack[0]))) {
        const_ids = (uint32_t *) xr_calloc((size_t) count, sizeof(*const_ids));
        if (!const_ids)
            return XG_NO_ID;
        const_ids_heap = true;
    } else {
        memset(const_stack, 0, sizeof(const_stack));
    }
    for (int i = 0; i < count; i++) {
        uint32_t kt = body_expr_type_key(bc, keys ? keys[i] : NULL);
        uint32_t vt = values ? body_expr_type_key(bc, values[i]) : 0;
        if (key_type_key == 0 && kt != 0)
            key_type_key = kt;
        if (value_type_key == 0 && vt != 0)
            value_type_key = vt;
        const_ids[i] = body_const_expr_id(keys ? keys[i] : NULL);
    }
    if (key_type_key == 0 || (container_kind == XG_MAP_CONTAINER_MAP && value_type_key == 0)) {
        if (const_ids_heap)
            xr_free(const_ids);
        return XG_NO_ID;
    }
    receiver_type_key = body_map_receiver_type_key(container_kind, key_type_key, value_type_key);
    memset(&shape, 0, sizeof(shape));
    shape.shape_id = (XgMapShapeId) (bc->evidence->nmap_shapes + 1);
    shape.module_id = bc->module_id;
    shape.owner_func_id = bc->owner_func_id;
    shape.source_span_id = source_span_id;
    shape.container_kind = container_kind;
    shape.source = XG_MAP_SHAPE_SRC_LITERAL;
    shape.key_type_key = key_type_key;
    shape.value_type_key = container_kind == XG_MAP_CONTAINER_SET ? 0 : value_type_key;
    shape.entry_start = count > 0 ? (uint32_t) (bc->evidence->nmap_entries + 1) : 0;
    shape.entry_count = (uint16_t) (count < UINT16_MAX ? count : UINT16_MAX);
    shape.literal_count = (uint32_t) (count > 0 ? count : 0);
    shape.flags = XG_MAP_SHAPE_LITERAL;
    if (count > 0 && count <= XG_SMALL_MAP_LITERAL_MAX)
        shape.flags |= XG_MAP_SHAPE_SMALL;
    if (container_kind == XG_MAP_CONTAINER_MAP &&
        body_map_value_type_supports_bool_direct(value_type_key) &&
        body_map_literal_has_bool_domain(keys, count, key_type_key))
        shape.flags |= XG_MAP_SHAPE_BOOL_DIRECT;
    if (body_map_literal_has_dense_i64_domain(keys, count, key_type_key))
        shape.flags |= XG_MAP_SHAPE_DENSE_INT;
    shape.shape_hash =
        body_map_shape_hash(container_kind, key_type_key, shape.value_type_key, const_ids, count);
    if (!xg_global_evidence_add_map_shape(bc->evidence, &shape)) {
        if (const_ids_heap)
            xr_free(const_ids);
        return XG_NO_ID;
    }
    for (int i = 0; i < count; i++) {
        XgMapEntrySummary entry;
        uint32_t key_const_id = const_ids[i];
        uint32_t value_const_id = values ? body_const_expr_id(values[i]) : 0;
        memset(&entry, 0, sizeof(entry));
        entry.entry_id = (XgMapEntryId) (bc->evidence->nmap_entries + 1);
        entry.shape_id = shape.shape_id;
        entry.entry_ordinal = (uint32_t) i;
        entry.key_const_id = key_const_id;
        entry.value_const_id = value_const_id;
        entry.prehash = body_map_const_prehash(keys[i]);
        if (key_const_id != 0)
            entry.flags |= XG_MAP_ENTRY_CONST_KEY;
        if (keys && keys[i] && keys[i]->type == AST_LITERAL_INT &&
            !keys[i]->as.literal.int_overflows_i64) {
            entry.key_i64 = keys[i]->as.literal.raw_value.int_val;
            entry.flags |= XG_MAP_ENTRY_INT_KEY;
        }
        if (keys && keys[i] &&
            (keys[i]->type == AST_LITERAL_TRUE || keys[i]->type == AST_LITERAL_FALSE)) {
            entry.key_i64 = keys[i]->as.literal.raw_value.bool_val ? 1 : 0;
            entry.flags |= XG_MAP_ENTRY_BOOL_KEY;
        }
        if (value_const_id != 0)
            entry.flags |= XG_MAP_ENTRY_CONST_VALUE;
        for (int j = 0; j < i; j++) {
            if (key_const_id != 0 && const_ids[j] == key_const_id) {
                entry.flags |= XG_MAP_ENTRY_DUPLICATE_KEY;
                break;
            }
        }
        (void) xg_global_evidence_add_map_entry(bc->evidence, &entry);
    }
    body_ensure_hash_eq(bc, key_type_key);
    if (out_receiver_type_key)
        *out_receiver_type_key = receiver_type_key;
    if (out_key_type_key)
        *out_key_type_key = key_type_key;
    if (out_value_type_key)
        *out_value_type_key = shape.value_type_key;
    if (out_container_kind)
        *out_container_kind = container_kind;
    if (const_ids_heap)
        xr_free(const_ids);
    return shape.shape_id;
}

static void body_bind_map_shape_local(XgBodyCollect *bc, const char *name, XgMapShapeId shape_id,
                                      uint8_t container_kind, uint32_t receiver_type_key,
                                      uint32_t key_type_key, uint32_t value_type_key) {
    XgLocalType *row;
    if (!bc || !name || shape_id == XG_NO_ID)
        return;
    row = body_find_local(bc, name);
    if (!row)
        return;
    row->map_shape_id = shape_id;
    row->map_container_kind = container_kind;
    row->map_receiver_type_key = receiver_type_key;
    row->map_key_type_key = key_type_key;
    row->map_value_type_key = value_type_key;
}

static void body_bind_map_shape_local_from_source(XgBodyCollect *bc, const char *name,
                                                  const XgLocalType *source) {
    XgLocalType *target;
    if (!bc || !name || !source || source->map_shape_id == XG_NO_ID)
        return;
    target = body_find_local(bc, name);
    if (!target)
        return;
    if (target->type_key != 0 && source->map_receiver_type_key != 0 &&
        target->type_key != source->map_receiver_type_key)
        return;
    body_bind_map_shape_local(bc, name, source->map_shape_id, source->map_container_kind,
                              source->map_receiver_type_key, source->map_key_type_key,
                              source->map_value_type_key);
}

static void body_clear_map_shape_local(XgBodyCollect *bc, const char *name) {
    XgLocalType *row;
    if (!bc || !name)
        return;
    row = body_find_local(bc, name);
    if (!row)
        return;
    row->map_shape_id = XG_NO_ID;
    row->map_container_kind = 0;
    row->map_receiver_type_key = 0;
    row->map_key_type_key = 0;
    row->map_value_type_key = 0;
}

static void body_bind_sequence_local(XgBodyCollect *bc, const char *name, uint8_t sequence_kind,
                                     uint32_t elem_type_key, const XrTypeRef *elem_type_ref) {
    XgLocalType *row;
    if (!bc || !name || sequence_kind == 0)
        return;
    row = body_find_local(bc, name);
    if (!row)
        return;
    row->sequence_kind = sequence_kind;
    row->sequence_elem_type_key = elem_type_key;
    row->sequence_elem_map_container_kind = 0;
    row->sequence_elem_map_key_type_key = 0;
    row->sequence_elem_map_value_type_key = 0;
    if (body_type_ref_map_parts(elem_type_ref, &row->sequence_elem_map_container_kind,
                                &row->sequence_elem_map_key_type_key,
                                &row->sequence_elem_map_value_type_key) &&
        row->sequence_elem_map_container_kind == XG_MAP_CONTAINER_SET)
        row->sequence_elem_map_value_type_key = 0;
}

static void body_clear_sequence_local(XgBodyCollect *bc, const char *name) {
    XgLocalType *row;
    if (!bc || !name)
        return;
    row = body_find_local(bc, name);
    if (!row)
        return;
    row->sequence_kind = 0;
    row->sequence_elem_type_key = 0;
    row->sequence_elem_map_container_kind = 0;
    row->sequence_elem_map_key_type_key = 0;
    row->sequence_elem_map_value_type_key = 0;
}

static XgLocalType *body_lookup_local_sequence(XgBodyCollect *bc, const AstNode *expr) {
    XgLocalType *row;
    if (!bc || !expr)
        return NULL;
    switch (expr->type) {
        case AST_GROUPING:
            return body_lookup_local_sequence(bc, expr->as.grouping);
        case AST_MOVE_EXPR:
            return body_lookup_local_sequence(bc, expr->as.move_expr.expr);
        case AST_UNSAFE_EXPR:
            return body_lookup_local_sequence(bc, expr->as.unsafe_expr.operand);
        case AST_FORCE_UNWRAP:
            return body_lookup_local_sequence(bc, expr->as.unary.operand);
        default:
            break;
    }
    if (expr->type != AST_VARIABLE || !expr->as.variable.name)
        return NULL;
    row = body_find_local(bc, expr->as.variable.name);
    return row && row->sequence_kind != 0 ? row : NULL;
}

static XgLocalType *body_lookup_local_map_shape(XgBodyCollect *bc, const AstNode *expr) {
    XgLocalType *row;
    if (!bc || !expr)
        return NULL;
    switch (expr->type) {
        case AST_GROUPING:
            return body_lookup_local_map_shape(bc, expr->as.grouping);
        case AST_MOVE_EXPR:
            return body_lookup_local_map_shape(bc, expr->as.move_expr.expr);
        case AST_UNSAFE_EXPR:
            return body_lookup_local_map_shape(bc, expr->as.unsafe_expr.operand);
        case AST_FORCE_UNWRAP:
            return body_lookup_local_map_shape(bc, expr->as.unary.operand);
        default:
            break;
    }
    if (expr->type != AST_VARIABLE || !expr->as.variable.name)
        return NULL;
    row = body_find_local(bc, expr->as.variable.name);
    return row && row->map_shape_id != XG_NO_ID ? row : NULL;
}

static const AstNode *body_map_receiver_unwrap(const AstNode *expr) {
    while (expr) {
        switch (expr->type) {
            case AST_GROUPING:
                expr = expr->as.grouping;
                break;
            case AST_MOVE_EXPR:
                expr = expr->as.move_expr.expr;
                break;
            case AST_UNSAFE_EXPR:
                expr = expr->as.unsafe_expr.operand;
                break;
            case AST_FORCE_UNWRAP:
                expr = expr->as.unary.operand;
                break;
            default:
                return expr;
        }
    }
    return NULL;
}

static const XgClassSummary *body_find_class_summary(XgBodyCollect *bc, XgClassId class_id) {
    if (!bc || !bc->evidence || class_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bc->evidence->nclasses; i++) {
        const XgClassSummary *cls = &bc->evidence->classes[i];
        if (cls->class_id == class_id)
            return cls;
    }
    return NULL;
}

static const XgClassFieldSummary *
body_find_class_field_in_hierarchy(XgBodyCollect *bc, XgClassId class_id, uint32_t field_name_id) {
    uint32_t depth = 0;
    while (class_id != XG_NO_ID && depth++ < 64) {
        const XgClassSummary *cls = body_find_class_summary(bc, class_id);
        if (!cls)
            return NULL;
        if (cls->field_start != 0 && cls->field_count > 0) {
            uint32_t start = cls->field_start - 1;
            if (start >= bc->evidence->nclass_fields ||
                cls->field_count > bc->evidence->nclass_fields - start)
                return NULL;
            for (uint32_t i = 0; i < cls->field_count; i++) {
                const XgClassFieldSummary *field = &bc->evidence->class_fields[start + i];
                if (field->owner_class_id == cls->class_id && field->name_id == field_name_id &&
                    (field->flags & XG_CLASS_FIELD_STATIC) == 0)
                    return field;
            }
        }
        class_id = cls->parent_class_id;
    }
    return NULL;
}

static bool body_class_field_map_parts(const XgClassFieldSummary *field,
                                       uint8_t *out_container_kind, uint32_t *out_key_type_key,
                                       uint32_t *out_value_type_key) {
    uint8_t container_kind = 0;
    uint32_t key_type_key = 0;
    uint32_t value_type_key = 0;
    if (out_container_kind)
        *out_container_kind = 0;
    if (out_key_type_key)
        *out_key_type_key = 0;
    if (out_value_type_key)
        *out_value_type_key = 0;
    if (!field)
        return false;
    if (field->semantic_kind == XG_CLASS_FIELD_TYPE_MAP) {
        container_kind = XG_MAP_CONTAINER_MAP;
        key_type_key = field->key_type_key != 0 ? field->key_type_key : field->element_type_key;
        value_type_key = field->value_type_key;
    } else if (field->semantic_kind == XG_CLASS_FIELD_TYPE_SET) {
        container_kind = XG_MAP_CONTAINER_SET;
        key_type_key = field->key_type_key != 0 ? field->key_type_key : field->element_type_key;
    } else {
        return false;
    }
    if (key_type_key == 0 || (container_kind == XG_MAP_CONTAINER_MAP && value_type_key == 0))
        return false;
    if (out_container_kind)
        *out_container_kind = container_kind;
    if (out_key_type_key)
        *out_key_type_key = key_type_key;
    if (out_value_type_key)
        *out_value_type_key = container_kind == XG_MAP_CONTAINER_SET ? 0 : value_type_key;
    return true;
}

static XgMapShapeId body_add_map_shape_for_static_type(XgBodyCollect *bc, uint8_t container_kind,
                                                       uint32_t key_type_key,
                                                       uint32_t value_type_key,
                                                       uint32_t source_span_id) {
    XgMapShapeSummary shape;
    uint32_t stored_value_type_key = container_kind == XG_MAP_CONTAINER_SET ? 0 : value_type_key;
    uint64_t shape_hash;
    if (!bc || !bc->evidence || key_type_key == 0 ||
        (container_kind == XG_MAP_CONTAINER_MAP && stored_value_type_key == 0))
        return XG_NO_ID;
    shape_hash = body_map_shape_hash(container_kind, key_type_key, stored_value_type_key, NULL, 0);
    for (uint32_t i = 0; i < bc->evidence->nmap_shapes; i++) {
        const XgMapShapeSummary *existing = &bc->evidence->map_shapes[i];
        if (existing->owner_func_id == bc->owner_func_id &&
            existing->container_kind == container_kind &&
            existing->source == XG_MAP_SHAPE_SRC_STATIC && existing->key_type_key == key_type_key &&
            existing->value_type_key == stored_value_type_key && existing->shape_hash == shape_hash)
            return existing->shape_id;
    }
    memset(&shape, 0, sizeof(shape));
    shape.shape_id = (XgMapShapeId) (bc->evidence->nmap_shapes + 1);
    shape.module_id = bc->module_id;
    shape.owner_func_id = bc->owner_func_id;
    shape.source_span_id = source_span_id;
    shape.container_kind = container_kind;
    shape.source = XG_MAP_SHAPE_SRC_STATIC;
    shape.key_type_key = key_type_key;
    shape.value_type_key = stored_value_type_key;
    shape.flags = XG_MAP_SHAPE_STATIC;
    shape.shape_hash = shape_hash;
    if (!xg_global_evidence_add_map_shape(bc->evidence, &shape))
        return XG_NO_ID;
    body_ensure_hash_eq(bc, key_type_key);
    return shape.shape_id;
}

static void body_bind_map_shape_local_for_type_ref(XgBodyCollect *bc, const char *name,
                                                   const XrTypeRef *type, uint32_t source_span_id) {
    uint8_t container_kind;
    uint32_t key_type_key;
    uint32_t value_type_key;
    uint32_t stored_value_type_key;
    XgMapShapeId shape_id;
    if (!bc || !name)
        return;
    if (!body_type_ref_map_parts(type, &container_kind, &key_type_key, &value_type_key))
        return;
    stored_value_type_key = container_kind == XG_MAP_CONTAINER_SET ? 0 : value_type_key;
    shape_id = body_add_map_shape_for_static_type(bc, container_kind, key_type_key,
                                                  stored_value_type_key, source_span_id);
    if (shape_id == XG_NO_ID)
        return;
    body_bind_map_shape_local(
        bc, name, shape_id, container_kind,
        body_map_receiver_type_key(container_kind, key_type_key, stored_value_type_key),
        key_type_key, stored_value_type_key);
}

static bool body_sequence_elem_map_parts(const XgLocalType *local, uint8_t *out_container_kind,
                                         uint32_t *out_key_type_key, uint32_t *out_value_type_key) {
    if (out_container_kind)
        *out_container_kind = 0;
    if (out_key_type_key)
        *out_key_type_key = 0;
    if (out_value_type_key)
        *out_value_type_key = 0;
    if (!local || local->sequence_elem_map_container_kind == 0 ||
        local->sequence_elem_map_key_type_key == 0 ||
        (local->sequence_elem_map_container_kind == XG_MAP_CONTAINER_MAP &&
         local->sequence_elem_map_value_type_key == 0))
        return false;
    if (out_container_kind)
        *out_container_kind = local->sequence_elem_map_container_kind;
    if (out_key_type_key)
        *out_key_type_key = local->sequence_elem_map_key_type_key;
    if (out_value_type_key)
        *out_value_type_key = local->sequence_elem_map_container_kind == XG_MAP_CONTAINER_SET
                                  ? 0
                                  : local->sequence_elem_map_value_type_key;
    return true;
}

static bool body_sequence_index_static_map_parts(XgBodyCollect *bc, const AstNode *expr,
                                                 uint8_t *out_container_kind,
                                                 uint32_t *out_key_type_key,
                                                 uint32_t *out_value_type_key) {
    XgLocalType *local;
    if (!bc || !expr || expr->type != AST_INDEX_GET)
        return false;
    local = body_lookup_local_sequence(bc, expr->as.index_get.array);
    return body_sequence_elem_map_parts(local, out_container_kind, out_key_type_key,
                                        out_value_type_key);
}

static bool body_sequence_get_static_map_parts(XgBodyCollect *bc, const CallExprNode *call,
                                               uint8_t *out_container_kind,
                                               uint32_t *out_key_type_key,
                                               uint32_t *out_value_type_key) {
    const MemberAccessNode *member;
    XgLocalType *local;
    if (!bc || !call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    member = &call->callee->as.member_access;
    if (!member->name || strcmp(member->name, "get") != 0 || call->arg_count != 1)
        return false;
    local = body_lookup_local_sequence(bc, member->object);
    return body_sequence_elem_map_parts(local, out_container_kind, out_key_type_key,
                                        out_value_type_key);
}

static bool body_expr_static_map_parts(XgBodyCollect *bc, const AstNode *expr,
                                       uint8_t *out_container_kind, uint32_t *out_key_type_key,
                                       uint32_t *out_value_type_key) {
    if (out_container_kind)
        *out_container_kind = 0;
    if (out_key_type_key)
        *out_key_type_key = 0;
    if (out_value_type_key)
        *out_value_type_key = 0;
    if (!bc || !expr)
        return false;
    switch (expr->type) {
        case AST_GROUPING:
            return body_expr_static_map_parts(bc, expr->as.grouping, out_container_kind,
                                              out_key_type_key, out_value_type_key);
        case AST_COMPTIME_EXPR:
            return body_expr_static_map_parts(bc, expr->as.comptime_expr.expr, out_container_kind,
                                              out_key_type_key, out_value_type_key);
        case AST_MOVE_EXPR:
            return body_expr_static_map_parts(bc, expr->as.move_expr.expr, out_container_kind,
                                              out_key_type_key, out_value_type_key);
        case AST_UNSAFE_EXPR:
            return body_expr_static_map_parts(bc, expr->as.unsafe_expr.operand, out_container_kind,
                                              out_key_type_key, out_value_type_key);
        case AST_FORCE_UNWRAP:
            return body_expr_static_map_parts(bc, expr->as.unary.operand, out_container_kind,
                                              out_key_type_key, out_value_type_key);
        case AST_AS_EXPR:
            return body_type_ref_map_parts(expr->as.as_expr.type, out_container_kind,
                                           out_key_type_key, out_value_type_key);
        case AST_INDEX_GET:
            return body_sequence_index_static_map_parts(bc, expr, out_container_kind,
                                                        out_key_type_key, out_value_type_key);
        case AST_CALL_EXPR:
            if (body_sequence_get_static_map_parts(bc, &expr->as.call_expr, out_container_kind,
                                                   out_key_type_key, out_value_type_key))
                return true;
            return body_type_ref_map_parts(body_call_return_type_ref(bc, &expr->as.call_expr),
                                           out_container_kind, out_key_type_key,
                                           out_value_type_key);
        default:
            return false;
    }
}

static bool body_lookup_map_receiver_shape(XgBodyCollect *bc, const AstNode *expr,
                                           uint32_t source_span_id, XgLocalType *out) {
    const AstNode *unwrapped;
    XgLocalType *local;
    XgClassId receiver_class;
    const XgClassFieldSummary *field;
    uint8_t container_kind;
    uint32_t key_type_key;
    uint32_t value_type_key;
    XgMapShapeId shape_id;
    if (!bc || !expr || !out)
        return false;
    memset(out, 0, sizeof(*out));
    local = body_lookup_local_map_shape(bc, expr);
    if (local) {
        *out = *local;
        return true;
    }
    if (body_expr_static_map_parts(bc, expr, &container_kind, &key_type_key, &value_type_key)) {
        shape_id = body_add_map_shape_for_static_type(bc, container_kind, key_type_key,
                                                      value_type_key, source_span_id);
        if (shape_id == XG_NO_ID)
            return false;
        out->map_shape_id = shape_id;
        out->map_container_kind = container_kind;
        out->map_key_type_key = key_type_key;
        out->map_value_type_key = container_kind == XG_MAP_CONTAINER_SET ? 0 : value_type_key;
        out->map_receiver_type_key =
            body_map_receiver_type_key(container_kind, key_type_key, out->map_value_type_key);
        return true;
    }
    unwrapped = body_map_receiver_unwrap(expr);
    if (!unwrapped || unwrapped->type != AST_MEMBER_ACCESS || !unwrapped->as.member_access.name)
        return false;
    receiver_class = body_resolve_expr_class(bc, unwrapped->as.member_access.object);
    field = body_find_class_field_in_hierarchy(bc, receiver_class,
                                               hash_name32(unwrapped->as.member_access.name));
    if (!body_class_field_map_parts(field, &container_kind, &key_type_key, &value_type_key))
        return false;
    shape_id = body_add_map_shape_for_static_type(bc, container_kind, key_type_key, value_type_key,
                                                  source_span_id);
    if (shape_id == XG_NO_ID)
        return false;
    out->map_shape_id = shape_id;
    out->map_container_kind = container_kind;
    out->map_key_type_key = key_type_key;
    out->map_value_type_key = container_kind == XG_MAP_CONTAINER_SET ? 0 : value_type_key;
    out->map_receiver_type_key =
        body_map_receiver_type_key(container_kind, key_type_key, out->map_value_type_key);
    return true;
}

static void body_add_map_key_access_row(XgBodyCollect *bc, const AstNode *node,
                                        const XgLocalType *local, const AstNode *key, uint8_t op,
                                        bool mutating, bool missing_panics) {
    uint32_t key_const_id;
    XgKeyAccessSummary row;
    if (!bc || !node || !local || local->map_shape_id == XG_NO_ID)
        return;
    key_const_id = body_const_expr_id(key);
    memset(&row, 0, sizeof(row));
    row.access_id = (XgKeyAccessId) (bc->evidence->nkey_accesses + 1);
    row.owner_func_id = bc->owner_func_id;
    row.source_span_id = (uint32_t) node->line;
    row.body_ordinal = bc->key_access_count++;
    row.container_kind = local->map_container_kind;
    row.op = op;
    row.receiver_shape_id = local->map_shape_id;
    row.receiver_type_key = local->map_receiver_type_key;
    row.key_type_key = local->map_key_type_key;
    row.value_type_key = local->map_value_type_key;
    row.key_const_id = key_const_id;
    row.key_prehash = body_map_const_prehash(key);
    if (key_const_id != 0)
        row.flags |= XG_KEY_ACCESS_CONST_KEY;
    if (missing_panics)
        row.flags |= XG_KEY_ACCESS_MISSING_PANICS;
    if (mutating)
        row.flags |= XG_KEY_ACCESS_MUTATING;
    (void) xg_global_evidence_add_key_access(bc->evidence, &row);
}

static void body_add_map_index_key_access(XgBodyCollect *bc, const AstNode *node, bool mutating) {
    const IndexGetNode *get;
    const IndexSetNode *set;
    XgLocalType receiver_shape;
    const AstNode *receiver;
    const AstNode *key;
    if (!bc || !node)
        return;
    if (node->type == AST_INDEX_GET) {
        get = &node->as.index_get;
        receiver = get->array;
        key = get->index;
    } else if (node->type == AST_INDEX_SET) {
        set = &node->as.index_set;
        receiver = set->array;
        key = set->index;
    } else {
        return;
    }
    if (!body_lookup_map_receiver_shape(bc, receiver, (uint32_t) node->line, &receiver_shape) ||
        receiver_shape.map_container_kind != XG_MAP_CONTAINER_MAP)
        return;
    body_add_map_key_access_row(bc, node, &receiver_shape, key,
                                mutating ? XG_KEY_ACCESS_SET : XG_KEY_ACCESS_INDEX_GET, mutating,
                                !mutating);
}

static void body_add_map_method_key_access(XgBodyCollect *bc, const AstNode *node) {
    const CallExprNode *call;
    const MemberAccessNode *member;
    XgLocalType receiver_shape;
    const AstNode *key = NULL;
    uint8_t op = 0;
    bool mutating = false;
    if (!bc || !node || node->type != AST_CALL_EXPR)
        return;
    call = &node->as.call_expr;
    if (!call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return;
    member = &call->callee->as.member_access;
    if (!member->name)
        return;
    if (!body_lookup_map_receiver_shape(bc, member->object, (uint32_t) node->line, &receiver_shape))
        return;

    if (receiver_shape.map_container_kind == XG_MAP_CONTAINER_MAP) {
        if (strcmp(member->name, "get") == 0 && call->arg_count == 1) {
            op = XG_KEY_ACCESS_GET;
            key = call->arguments ? call->arguments[0] : NULL;
        } else if (strcmp(member->name, "has") == 0 && call->arg_count == 1) {
            op = XG_KEY_ACCESS_HAS;
            key = call->arguments ? call->arguments[0] : NULL;
        } else if (strcmp(member->name, "delete") == 0 && call->arg_count == 1) {
            op = XG_KEY_ACCESS_DELETE;
            key = call->arguments ? call->arguments[0] : NULL;
            mutating = true;
        } else if (strcmp(member->name, "set") == 0 && call->arg_count == 2) {
            op = XG_KEY_ACCESS_SET;
            key = call->arguments ? call->arguments[0] : NULL;
            mutating = true;
        } else if (strcmp(member->name, "clear") == 0 && call->arg_count == 0) {
            op = XG_KEY_ACCESS_CLEAR;
            mutating = true;
        }
    } else if (receiver_shape.map_container_kind == XG_MAP_CONTAINER_SET) {
        if (strcmp(member->name, "has") == 0 && call->arg_count == 1) {
            op = XG_KEY_ACCESS_HAS;
            key = call->arguments ? call->arguments[0] : NULL;
        } else if (strcmp(member->name, "delete") == 0 && call->arg_count == 1) {
            op = XG_KEY_ACCESS_DELETE;
            key = call->arguments ? call->arguments[0] : NULL;
            mutating = true;
        } else if (strcmp(member->name, "add") == 0 && call->arg_count == 1) {
            op = XG_KEY_ACCESS_ADD;
            key = call->arguments ? call->arguments[0] : NULL;
            mutating = true;
        } else if (strcmp(member->name, "clear") == 0 && call->arg_count == 0) {
            op = XG_KEY_ACCESS_CLEAR;
            mutating = true;
        }
    }

    if (op == 0)
        return;
    body_add_map_key_access_row(bc, node, &receiver_shape, key, op, mutating, false);
}

static uint32_t body_sequence_receiver_type_key(uint8_t sequence_kind, uint32_t elem_type_key) {
    switch ((XgSequenceKind) sequence_kind) {
        case XG_SEQ_ARRAY:
            return hash_named_type_key32("Array", NULL, 0) ^ elem_type_key;
        case XG_SEQ_BYTES:
            return hash_named_type_key32("Bytes", NULL, 0);
        case XG_SEQ_STRING:
            return hash_synthetic_tref32(XR_TREF_STRING, NULL, NULL, 0);
        case XG_SEQ_SPAN:
            return hash_named_type_key32("Span", NULL, 0) ^ elem_type_key;
        case XG_SEQ_BYTE_SPAN:
            return hash_named_type_key32("ByteSpan", NULL, 0);
        case XG_SEQ_STRING_BUILDER:
            return hash_named_type_key32("StringBuilder", NULL, 0);
        default:
            return 0;
    }
}

static void body_add_sequence_access_row(XgBodyCollect *bc, const AstNode *node,
                                         const XgLocalType *local, uint8_t access_kind,
                                         const AstNode *index, const AstNode *length,
                                         bool mutating) {
    XgSequenceAccessSummary row;
    if (!bc || !node || !local || local->sequence_kind == 0)
        return;
    memset(&row, 0, sizeof(row));
    row.access_id = (XgSequenceAccessId) (bc->evidence->nsequence_accesses + 1);
    row.owner_func_id = bc->owner_func_id;
    row.source_span_id = (uint32_t) node->line;
    row.body_ordinal = bc->sequence_access_count++;
    row.sequence_kind = local->sequence_kind;
    row.access_kind = access_kind;
    row.receiver_type_key =
        local->type_key
            ? local->type_key
            : body_sequence_receiver_type_key(local->sequence_kind, local->sequence_elem_type_key);
    row.elem_type_key = local->sequence_elem_type_key;
    row.index_expr_id = body_const_expr_id(index);
    row.length_expr_id = body_const_expr_id(length);
    if (mutating)
        row.flags |= XG_SEQ_ACCESS_MUTATING;
    if (row.index_expr_id != 0)
        row.flags |= XG_SEQ_ACCESS_CONST_INDEX;
    if (access_kind == XG_SEQ_ACCESS_SLICE)
        row.flags |= XG_SEQ_ACCESS_SLICE_NORMALIZED;
    if (local->sequence_kind == XG_SEQ_SPAN || local->sequence_kind == XG_SEQ_BYTE_SPAN)
        row.flags |= XG_SEQ_ACCESS_FROM_SPAN;
    (void) xg_global_evidence_add_sequence_access(bc->evidence, &row);
}

static void body_add_sequence_index_access(XgBodyCollect *bc, const AstNode *node, bool mutating) {
    const AstNode *receiver;
    const AstNode *index;
    XgLocalType *local;
    if (!bc || !node)
        return;
    if (node->type == AST_INDEX_GET) {
        receiver = node->as.index_get.array;
        index = node->as.index_get.index;
    } else if (node->type == AST_INDEX_SET) {
        receiver = node->as.index_set.array;
        index = node->as.index_set.index;
    } else {
        return;
    }
    local = body_lookup_local_sequence(bc, receiver);
    if (!local)
        return;
    body_add_sequence_access_row(bc, node, local,
                                 mutating ? XG_SEQ_ACCESS_INDEX_SET : XG_SEQ_ACCESS_INDEX_GET,
                                 index, NULL, mutating);
}

static void body_add_sequence_slice_access(XgBodyCollect *bc, const AstNode *node) {
    XgLocalType *local;
    if (!bc || !node || node->type != AST_SLICE_EXPR)
        return;
    local = body_lookup_local_sequence(bc, node->as.slice_expr.source);
    if (!local)
        return;
    body_add_sequence_access_row(bc, node, local, XG_SEQ_ACCESS_SLICE, node->as.slice_expr.start,
                                 node->as.slice_expr.end, false);
}

static void body_add_sequence_length_access(XgBodyCollect *bc, const AstNode *node) {
    const MemberAccessNode *member;
    XgLocalType *local;
    if (!bc || !node || node->type != AST_MEMBER_ACCESS)
        return;
    member = &node->as.member_access;
    if (!member->name || strcmp(member->name, "length") != 0)
        return;
    local = body_lookup_local_sequence(bc, member->object);
    if (!local)
        return;
    body_add_sequence_access_row(bc, node, local, XG_SEQ_ACCESS_LENGTH, NULL, NULL, false);
}

static void body_add_capacity_op(XgBodyCollect *bc, const AstNode *node, const XgLocalType *local,
                                 uint8_t op_kind, const AstNode *count_expr, bool may_grow) {
    XgCapacityOpSummary row;
    if (!bc || !node || !local || local->sequence_kind == 0)
        return;
    memset(&row, 0, sizeof(row));
    row.op_id = (XgCapacityOpId) (bc->evidence->ncapacity_ops + 1);
    row.owner_func_id = bc->owner_func_id;
    row.source_span_id = (uint32_t) node->line;
    row.body_ordinal = bc->capacity_op_count++;
    row.sequence_kind = local->sequence_kind;
    row.op_kind = op_kind;
    row.receiver_type_key =
        local->type_key
            ? local->type_key
            : body_sequence_receiver_type_key(local->sequence_kind, local->sequence_elem_type_key);
    row.elem_type_key = local->sequence_elem_type_key;
    row.count_expr_id = body_const_expr_id(count_expr);
    if (may_grow)
        row.flags |= XG_CAPACITY_MAY_GROW;
    if (row.count_expr_id != 0)
        row.flags |= XG_CAPACITY_EXACT_COUNT;
    if (op_kind == XG_CAPACITY_TO_STRING)
        row.flags |= XG_CAPACITY_BUILDER_FINAL;
    (void) xg_global_evidence_add_capacity_op(bc->evidence, &row);
}

static void body_add_bulk_op(XgBodyCollect *bc, const AstNode *node, uint8_t op_kind,
                             const XgLocalType *dst_local, const AstNode *src_expr,
                             const AstNode *length_expr, bool overlap_possible) {
    XgBulkOpSummary row;
    if (!bc || !node || !dst_local)
        return;
    memset(&row, 0, sizeof(row));
    row.op_id = (XgBulkOpId) (bc->evidence->nbulk_ops + 1);
    row.owner_func_id = bc->owner_func_id;
    row.source_span_id = (uint32_t) node->line;
    row.body_ordinal = bc->bulk_op_count++;
    row.op_kind = op_kind;
    row.elem_type_key = dst_local->sequence_elem_type_key;
    row.src_type_key = body_expr_type_key(bc, src_expr);
    row.dst_type_key = dst_local->type_key
                           ? dst_local->type_key
                           : body_sequence_receiver_type_key(dst_local->sequence_kind,
                                                             dst_local->sequence_elem_type_key);
    row.length_expr_id = body_const_expr_id(length_expr);
    if (dst_local->sequence_elem_type_key == body_uint8_type_key() ||
        dst_local->sequence_elem_type_key == body_char_type_key())
        row.flags |= XG_BULK_POD;
    if (overlap_possible)
        row.flags |= XG_BULK_OVERLAP_POSSIBLE;
    if (dst_local->sequence_kind == XG_SEQ_ARRAY)
        row.flags |= XG_BULK_WRITE_BARRIER;
    (void) xg_global_evidence_add_bulk_op(bc->evidence, &row);
}

static void body_add_encoding_op(XgBodyCollect *bc, const AstNode *node, uint8_t op_kind,
                                 const AstNode *input_expr, uint32_t output_type_key,
                                 uint32_t flags) {
    XgEncodingOpSummary row;
    if (!bc || !node)
        return;
    memset(&row, 0, sizeof(row));
    row.op_id = (XgEncodingOpId) (bc->evidence->nencoding_ops + 1);
    row.owner_func_id = bc->owner_func_id;
    row.source_span_id = (uint32_t) node->line;
    row.body_ordinal = bc->encoding_op_count++;
    row.op_kind = op_kind;
    row.input_type_key = body_expr_type_key(bc, input_expr);
    row.output_type_key = output_type_key;
    row.flags = flags;
    if (input_expr && input_expr->type == AST_LITERAL_STRING)
        row.flags |= XG_ENCODING_STATIC_LITERAL | XG_ENCODING_KNOWN_UTF8;
    (void) xg_global_evidence_add_encoding_op(bc->evidence, &row);
}

static void body_add_sequence_method_evidence(XgBodyCollect *bc, const AstNode *node) {
    const CallExprNode *call;
    const MemberAccessNode *member;
    XgLocalType *local;
    const AstNode *arg0;
    if (!bc || !node || node->type != AST_CALL_EXPR)
        return;
    call = &node->as.call_expr;
    if (!call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return;
    member = &call->callee->as.member_access;
    if (!member->name)
        return;
    local = body_lookup_local_sequence(bc, member->object);
    arg0 = call->arguments && call->arg_count > 0 ? call->arguments[0] : NULL;

    if (local) {
        if (strcmp(member->name, "push") == 0 && call->arg_count == 1) {
            body_add_capacity_op(bc, node, local, XG_CAPACITY_PUSH, arg0, true);
            return;
        }
        if ((strcmp(member->name, "append") == 0 || strcmp(member->name, "extend") == 0) &&
            call->arg_count >= 1) {
            body_add_capacity_op(bc, node, local,
                                 strcmp(member->name, "extend") == 0 ? XG_CAPACITY_EXTEND
                                                                     : XG_CAPACITY_APPEND,
                                 arg0, true);
            return;
        }
        if (strcmp(member->name, "reserve") == 0 && call->arg_count == 1) {
            body_add_capacity_op(bc, node, local, XG_CAPACITY_RESERVE, arg0, true);
            return;
        }
        if (strcmp(member->name, "clear") == 0 && call->arg_count == 0) {
            body_add_capacity_op(bc, node, local, XG_CAPACITY_CLEAR, NULL, false);
            return;
        }
        if (strcmp(member->name, "toString") == 0 && call->arg_count == 0) {
            body_add_capacity_op(bc, node, local, XG_CAPACITY_TO_STRING, NULL, false);
            if (local->sequence_kind == XG_SEQ_BYTES ||
                local->sequence_kind == XG_SEQ_STRING_BUILDER)
                body_add_encoding_op(bc, node, XG_ENCODING_BYTES_TO_STRING, member->object,
                                     hash_synthetic_tref32(XR_TREF_STRING, NULL, NULL, 0),
                                     XG_ENCODING_VALIDATED_ONCE | XG_ENCODING_SCALAR_BOUNDARY);
            return;
        }
        if (strcmp(member->name, "appendFrom") == 0 && call->arg_count >= 1) {
            body_add_capacity_op(bc, node, local, XG_CAPACITY_APPEND, arg0, true);
            body_add_bulk_op(bc, node, XG_BULK_COPY, local, arg0, NULL, false);
            return;
        }
        if (strcmp(member->name, "copyFrom") == 0 && call->arg_count >= 1) {
            body_add_bulk_op(bc, node, XG_BULK_COPY, local, arg0, NULL, true);
            return;
        }
        if (strcmp(member->name, "fill") == 0 && call->arg_count >= 1) {
            body_add_bulk_op(bc, node, XG_BULK_FILL, local, arg0, NULL, false);
            return;
        }
    }

    if (strcmp(member->name, "toBytes") == 0 && call->arg_count == 0 &&
        body_expr_type_key(bc, member->object) ==
            hash_synthetic_tref32(XR_TREF_STRING, NULL, NULL, 0)) {
        uint32_t output_type_key = hash_named_type_key32("Bytes", NULL, 0);
        body_add_encoding_op(bc, node, XG_ENCODING_STRING_TO_BYTES, member->object, output_type_key,
                             XG_ENCODING_KNOWN_UTF8 | XG_ENCODING_SCALAR_BOUNDARY);
    }
}

static void body_add_generic_inst(XgBodyCollect *bc, uint8_t kind, const char *name,
                                  XrTypeRef **type_args, int type_arg_count,
                                  XgCallsiteId root_callsite_id, uint32_t source_span_id,
                                  XgDeclId origin_decl_id, XgFuncId origin_func_id,
                                  XgMethodId origin_method_id, XgClassId origin_class_id) {
    XgGenericInstSummary inst;
    if (!bc || !bc->evidence || !name || type_arg_count <= 0 || !type_args)
        return;
    memset(&inst, 0, sizeof(inst));
    inst.generic_inst_id = (XgGenericInstId) (bc->evidence->ngeneric_insts + 1);
    inst.module_id = bc->module_id;
    inst.origin_decl_id = origin_decl_id;
    inst.origin_func_id = origin_func_id;
    inst.origin_method_id = origin_method_id;
    inst.origin_class_id = origin_class_id;
    inst.root_callsite_id = root_callsite_id;
    inst.constraint_interface_id = body_first_constraint_interface(bc, type_args, type_arg_count);
    inst.name_id = hash_name32(name);
    inst.type_key = hash_generic_inst_type_key(name, type_args, type_arg_count, kind);
    inst.type_arg_key_start = hash_tref_list32(type_args, type_arg_count);
    inst.type_arg_count = (uint16_t) (type_arg_count < UINT16_MAX ? type_arg_count : UINT16_MAX);
    inst.source_span_id = source_span_id;
    inst.kind = kind;
    inst.flags = XG_GENERIC_INST_CONCRETE_TYPES;
    if (inst.constraint_interface_id != XG_NO_ID)
        inst.flags |= XG_GENERIC_INST_INTERFACE_CONSTRAINT;
    (void) xg_global_evidence_add_generic_inst(bc->evidence, &inst);
}

static void collect_callsite(XgBodyCollect *bc, const AstNode *call) {
    XgCallsiteSummary row;
    const AstNode *callee;
    XgDeclId generic_origin_decl_id = XG_NO_ID;
    XgFuncId generic_origin_func_id = XG_NO_ID;
    XgMethodId generic_origin_method_id = XG_NO_ID;
    XgClassId generic_origin_class_id = XG_NO_ID;
    const char *generic_name = NULL;
    uint8_t generic_kind = XG_GENERIC_INST_FUNCTION;
    bc->effect_bits |= XG_BODY_MAY_CALL;
    if (body_call_is_sys_thread_spawn(&call->as.call_expr)) {
        bc->effect_bits |= XG_BODY_MAY_ALLOC;
        bc->escape_bits |= XG_BODY_ESCAPE_CORO;
        bc->capability_bits |= XG_CAP_SYS_THREAD | XG_CAP_COROUTINE | XG_CAP_TASK | XG_CAP_OBJECTS;
    }
    memset(&row, 0, sizeof(row));
    row.callsite_id = (XgCallsiteId) (bc->evidence->ncallsites + 1);
    row.owner_func_id = bc->owner_func_id;
    row.source_node_id =
        producer_unique_callsite_source_node_id(bc, producer_source_node_id(bc->module_id, call));
    row.source_span_id = (uint32_t) call->line;
    row.body_ordinal = bc->callsite_count;
    row.kind = XG_CALL_CLOSURE;
    row.arg_count =
        (uint16_t) (call->as.call_expr.arg_count < UINT16_MAX ? call->as.call_expr.arg_count
                                                              : UINT16_MAX);
    row.arg_type_key_start = body_call_arg_type_key_start(bc, call->as.call_expr.arguments,
                                                          call->as.call_expr.arg_count);
    if (call->as.call_expr.default_arg_count > 0)
        row.flags |= XG_CALL_USES_DEFAULT_ARGS;
    callee = call->as.call_expr.callee;
    if (callee && callee->type == AST_VARIABLE) {
        const char *callee_name = callee->as.variable.name;
        XgFuncNameRow *target = producer_lookup_func_row(bc->producer, callee_name);
        uint32_t callee_name_id = hash_name32(callee_name);
        generic_name = callee_name;
        if (target)
            generic_origin_decl_id = target->decl_id;
        if (producer_lookup_class(bc->producer, callee->as.variable.name) != XG_NO_ID)
            bc->capability_bits |= XG_CAP_OBJECTS;
        bc->capability_bits |= body_capabilities_for_builtin_constructor(callee_name);
        if (callee_name && strcmp(callee_name, "typename") == 0)
            bc->metadata_use_bits |= XG_METADATA_TYPENAME;
        if (target && (target->decl_flags & XG_DECL_EXTERN)) {
            bc->effect_bits |= XG_BODY_MAY_CALL_NATIVE;
            bc->escape_bits |= XG_BODY_ESCAPE_EXTERN;
            if ((target->decl_flags & (XG_DECL_NAKED | XG_DECL_INTERRUPT)) == 0)
                bc->capability_bits |= XG_CAP_EXTERN;
            row.kind = XG_CALL_EXTERN;
            row.method_id = (XgMethodId) callee_name_id;
            row.method_name_id = callee_name_id;
        } else if (target && (target->decl_flags & XG_DECL_NATIVE)) {
            bc->effect_bits |= XG_BODY_MAY_CALL_NATIVE;
            bc->escape_bits |= XG_BODY_ESCAPE_NATIVE;
            bc->capability_bits |= XG_CAP_NATIVE;
            row.kind = XG_CALL_NATIVE;
            row.method_id = (XgMethodId) callee_name_id;
            row.method_name_id = callee_name_id;
        } else if (target) {
            row.kind = XG_CALL_DIRECT_FUNC;
            row.static_target_func_id = target->func_id;
            if ((target->decl_flags & (XG_DECL_EXTERN | XG_DECL_NATIVE)) == 0)
                generic_origin_func_id = target->func_id;
        }
    } else if (callee && callee->type == AST_MEMBER_ACCESS) {
        const char *stdlib_module =
            body_stdlib_module_for_expr(bc, callee->as.member_access.object);
        XgInterfaceId receiver_interface =
            body_resolve_expr_interface(bc, callee->as.member_access.object);
        uint32_t method_name_id = hash_name32(callee->as.member_access.name);
        generic_name = callee->as.member_access.name;
        generic_kind = XG_GENERIC_INST_METHOD;
        if (stdlib_module)
            (void) producer_add_stdlib_symbol_dependency(bc->producer, bc->module_id,
                                                         (uint32_t) call->line, stdlib_module,
                                                         callee->as.member_access.name);
        bc->capability_bits |=
            body_capabilities_for_builtin_member_constructor(&callee->as.member_access);
        if (receiver_interface != XG_NO_ID) {
            const XgInterfaceMethodSummary *interface_method =
                producer_find_interface_method_summary(bc->producer, receiver_interface,
                                                       method_name_id);
            row.kind = XG_CALL_INTERFACE;
            row.receiver_static_interface_id = receiver_interface;
            row.method_id = interface_method ? interface_method->interface_method_id
                                             : (XgMethodId) method_name_id;
            row.method_name_id = method_name_id;
            row.method_signature_key = interface_method
                                           ? interface_method->signature_key
                                           : producer_find_interface_method_signature(
                                                 bc->producer, receiver_interface, method_name_id);
        } else {
            XgClassId receiver_class = body_resolve_expr_class(bc, callee->as.member_access.object);
            XgMethodSummary *method = producer_find_method_by_name_in_hierarchy(
                bc->producer, receiver_class, method_name_id, false);
            if (receiver_class != XG_NO_ID) {
                row.kind = XG_CALL_METHOD;
                row.receiver_static_class_id = receiver_class;
                row.method_id = method ? method->method_id : (XgMethodId) method_name_id;
                row.method_name_id = method ? method->name_id : method_name_id;
                row.method_signature_key = method ? method->signature_key : 0;
                if (method)
                    generic_origin_method_id = method->method_id;
                generic_origin_class_id = receiver_class;
                if (method && (method->flags & XG_METHOD_NATIVE) != 0) {
                    bc->effect_bits |= XG_BODY_MAY_CALL_NATIVE;
                    bc->escape_bits |= XG_BODY_ESCAPE_NATIVE;
                    bc->capability_bits |= XG_CAP_NATIVE;
                }
            } else {
                bc->effect_bits |= XG_BODY_MAY_CALL_NATIVE;
                bc->escape_bits |= XG_BODY_ESCAPE_NATIVE;
                row.kind = XG_CALL_NATIVE;
                row.method_id = (XgMethodId) method_name_id;
                row.method_name_id = method_name_id;
            }
        }
    }
    if (bc->callsite_count == 0)
        bc->callsite_start = row.callsite_id;
    if (xg_global_evidence_add_callsite(bc->evidence, &row)) {
        bc->callsite_count++;
        body_add_options_bag_callsite(bc, &call->as.call_expr, &row);
        body_add_generic_inst(bc, generic_kind, generic_name, call->as.call_expr.type_args,
                              call->as.call_expr.type_arg_count, row.callsite_id,
                              (uint32_t) call->line, generic_origin_decl_id, generic_origin_func_id,
                              generic_origin_method_id, generic_origin_class_id);
    }
}

static void collect_super_callsite(XgBodyCollect *bc, const AstNode *call) {
    XgCallsiteSummary row;
    XgClassId parent_class;
    const char *method_name;
    uint32_t method_name_id;
    XgMethodSummary *method;
    bool is_constructor_call;
    if (!bc || !call)
        return;
    bc->effect_bits |= XG_BODY_MAY_CALL;
    parent_class = body_parent_class_id(bc);
    is_constructor_call = call->as.super_call.method_name == NULL;
    method_name = is_constructor_call ? "constructor" : call->as.super_call.method_name;
    method_name_id = hash_name32(method_name);
    method = producer_find_method_by_name_in_hierarchy(bc->producer, parent_class, method_name_id,
                                                       is_constructor_call);
    memset(&row, 0, sizeof(row));
    row.callsite_id = (XgCallsiteId) (bc->evidence->ncallsites + 1);
    row.owner_func_id = bc->owner_func_id;
    row.source_node_id =
        producer_unique_callsite_source_node_id(bc, producer_source_node_id(bc->module_id, call));
    row.source_span_id = (uint32_t) call->line;
    row.body_ordinal = bc->callsite_count;
    row.kind = XG_CALL_METHOD;
    row.receiver_static_class_id = parent_class;
    row.method_id = method ? method->method_id : (XgMethodId) method_name_id;
    row.method_name_id = method ? method->name_id : method_name_id;
    row.method_signature_key = method ? method->signature_key : 0;
    row.arg_count =
        (uint16_t) (call->as.super_call.arg_count < UINT16_MAX ? call->as.super_call.arg_count
                                                               : UINT16_MAX);
    row.arg_type_key_start = body_call_arg_type_key_start(bc, call->as.super_call.arguments,
                                                          call->as.super_call.arg_count);
    if (bc->callsite_count == 0)
        bc->callsite_start = row.callsite_id;
    if (xg_global_evidence_add_callsite(bc->evidence, &row))
        bc->callsite_count++;
}

static bool static_data_node_is_scalar_rodata(const AstNode *node) {
    if (!node)
        return false;
    switch (node->type) {
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_STRING:
        case AST_LITERAL_CHAR:
        case AST_LITERAL_NULL:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_VARIABLE:
        case AST_ENUM_ACCESS:
            return true;
        default:
            return false;
    }
}

static uint32_t static_data_shape_bits_for_expr(const AstNode *node);

static uint32_t static_data_shape_bits_for_all(AstNode **nodes, int count, bool *out_all_rodata,
                                               bool *out_all_freestanding_safe) {
    uint32_t bits = 0;
    bool all_rodata = true;
    bool all_freestanding_safe = true;
    for (int i = 0; i < count; i++) {
        uint32_t child_bits = static_data_shape_bits_for_expr(nodes ? nodes[i] : NULL);
        bits |= child_bits;
        if ((child_bits & XG_STATIC_DATA_RODATA) == 0)
            all_rodata = false;
        if ((child_bits & XG_STATIC_DATA_FREESTANDING_SAFE) == 0)
            all_freestanding_safe = false;
    }
    if (out_all_rodata)
        *out_all_rodata = all_rodata;
    if (out_all_freestanding_safe)
        *out_all_freestanding_safe = all_freestanding_safe;
    return bits;
}

static uint32_t static_data_shape_bits_for_pair(const AstNode *left, const AstNode *right) {
    uint32_t left_bits = static_data_shape_bits_for_expr(left);
    uint32_t right_bits = static_data_shape_bits_for_expr(right);
    uint32_t bits = (left_bits | right_bits) & XG_STATIC_DATA_RUNTIME_INIT;
    if ((left_bits & XG_STATIC_DATA_RODATA) != 0 && (right_bits & XG_STATIC_DATA_RODATA) != 0)
        bits |= XG_STATIC_DATA_RODATA;
    if ((left_bits & XG_STATIC_DATA_FREESTANDING_SAFE) != 0 &&
        (right_bits & XG_STATIC_DATA_FREESTANDING_SAFE) != 0)
        bits |= XG_STATIC_DATA_FREESTANDING_SAFE;
    return bits;
}

static uint32_t static_data_shape_bits_for_expr(const AstNode *node) {
    if (!node)
        return 0;
    if (static_data_node_is_scalar_rodata(node))
        return XG_STATIC_DATA_RODATA | XG_STATIC_DATA_FREESTANDING_SAFE;
    switch (node->type) {
        case AST_LITERAL_BIGINT:
        case AST_LITERAL_REGEX:
        case AST_TEMPLATE_STRING:
        case AST_OBJECT_LITERAL:
        case AST_MAP_LITERAL:
        case AST_SET_LITERAL:
        case AST_NEW_EXPR:
            return XG_STATIC_DATA_RUNTIME_INIT;
        case AST_GROUPING:
            return static_data_shape_bits_for_expr(node->as.grouping);
        case AST_COMPTIME_EXPR:
            return static_data_shape_bits_for_expr(node->as.comptime_expr.expr);
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
        case AST_FORCE_UNWRAP:
            return static_data_shape_bits_for_expr(node->as.unary.operand);
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
        case AST_NULLISH_COALESCE:
            return static_data_shape_bits_for_pair(node->as.binary.left, node->as.binary.right);
        case AST_TERNARY: {
            AstNode *parts[3] = {node->as.ternary.condition, node->as.ternary.true_expr,
                                 node->as.ternary.false_expr};
            bool all_rodata = false;
            bool all_safe = false;
            uint32_t bits = static_data_shape_bits_for_all(parts, 3, &all_rodata, &all_safe);
            bits &= XG_STATIC_DATA_RUNTIME_INIT;
            if (all_rodata)
                bits |= XG_STATIC_DATA_RODATA;
            if (all_safe)
                bits |= XG_STATIC_DATA_FREESTANDING_SAFE;
            return bits;
        }
        case AST_AS_EXPR:
            return static_data_shape_bits_for_expr(node->as.as_expr.expr);
        case AST_INDEX_GET:
            return static_data_shape_bits_for_pair(node->as.index_get.array,
                                                   node->as.index_get.index);
        case AST_MEMBER_ACCESS:
            return static_data_shape_bits_for_expr(node->as.member_access.object);
        case AST_SPREAD_EXPR:
            return static_data_shape_bits_for_expr(node->as.spread_expr.expr);
        case AST_ARRAY_LITERAL: {
            bool all_rodata = false;
            bool all_safe = false;
            uint32_t bits;
            if (node->as.array_literal.is_repeat) {
                bits = static_data_shape_bits_for_pair(node->as.array_literal.repeat_value,
                                                       node->as.array_literal.repeat_count);
                all_rodata = (bits & XG_STATIC_DATA_RODATA) != 0;
                all_safe = (bits & XG_STATIC_DATA_FREESTANDING_SAFE) != 0;
            } else {
                bits = static_data_shape_bits_for_all(node->as.array_literal.elements,
                                                      node->as.array_literal.count, &all_rodata,
                                                      &all_safe);
            }
            bits &= XG_STATIC_DATA_RUNTIME_INIT;
            if (all_rodata)
                bits |= XG_STATIC_DATA_RODATA;
            if (all_safe)
                bits |= XG_STATIC_DATA_FIXED_LAYOUT | XG_STATIC_DATA_FREESTANDING_SAFE;
            return bits;
        }
        case AST_TUPLE_LITERAL: {
            bool all_rodata = false;
            bool all_safe = false;
            uint32_t bits = static_data_shape_bits_for_all(node->as.tuple_literal.elements,
                                                           node->as.tuple_literal.count,
                                                           &all_rodata, &all_safe);
            bits &= XG_STATIC_DATA_RUNTIME_INIT;
            if (all_rodata)
                bits |= XG_STATIC_DATA_RODATA;
            if (all_safe)
                bits |= XG_STATIC_DATA_FIXED_LAYOUT | XG_STATIC_DATA_FREESTANDING_SAFE;
            return bits;
        }
        case AST_STRUCT_LITERAL: {
            bool all_rodata = false;
            bool all_safe = false;
            uint32_t bits = static_data_shape_bits_for_all(node->as.struct_literal.field_values,
                                                           node->as.struct_literal.field_count,
                                                           &all_rodata, &all_safe);
            bits &= XG_STATIC_DATA_RUNTIME_INIT;
            if (all_rodata)
                bits |= XG_STATIC_DATA_RODATA;
            if (all_safe)
                bits |= XG_STATIC_DATA_FIXED_LAYOUT | XG_STATIC_DATA_FREESTANDING_SAFE;
            return bits;
        }
        default:
            return 0;
    }
}

static uint32_t static_data_bits_for_comptime_expr(const AstNode *expr) {
    return XG_STATIC_DATA_COMPTIME_VALUE | static_data_shape_bits_for_expr(expr);
}

static void body_add_function_params(XgBodyCollect *bc, const FunctionDeclNode *function);

static bool body_seed_captured_locals(XgBodyCollect *bc, const XgPendingBody *pending) {
    if (!bc || !pending)
        return true;
    if (pending->captured_name_local_count > 0) {
        if (!body_reserve_name_locals(bc, pending->captured_name_local_count))
            return false;
        memcpy(bc->name_locals, pending->captured_name_locals,
               (size_t) pending->captured_name_local_count * sizeof(*bc->name_locals));
        bc->nname_locals = pending->captured_name_local_count;
    }
    if (pending->captured_local_count > 0) {
        if (!body_reserve_locals(bc, pending->captured_local_count))
            return false;
        memcpy(bc->locals, pending->captured_locals,
               (size_t) pending->captured_local_count * sizeof(*bc->locals));
        bc->nlocals = pending->captured_local_count;
    }
    return true;
}

static void body_enqueue_child_function_body(XgBodyCollect *bc, const AstNode *node,
                                             const FunctionDeclNode *fn) {
    XgFuncId child_func_id;
    XgPendingBody *pending;
    const char *name;
    uint32_t source_node_id;
    uint32_t signature_key;
    if (!bc || !bc->producer || !node || !fn || !fn->body)
        return;
    child_func_id = producer_next_func_id(bc->producer);
    name = fn->name ? fn->name : "<anonymous>";
    signature_key = hash_function_signature(fn);
    source_node_id = producer_unique_body_source_node_id(
        bc->producer, bc->module_id, producer_source_node_id(bc->module_id, node), child_func_id,
        hash_name32(name), signature_key);
    if (!producer_enqueue_body(bc->producer, child_func_id, bc->module_id, XG_NO_ID,
                               bc->current_class_id, XG_NO_ID, hash_name32(name), signature_key,
                               source_node_id, (uint32_t) node->line, XG_BODY_FUNCTION, fn->body,
                               NULL, fn))
        return;
    pending = &bc->producer->bodies[bc->producer->nbodies - 1];
    if (!producer_snapshot_body_captures(pending, bc)) {
        xr_free(pending->captured_locals);
        xr_free(pending->captured_name_locals);
        memset(pending, 0, sizeof(*pending));
        bc->producer->nbodies--;
    }
}

static void walk_body_for_calls(XgBodyCollect *bc, const AstNode *node) {
    if (!bc || !node)
        return;
    switch (node->type) {
        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++) {
                const AstNode *stmt = node->as.program.statements[i];
                if (!stmt)
                    continue;
                switch (stmt->type) {
                    case AST_FUNCTION_DECL:
                    case AST_CLASS_DECL:
                    case AST_STRUCT_DECL:
                    case AST_UNION_DECL:
                    case AST_INTERFACE_DECL:
                    case AST_ENUM_DECL:
                    case AST_IMPORT_STMT:
                    case AST_EXPORT_STMT:
                    case AST_TYPE_ALIAS:
                        break;
                    default:
                        walk_body_for_calls(bc, stmt);
                        break;
                }
            }
            break;
        case AST_CALL_EXPR:
            body_add_json_codec_call(bc, node);
            body_add_map_method_key_access(bc, node);
            body_add_sequence_method_evidence(bc, node);
            collect_callsite(bc, node);
            walk_body_for_calls(bc, node->as.call_expr.callee);
            for (int i = 0; i < node->as.call_expr.arg_count; i++)
                walk_body_for_calls(bc, node->as.call_expr.arguments[i]);
            break;
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR: {
            const FunctionDeclNode *fn =
                node->type == AST_FUNCTION_DECL ? &node->as.function_decl : &node->as.function_expr;
            bool captures = body_function_expr_captures_current_locals(bc, fn);
            if (captures) {
                bc->escape_bits |= XG_BODY_ESCAPE_CAPTURE;
                bc->effect_bits |= XG_BODY_MAY_ALLOC;
                (void) body_add_interface_capture_uses(bc, (uint32_t) node->line);
            }
            if (fn->is_generator)
                bc->capability_bits |= XG_CAP_GENERATOR | XG_CAP_COROUTINE;
            if (node->type == AST_FUNCTION_DECL)
                (void) body_push_name_local(bc, fn->name, fn->symbol_id);
            body_enqueue_child_function_body(bc, node, fn);
            break;
        }
        case AST_SUPER_CALL:
            collect_super_callsite(bc, node);
            for (int i = 0; i < node->as.super_call.arg_count; i++)
                walk_body_for_calls(bc, node->as.super_call.arguments[i]);
            break;
        case AST_BLOCK: {
            uint32_t base_locals = bc->nlocals;
            uint32_t base_name_locals = bc->nname_locals;
            for (int i = 0; i < node->as.block.count; i++)
                walk_body_for_calls(bc, node->as.block.statements[i]);
            bc->nlocals = base_locals;
            bc->nname_locals = base_name_locals;
            break;
        }
        case AST_EXPR_STMT:
            walk_body_for_calls(bc, node->as.expr_stmt);
            break;
        case AST_VARIABLE:
            body_note_variable_read(bc, &node->as.variable);
            break;
        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++)
                walk_body_for_calls(bc, node->as.print_stmt.exprs[i]);
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
        case AST_SHARED_DECL: {
            uint32_t type_key = node->as.var_decl.type_annotation
                                    ? hash_tref32(node->as.var_decl.type_annotation)
                                    : 0;
            const ObjectLiteralNode *json_literal =
                body_type_ref_is_json(node->as.var_decl.type_annotation)
                    ? body_static_object_literal(node->as.var_decl.initializer)
                    : NULL;
            const ObjectLiteralNode *record_literal =
                !json_literal ? body_static_object_literal(node->as.var_decl.initializer) : NULL;
            XgJsonShapeId json_shape_id = XG_NO_ID;
            XgJsonShapeId source_json_shape_id = XG_NO_ID;
            const ObjectLiteralNode *source_json_literal = NULL;
            XgRecordShapeId record_shape_id = XG_NO_ID;
            XgMapShapeId map_shape_id = XG_NO_ID;
            uint8_t map_container_kind = 0;
            uint32_t map_receiver_type_key = 0;
            uint32_t map_key_type_key = 0;
            uint32_t map_value_type_key = 0;
            XgLocalType *source_map_local = NULL;
            uint8_t sequence_kind = 0;
            uint32_t sequence_elem_type_key = 0;
            XgClassId class_id =
                producer_lookup_class_from_tref(bc->producer, node->as.var_decl.type_annotation);
            XgInterfaceId interface_id = producer_lookup_interface_from_tref(
                bc->producer, node->as.var_decl.type_annotation);
            bool inferred = false;
            (void) body_type_ref_sequence_parts(node->as.var_decl.type_annotation, &sequence_kind,
                                                &sequence_elem_type_key);
            (void) body_add_interface_object_uses_for_type_ref(
                bc, node->as.var_decl.type_annotation, 0, (uint32_t) node->line);
            bc->capability_bits |=
                body_capabilities_for_type_ref(node->as.var_decl.type_annotation);
            walk_body_for_calls(bc, node->as.var_decl.initializer);
            source_json_shape_id =
                body_lookup_json_shape(bc, node->as.var_decl.initializer, &source_json_literal);
            source_map_local = body_lookup_local_map_shape(bc, node->as.var_decl.initializer);
            if (node->as.var_decl.initializer &&
                (node->as.var_decl.initializer->type == AST_MAP_LITERAL ||
                 node->as.var_decl.initializer->type == AST_SET_LITERAL)) {
                map_shape_id = body_add_map_shape_for_literal(
                    bc, node->as.var_decl.initializer, node->as.var_decl.type_annotation,
                    (uint32_t) node->line, &map_receiver_type_key, &map_key_type_key,
                    &map_value_type_key, &map_container_kind);
                if (type_key == 0 && map_receiver_type_key != 0)
                    type_key = map_receiver_type_key;
            }
            if (json_literal && type_key == 0)
                type_key = hash_named_type_key32("Json", NULL, 0);
            if (record_literal && type_key == 0)
                type_key = body_record_type_key(record_literal);
            if (class_id == XG_NO_ID && interface_id == XG_NO_ID) {
                uint32_t expr_type_key;
                class_id = body_resolve_expr_class(bc, node->as.var_decl.initializer);
                interface_id = body_resolve_expr_interface(bc, node->as.var_decl.initializer);
                expr_type_key = body_expr_type_key(bc, node->as.var_decl.initializer);
                if (expr_type_key != 0)
                    type_key = expr_type_key;
                inferred = class_id != XG_NO_ID || interface_id != XG_NO_ID || type_key != 0;
            }
            if (json_literal && type_key == 0)
                type_key = hash_named_type_key32("Json", NULL, 0);
            if (record_literal && type_key == 0)
                type_key = body_record_type_key(record_literal);
            (void) body_push_local(bc, node->as.var_decl.name, node->as.var_decl.symbol_id,
                                   class_id, interface_id, type_key, inferred);
            body_bind_record_bridge_shapes_for_type_key(bc, node->as.var_decl.name, type_key);
            if (json_literal) {
                json_shape_id = body_add_json_shape_for_literal(bc, json_literal,
                                                                (uint32_t) node->line, type_key);
                body_bind_json_shape_local(bc, node->as.var_decl.name, json_shape_id, json_literal);
            } else if (source_json_shape_id != XG_NO_ID &&
                       body_local_type_is_json(body_find_local(bc, node->as.var_decl.name))) {
                body_bind_json_shape_local(bc, node->as.var_decl.name, source_json_shape_id,
                                           source_json_literal);
            }
            if (record_literal) {
                record_shape_id = body_add_record_shape_for_literal(
                    bc, record_literal, (uint32_t) node->line, type_key);
                body_bind_record_shape_local(
                    bc, node->as.var_decl.name, record_shape_id,
                    body_record_literal_has_spread(record_literal) ? NULL : record_literal);
            }
            if (map_shape_id != XG_NO_ID) {
                body_bind_map_shape_local(bc, node->as.var_decl.name, map_shape_id,
                                          map_container_kind, map_receiver_type_key,
                                          map_key_type_key, map_value_type_key);
            } else if (source_map_local) {
                body_bind_map_shape_local_from_source(bc, node->as.var_decl.name, source_map_local);
            } else {
                body_bind_map_shape_local_for_type_ref(bc, node->as.var_decl.name,
                                                       node->as.var_decl.type_annotation,
                                                       (uint32_t) node->line);
            }
            if (sequence_kind != 0)
                body_bind_sequence_local(
                    bc, node->as.var_decl.name, sequence_kind, sequence_elem_type_key,
                    body_type_ref_sequence_elem_type_ref(node->as.var_decl.type_annotation));
            body_add_generic_array_storage(bc, node->as.var_decl.type_annotation,
                                           (uint32_t) node->line);
            break;
        }
        case AST_ASSIGNMENT: {
            XgLocalType *target_row = body_find_local(bc, node->as.assignment.name);
            bool target_is_json = body_local_type_is_json(target_row);
            const ObjectLiteralNode *json_literal =
                target_is_json ? body_static_object_literal(node->as.assignment.value) : NULL;
            const ObjectLiteralNode *source_json_literal = NULL;
            XgJsonShapeId source_json_shape_id =
                body_lookup_json_shape(bc, node->as.assignment.value, &source_json_literal);
            XgLocalType *source_map_local =
                body_lookup_local_map_shape(bc, node->as.assignment.value);
            XgClassId class_id;
            XgInterfaceId interface_id;
            uint32_t type_key;
            walk_body_for_calls(bc, node->as.assignment.value);
            class_id = body_resolve_expr_class(bc, node->as.assignment.value);
            interface_id = body_resolve_expr_interface(bc, node->as.assignment.value);
            type_key = body_expr_type_key(bc, node->as.assignment.value);
            body_clear_json_shape_local(bc, node->as.assignment.name);
            body_clear_record_shape_local(bc, node->as.assignment.name);
            body_clear_map_shape_local(bc, node->as.assignment.name);
            body_clear_sequence_local(bc, node->as.assignment.name);
            body_assign_local(bc, node->as.assignment.name, node->as.assignment.symbol_id, class_id,
                              interface_id, type_key);
            if (target_row && target_row->interface_id != XG_NO_ID) {
                uint32_t target_type_key =
                    target_row->type_key != 0
                        ? target_row->type_key
                        : body_interface_type_key(bc, target_row->interface_id);
                uint32_t ordinal = ++bc->interface_object_use_count;
                (void) producer_add_interface_object_use_row(
                    bc->producer, target_row->interface_id, bc->owner_func_id,
                    (uint32_t) node->line, ordinal, target_type_key, XG_INTERFACE_OBJECT_USE_VALUE);
            }
            target_row = body_find_local(bc, node->as.assignment.name);
            body_bind_record_bridge_shapes_for_type_key(
                bc, node->as.assignment.name,
                target_row && target_row->type_key != 0 ? target_row->type_key : type_key);
            if (json_literal) {
                XgJsonShapeId json_shape_id = body_add_json_shape_for_literal(
                    bc, json_literal, (uint32_t) node->line,
                    type_key ? type_key : hash_named_type_key32("Json", NULL, 0));
                body_bind_json_shape_local(bc, node->as.assignment.name, json_shape_id,
                                           json_literal);
            } else if (source_json_shape_id != XG_NO_ID &&
                       body_local_type_is_json(body_find_local(bc, node->as.assignment.name))) {
                body_bind_json_shape_local(bc, node->as.assignment.name, source_json_shape_id,
                                           source_json_literal);
            }
            if (source_map_local)
                body_bind_map_shape_local_from_source(bc, node->as.assignment.name,
                                                      source_map_local);
            bc->effect_bits |= XG_BODY_MAY_MUTATE;
            break;
        }
        case AST_MEMBER_ACCESS: {
            const char *stdlib_module =
                body_stdlib_module_for_expr(bc, node->as.member_access.object);
            bc->effect_bits |= XG_BODY_MAY_READ_MEM;
            if (stdlib_module &&
                producer_stdlib_member_is_constant(stdlib_module, node->as.member_access.name)) {
                (void) producer_add_stdlib_symbol_dependency(bc->producer, bc->module_id,
                                                             (uint32_t) node->line, stdlib_module,
                                                             node->as.member_access.name);
            }
            body_add_json_member_access(bc, node, false);
            body_add_record_member_access(bc, node, false);
            body_add_sequence_length_access(bc, node);
            walk_body_for_calls(bc, node->as.member_access.object);
            break;
        }
        case AST_MEMBER_SET:
            body_add_json_member_access(bc, node, true);
            body_add_record_member_access(bc, node, true);
            walk_body_for_calls(bc, node->as.member_set.object);
            walk_body_for_calls(bc, node->as.member_set.value);
            bc->effect_bits |= XG_BODY_MAY_MUTATE;
            bc->escape_bits |= XG_BODY_ESCAPE_FIELD;
            break;
        case AST_INDEX_GET:
            bc->effect_bits |= XG_BODY_MAY_READ_MEM;
            body_add_json_index_access(bc, node, false);
            body_add_map_index_key_access(bc, node, false);
            body_add_sequence_index_access(bc, node, false);
            walk_body_for_calls(bc, node->as.index_get.array);
            walk_body_for_calls(bc, node->as.index_get.index);
            break;
        case AST_INDEX_SET:
            body_add_json_index_access(bc, node, true);
            body_add_map_index_key_access(bc, node, true);
            body_add_sequence_index_access(bc, node, true);
            walk_body_for_calls(bc, node->as.index_set.array);
            walk_body_for_calls(bc, node->as.index_set.index);
            walk_body_for_calls(bc, node->as.index_set.value);
            bc->effect_bits |= XG_BODY_MAY_MUTATE;
            bc->escape_bits |= XG_BODY_ESCAPE_CONTAINER;
            break;
        case AST_SLICE_EXPR:
            bc->effect_bits |= XG_BODY_MAY_READ_MEM;
            body_add_sequence_slice_access(bc, node);
            walk_body_for_calls(bc, node->as.slice_expr.source);
            walk_body_for_calls(bc, node->as.slice_expr.start);
            walk_body_for_calls(bc, node->as.slice_expr.end);
            break;
        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++)
                walk_body_for_calls(bc, node->as.return_stmt.values[i]);
            if (node->as.return_stmt.value_count > 0)
                (void) body_add_interface_object_uses_for_type_ref(
                    bc, bc->return_type, XG_INTERFACE_OBJECT_USE_RETURN, (uint32_t) node->line);
            if (node->as.return_stmt.value_count > 0)
                bc->escape_bits |= XG_BODY_ESCAPE_RETURN;
            break;
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
        case AST_NULLISH_COALESCE:
            walk_body_for_calls(bc, node->as.binary.left);
            walk_body_for_calls(bc, node->as.binary.right);
            break;
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
        case AST_FORCE_UNWRAP:
            walk_body_for_calls(bc, node->as.unary.operand);
            break;
        case AST_GROUPING:
            walk_body_for_calls(bc, node->as.grouping);
            break;
        case AST_TERNARY:
            walk_body_for_calls(bc, node->as.ternary.condition);
            walk_body_for_calls(bc, node->as.ternary.true_expr);
            walk_body_for_calls(bc, node->as.ternary.false_expr);
            break;
        case AST_AS_EXPR:
            walk_body_for_calls(bc, node->as.as_expr.expr);
            break;
        case AST_IS_EXPR:
            bc->capability_bits |= XG_CAP_INSTANCEOF;
            walk_body_for_calls(bc, node->as.is_expr.expr);
            break;
        case AST_COMPTIME_EXPR:
            bc->static_data_use_bits |=
                static_data_bits_for_comptime_expr(node->as.comptime_expr.expr);
            walk_body_for_calls(bc, node->as.comptime_expr.expr);
            break;
        case AST_ARRAY_LITERAL:
            bc->effect_bits |= XG_BODY_MAY_ALLOC;
            if (node->as.array_literal.elements) {
                for (int i = 0; i < node->as.array_literal.count; i++)
                    walk_body_for_calls(bc, node->as.array_literal.elements[i]);
            }
            walk_body_for_calls(bc, node->as.array_literal.repeat_value);
            walk_body_for_calls(bc, node->as.array_literal.repeat_count);
            break;
        case AST_TUPLE_LITERAL:
            if (node->as.tuple_literal.elements) {
                for (int i = 0; i < node->as.tuple_literal.count; i++)
                    walk_body_for_calls(bc, node->as.tuple_literal.elements[i]);
            }
            break;
        case AST_OBJECT_LITERAL:
            bc->effect_bits |= XG_BODY_MAY_ALLOC;
            if (node->as.object_literal.keys || node->as.object_literal.values) {
                for (int i = 0; i < node->as.object_literal.count; i++) {
                    walk_body_for_calls(
                        bc, node->as.object_literal.keys ? node->as.object_literal.keys[i] : NULL);
                    walk_body_for_calls(bc, node->as.object_literal.values
                                                ? node->as.object_literal.values[i]
                                                : NULL);
                }
            }
            break;
        case AST_MAP_LITERAL:
            bc->effect_bits |= XG_BODY_MAY_ALLOC;
            if (node->as.map_literal.keys || node->as.map_literal.values) {
                for (int i = 0; i < node->as.map_literal.count; i++) {
                    walk_body_for_calls(bc, node->as.map_literal.keys ? node->as.map_literal.keys[i]
                                                                      : NULL);
                    walk_body_for_calls(
                        bc, node->as.map_literal.values ? node->as.map_literal.values[i] : NULL);
                }
            }
            break;
        case AST_SET_LITERAL:
            bc->effect_bits |= XG_BODY_MAY_ALLOC;
            if (node->as.set_literal.elements) {
                for (int i = 0; i < node->as.set_literal.count; i++)
                    walk_body_for_calls(bc, node->as.set_literal.elements[i]);
            }
            break;
        case AST_STRUCT_LITERAL:
            if (node->as.struct_literal.type_arg_count > 0) {
                XgClassId origin_class =
                    producer_lookup_class(bc->producer, node->as.struct_literal.struct_name);
                body_add_generic_inst(
                    bc, XG_GENERIC_INST_CLASS, node->as.struct_literal.struct_name,
                    node->as.struct_literal.type_args, node->as.struct_literal.type_arg_count,
                    XG_NO_ID, (uint32_t) node->line,
                    producer_lookup_class_decl_id(bc->producer, origin_class), XG_NO_ID, XG_NO_ID,
                    origin_class);
            }
            if (node->as.struct_literal.field_values) {
                for (int i = 0; i < node->as.struct_literal.field_count; i++)
                    walk_body_for_calls(bc, node->as.struct_literal.field_values[i]);
            }
            break;
        case AST_NEW_EXPR:
            bc->effect_bits |= XG_BODY_MAY_ALLOC;
            bc->capability_bits |= XG_CAP_OBJECTS;
            bc->capability_bits |=
                body_capabilities_for_builtin_constructor(node->as.new_expr.class_name);
            if (node->as.new_expr.type_arg_count > 0) {
                XgClassId origin_class =
                    producer_lookup_class(bc->producer, node->as.new_expr.class_name);
                body_add_generic_inst(bc, XG_GENERIC_INST_CLASS, node->as.new_expr.class_name,
                                      node->as.new_expr.type_args, node->as.new_expr.type_arg_count,
                                      XG_NO_ID, (uint32_t) node->line,
                                      producer_lookup_class_decl_id(bc->producer, origin_class),
                                      XG_NO_ID, XG_NO_ID, origin_class);
            }
            for (int i = 0; i < node->as.new_expr.arg_count; i++)
                walk_body_for_calls(bc, node->as.new_expr.arguments[i]);
            break;
        case AST_THROW_STMT:
            bc->effect_bits |= XG_BODY_MAY_THROW;
            walk_body_for_calls(bc, node->as.throw_stmt.expression);
            break;
        case AST_AWAIT_EXPR:
            bc->effect_bits |= XG_BODY_MAY_SUSPEND;
            bc->capability_bits |= XG_CAP_COROUTINE;
            if (node->as.await_expr.timeout)
                bc->capability_bits |= XG_CAP_TIMER | XG_CAP_CHANNEL | XG_CAP_OBJECTS;
            walk_body_for_calls(bc, node->as.await_expr.expr);
            walk_body_for_calls(bc, node->as.await_expr.timeout);
            walk_body_for_calls(bc, node->as.await_expr.into);
            break;
        case AST_YIELD_STMT:
            bc->effect_bits |= XG_BODY_MAY_SUSPEND;
            bc->capability_bits |= XG_CAP_GENERATOR | XG_CAP_COROUTINE;
            walk_body_for_calls(bc, node->as.yield_stmt.value);
            break;
        case AST_GO_EXPR:
            bc->effect_bits |= XG_BODY_MAY_SUSPEND | XG_BODY_MAY_ALLOC;
            bc->escape_bits |= XG_BODY_ESCAPE_CORO;
            bc->capability_bits |= XG_CAP_COROUTINE | XG_CAP_TASK | XG_CAP_NETPOLL | XG_CAP_OBJECTS;
            if (node->as.go_expr.spawn_kind == XR_SPAWN_THREAD)
                bc->capability_bits |= XG_CAP_SYS_THREAD;
            walk_body_for_calls(bc, node->as.go_expr.expr);
            break;
        case AST_CHANNEL_NEW:
            bc->effect_bits |= XG_BODY_MAY_ALLOC;
            bc->capability_bits |= XG_CAP_CHANNEL | XG_CAP_COROUTINE | XG_CAP_OBJECTS;
            walk_body_for_calls(bc, node->as.channel_new.buffer_size);
            break;
        case AST_SELECT_STMT:
            bc->effect_bits |= XG_BODY_MAY_SUSPEND;
            bc->capability_bits |= XG_CAP_CHANNEL | XG_CAP_COROUTINE | XG_CAP_OBJECTS;
            for (int i = 0; i < node->as.select_stmt.case_count; i++)
                walk_body_for_calls(bc, node->as.select_stmt.cases[i]);
            break;
        case AST_SELECT_CASE: {
            uint32_t base_name_locals = bc->nname_locals;
            if (node->as.select_case.is_timeout)
                bc->capability_bits |=
                    XG_CAP_TIMER | XG_CAP_CHANNEL | XG_CAP_COROUTINE | XG_CAP_OBJECTS;
            walk_body_for_calls(bc, node->as.select_case.channel);
            walk_body_for_calls(bc, node->as.select_case.value);
            (void) body_push_name_local(bc, node->as.select_case.var_name,
                                        node->as.select_case.var_symbol_id);
            walk_body_for_calls(bc, node->as.select_case.body);
            bc->nname_locals = base_name_locals;
            break;
        }
        case AST_DEFER_STMT:
            walk_body_for_calls(bc, node->as.defer_stmt.expr);
            break;
        case AST_SCOPE_BLOCK:
            bc->capability_bits |= XG_CAP_SCOPE | XG_CAP_COROUTINE | XG_CAP_OBJECTS;
            walk_body_for_calls(bc, node->as.scope_block.body);
            break;
        case AST_CANCELLED_EXPR:
            bc->capability_bits |= XG_CAP_COROUTINE;
            break;
        case AST_MOVE_EXPR:
            walk_body_for_calls(bc, node->as.move_expr.expr);
            break;
        case AST_UNSAFE_EXPR:
            walk_body_for_calls(bc, node->as.unsafe_expr.operand);
            break;
        case AST_COMPOUND_ASSIGNMENT:
            walk_body_for_calls(bc, node->as.compound_assignment.object);
            walk_body_for_calls(bc, node->as.compound_assignment.value);
            bc->effect_bits |= XG_BODY_MAY_MUTATE;
            break;
        case AST_INC:
        case AST_DEC:
            bc->effect_bits |= XG_BODY_MAY_MUTATE;
            break;
        case AST_IF_STMT:
            walk_body_for_calls(bc, node->as.if_stmt.condition);
            walk_body_for_calls(bc, node->as.if_stmt.then_branch);
            walk_body_for_calls(bc, node->as.if_stmt.else_branch);
            break;
        case AST_WHILE_STMT:
            walk_body_for_calls(bc, node->as.while_stmt.condition);
            walk_body_for_calls(bc, node->as.while_stmt.body);
            break;
        case AST_FOR_STMT: {
            uint32_t base_locals = bc->nlocals;
            uint32_t base_name_locals = bc->nname_locals;
            walk_body_for_calls(bc, node->as.for_stmt.initializer);
            walk_body_for_calls(bc, node->as.for_stmt.condition);
            walk_body_for_calls(bc, node->as.for_stmt.increment);
            walk_body_for_calls(bc, node->as.for_stmt.body);
            bc->nlocals = base_locals;
            bc->nname_locals = base_name_locals;
            break;
        }
        case AST_FOR_IN_STMT: {
            uint32_t base_locals = bc->nlocals;
            uint32_t base_name_locals = bc->nname_locals;
            XgClassId item_class =
                producer_lookup_class_from_tref(bc->producer, node->as.for_in_stmt.item_type);
            XgInterfaceId item_interface =
                producer_lookup_interface_from_tref(bc->producer, node->as.for_in_stmt.item_type);
            uint32_t item_type_key =
                node->as.for_in_stmt.item_type ? hash_tref32(node->as.for_in_stmt.item_type) : 0;
            bc->capability_bits |= body_capabilities_for_type_ref(node->as.for_in_stmt.item_type);
            bc->effect_bits |= XG_BODY_MAY_READ_MEM;
            walk_body_for_calls(bc, node->as.for_in_stmt.collection);
            (void) body_push_name_local(bc, node->as.for_in_stmt.value_name,
                                        node->as.for_in_stmt.value_symbol_id);
            (void) body_push_local(bc, node->as.for_in_stmt.item_name,
                                   node->as.for_in_stmt.item_symbol_id, item_class, item_interface,
                                   item_type_key, false);
            walk_body_for_calls(bc, node->as.for_in_stmt.body);
            bc->nlocals = base_locals;
            bc->nname_locals = base_name_locals;
            break;
        }
        case AST_TRY_CATCH:
            bc->effect_bits |= XG_BODY_MAY_THROW;
            for (int i = 0; i < node->as.try_catch.catch_count; i++) {
                XrCatchClause *cc =
                    node->as.try_catch.catch_clauses ? node->as.try_catch.catch_clauses[i] : NULL;
                if (cc && cc->is_panic) {
                    bc->capability_bits |= XG_CAP_EXCEPTION;
                    break;
                }
            }
            walk_body_for_calls(bc, node->as.try_catch.try_body);
            for (int i = 0; i < node->as.try_catch.catch_count; i++) {
                uint32_t base_name_locals = bc->nname_locals;
                XrCatchClause *cc =
                    node->as.try_catch.catch_clauses ? node->as.try_catch.catch_clauses[i] : NULL;
                if (cc)
                    (void) body_push_name_local(bc, cc->var_name, cc->symbol_id);
                walk_body_for_calls(bc, node->as.try_catch.catch_clauses
                                            ? node->as.try_catch.catch_clauses[i]->body
                                            : NULL);
                bc->nname_locals = base_name_locals;
            }
            break;
        default:
            break;
    }
}

static void body_add_method_params(XgBodyCollect *bc, const MethodDeclNode *method) {
    if (!bc || !method)
        return;
    for (int i = 0; i < method->param_count; i++) {
        XgClassId class_id = producer_lookup_class_from_tref(
            bc->producer, method->param_types ? method->param_types[i] : NULL);
        XgInterfaceId interface_id = producer_lookup_interface_from_tref(
            bc->producer, method->param_types ? method->param_types[i] : NULL);
        uint32_t type_key =
            method->param_types && method->param_types[i] ? hash_tref32(method->param_types[i]) : 0;
        uint8_t sequence_kind = 0;
        uint32_t sequence_elem_type_key = 0;
        bc->capability_bits |=
            body_capabilities_for_type_ref(method->param_types ? method->param_types[i] : NULL);
        (void) body_add_interface_object_uses_for_type_ref(
            bc, method->param_types ? method->param_types[i] : NULL, XG_INTERFACE_OBJECT_USE_PARAM,
            0);
        (void) body_push_local(bc, method->parameters ? method->parameters[i] : NULL, 0, class_id,
                               interface_id, type_key, false);
        body_bind_map_shape_local_for_type_ref(
            bc, method->parameters ? method->parameters[i] : NULL,
            method->param_types ? method->param_types[i] : NULL, 0);
        if (body_type_ref_sequence_parts(method->param_types ? method->param_types[i] : NULL,
                                         &sequence_kind, &sequence_elem_type_key))
            body_bind_sequence_local(bc, method->parameters ? method->parameters[i] : NULL,
                                     sequence_kind, sequence_elem_type_key,
                                     body_type_ref_sequence_elem_type_ref(
                                         method->param_types ? method->param_types[i] : NULL));
        body_add_generic_array_storage(bc, method->param_types ? method->param_types[i] : NULL, 0);
    }
}

static void body_add_function_params(XgBodyCollect *bc, const FunctionDeclNode *function) {
    if (!bc || !function)
        return;
    for (int i = 0; i < function->param_count; i++) {
        XrParamNode *param = function->params ? function->params[i] : NULL;
        XgClassId class_id =
            producer_lookup_class_from_tref(bc->producer, param ? param->type : NULL);
        XgInterfaceId interface_id =
            producer_lookup_interface_from_tref(bc->producer, param ? param->type : NULL);
        uint32_t type_key = param && param->type ? hash_tref32(param->type) : 0;
        uint8_t sequence_kind = 0;
        uint32_t sequence_elem_type_key = 0;
        bc->capability_bits |= body_capabilities_for_type_ref(param ? param->type : NULL);
        (void) body_add_interface_object_uses_for_type_ref(
            bc, param ? param->type : NULL, XG_INTERFACE_OBJECT_USE_PARAM,
            param && param->line > 0 ? (uint32_t) param->line : 0);
        (void) body_push_local(bc, param ? param->name : NULL, param ? param->symbol_id : 0,
                               class_id, interface_id, type_key, false);
        body_bind_map_shape_local_for_type_ref(
            bc, param ? param->name : NULL, param ? param->type : NULL,
            param && param->line > 0 ? (uint32_t) param->line : 0);
        if (body_type_ref_sequence_parts(param ? param->type : NULL, &sequence_kind,
                                         &sequence_elem_type_key))
            body_bind_sequence_local(
                bc, param ? param->name : NULL, sequence_kind, sequence_elem_type_key,
                body_type_ref_sequence_elem_type_ref(param ? param->type : NULL));
        body_add_generic_array_storage(bc, param ? param->type : NULL, 0);
    }
}

static bool add_body_summary(XgProducer *producer, const XgPendingBody *pending) {
    XgBodyCollect bc;
    XgBodySummary row;
    XgPendingBody pending_copy;
    if (!producer || !pending || !pending->body)
        return true;
    pending_copy = *pending;
    pending = &pending_copy;
    memset(&bc, 0, sizeof(bc));
    bc.producer = producer;
    bc.evidence = producer->evidence;
    bc.owner_func_id = pending->func_id;
    bc.module_id = pending->module_id;
    bc.current_class_id = pending->current_class_id;
    bc.return_type = pending->method     ? pending->method->return_type
                     : pending->function ? pending->function->return_type
                                         : NULL;
    if (!body_seed_captured_locals(&bc, pending)) {
        xr_free(bc.locals);
        xr_free(bc.name_locals);
        return false;
    }
    body_add_method_params(&bc, pending->method);
    body_add_function_params(&bc, pending->function);
    if (pending->function && pending->function->is_generator)
        bc.capability_bits |= XG_CAP_GENERATOR | XG_CAP_COROUTINE;
    walk_body_for_calls(&bc, pending->body);

    memset(&row, 0, sizeof(row));
    row.func_id = pending->func_id;
    row.module_id = pending->module_id;
    row.source_node_id = pending->source_node_id;
    row.owner_decl_id = pending->owner_decl_id;
    row.owner_class_id = pending->current_class_id;
    row.owner_method_id = pending->owner_method_id;
    row.name_id = pending->name_id;
    row.signature_key = pending->signature_key;
    row.source_span_id = pending->source_span_id;
    row.kind = pending->kind;
    row.body_hash = hash_ast_shape(pending->body, XR_FNV64_OFFSET_BASIS);
    row.effect_bits = bc.effect_bits;
    row.escape_bits = bc.escape_bits;
    row.capability_bits = bc.capability_bits;
    row.callsite_start = bc.callsite_start;
    row.callsite_count = bc.callsite_count;
    row.metadata_use_bits = bc.metadata_use_bits;
    row.static_data_use_bits = bc.static_data_use_bits;
    xr_free(bc.locals);
    xr_free(bc.name_locals);
    return xg_global_evidence_add_body(producer->evidence, &row) != NULL;
}

static bool producer_emit_body_summaries(XgProducer *producer) {
    if (!producer)
        return false;
    for (uint32_t i = 0; i < producer->nbodies; i++) {
        if (!add_body_summary(producer, &producer->bodies[i]))
            return false;
    }
    return true;
}

static bool add_function_decl(XgProducer *p, XgModuleId module_id, const AstNode *node) {
    const FunctionDeclNode *fn = &node->as.function_decl;
    XgDeclSummary decl;
    XgDeclId decl_id = (XgDeclId) (p->evidence->ndecls + 1);
    XgFuncId func_id = producer_next_func_id(p);
    XrAttribute *extern_attr = attrs_find(fn->attributes, fn->attr_count, ATTR_EXTERN);
    XrAttribute *native_attr = attrs_find(fn->attributes, fn->attr_count, ATTR_NATIVE);
    XrAttribute *c_export_attr = attrs_find(fn->attributes, fn->attr_count, ATTR_C_EXPORT);
    XrAttribute *dylib_attr = attrs_find(fn->attributes, fn->attr_count, ATTR_DYLIB);
    XrAttribute *naked_attr = attrs_find(fn->attributes, fn->attr_count, ATTR_NAKED);
    XrAttribute *interrupt_attr = attrs_find(fn->attributes, fn->attr_count, ATTR_INTERRUPT);
    memset(&decl, 0, sizeof(decl));
    decl.module_id = module_id;
    decl.source_node_id = producer_source_node_id(module_id, node);
    decl.decl_id = decl_id;
    decl.kind = XG_DECL_FUNC;
    decl.name_id = hash_name32(fn->name);
    decl.signature_key = hash_function_signature(fn);
    decl.source_span_id = (uint32_t) node->line;
    if (native_attr)
        decl.flags |= XG_DECL_NATIVE;
    if (extern_attr)
        decl.flags |= XG_DECL_EXTERN;
    if (c_export_attr)
        decl.flags |= XG_DECL_C_EXPORT;
    if (naked_attr)
        decl.flags |= XG_DECL_NAKED;
    if (interrupt_attr)
        decl.flags |= XG_DECL_INTERRUPT;
    if (!xg_global_evidence_add_decl(p->evidence, &decl))
        return false;
    if (extern_attr && dylib_attr && dylib_attr->str_arg && dylib_attr->str_arg[0]) {
        if (!producer_add_link_dependency(p, module_id, decl_id, (uint32_t) node->line,
                                          XG_LINK_DEP_EXTERN_DYLIB, dylib_attr->str_arg))
            return false;
    }
    if (!producer_register_func(p, fn->name, func_id, decl_id, decl.flags))
        return false;
    return producer_enqueue_body(p, func_id, module_id, decl_id, XG_NO_ID, XG_NO_ID,
                                 hash_name32(fn->name), decl.signature_key, decl.source_node_id,
                                 (uint32_t) node->line, XG_BODY_FUNCTION, fn->body, NULL, fn);
}

static bool add_monomorphized_class_instantiation(XgProducer *p, XgModuleId module_id,
                                                  const AstNode *node, const ClassDeclNode *cls,
                                                  XgClassId specialized_class_id) {
    XgGenericInstSummary inst;
    XgClassId origin_class_id;
    const char *origin_name;
    if (!p || !p->evidence || !cls || !cls->is_monomorphized || !cls->generic_origin_name ||
        cls->mono_type_arg_count <= 0 || !cls->mono_type_arg_names)
        return true;

    origin_name = cls->generic_origin_name;
    origin_class_id = producer_lookup_class(p, origin_name);

    memset(&inst, 0, sizeof(inst));
    inst.generic_inst_id = (XgGenericInstId) (p->evidence->ngeneric_insts + 1);
    inst.module_id = module_id;
    inst.origin_class_id = origin_class_id;
    inst.origin_decl_id =
        origin_class_id != XG_NO_ID ? producer_lookup_class_decl_id(p, origin_class_id) : XG_NO_ID;
    inst.specialized_class_id = specialized_class_id;
    inst.name_id = hash_name32(origin_name);
    inst.type_key = hash_generic_inst_name_type_key(
        origin_name, cls->mono_type_arg_names, cls->mono_type_arg_count, XG_GENERIC_INST_CLASS);
    inst.type_arg_key_start = hash_name_list32(cls->mono_type_arg_names, cls->mono_type_arg_count);
    inst.type_arg_count =
        (uint16_t) (cls->mono_type_arg_count < UINT16_MAX ? cls->mono_type_arg_count : UINT16_MAX);
    inst.source_span_id = node ? (uint32_t) node->line : 0;
    inst.kind = XG_GENERIC_INST_CLASS;
    inst.flags = XG_GENERIC_INST_CONCRETE_TYPES | XG_GENERIC_INST_SPECIALIZED_ABI |
                 XG_GENERIC_INST_CONCRETE_STORAGE;
    return xg_global_evidence_add_generic_inst(p->evidence, &inst) != NULL;
}

static bool add_class_like_decl(XgProducer *p, XgModuleId module_id, const AstNode *node,
                                XgDeclKind kind) {
    const ClassDeclNode *cls = node->type == AST_CLASS_DECL    ? &node->as.class_decl
                               : node->type == AST_STRUCT_DECL ? &node->as.struct_decl
                                                               : &node->as.union_decl;
    XgDeclSummary decl;
    XgClassSummary csum;
    XgDeclId decl_id = (XgDeclId) (p->evidence->ndecls + 1);
    XgClassId class_id = (XgClassId) (p->evidence->nclasses + 1);
    uint32_t field_start = p->evidence->nclass_fields + 1;
    uint32_t field_count = 0;
    uint32_t instance_field_count = 0;
    uint32_t method_start = p->evidence->nmethods + 1;
    uint32_t method_count = 0;
    uint32_t derive_flags = attrs_derive_flags(cls->attributes, cls->attr_count);
    memset(&decl, 0, sizeof(decl));
    decl.module_id = module_id;
    decl.source_node_id = producer_source_node_id(module_id, node);
    decl.decl_id = decl_id;
    decl.kind = (uint8_t) kind;
    decl.name_id = hash_name32(cls->name);
    decl.source_span_id = (uint32_t) node->line;
    if (cls->is_native)
        decl.flags |= XG_DECL_NATIVE;
    if (cls->explicit_final)
        decl.flags |= XG_DECL_FINAL;
    if (derive_flags != 0)
        decl.flags |= XG_DECL_DERIVE;
    decl.derive_flags = derive_flags;
    if (!xg_global_evidence_add_decl(p->evidence, &decl))
        return false;
    for (int i = 0; i < cls->method_count; i++) {
        const AstNode *method_node = cls->methods[i];
        XgMethodSummary method;
        const MethodDeclNode *m;
        XgFuncId method_func_id;
        if (!method_node || method_node->type != AST_METHOD_DECL)
            continue;
        m = &method_node->as.method_decl;
        method_func_id = producer_next_func_id(p);
        memset(&method, 0, sizeof(method));
        method.method_id = (XgMethodId) (p->evidence->nmethods + 1);
        method.owner_class_id = class_id;
        method.source_node_id = producer_source_node_id(module_id, method_node);
        method.name_id = hash_name32(m->name);
        method.signature_key = hash_method_signature(m);
        if (m->is_static)
            method.flags |= XG_METHOD_STATIC;
        if (m->is_constructor)
            method.flags |= XG_METHOD_CONSTRUCTOR;
        if (cls->is_native || !m->body)
            method.flags |= XG_METHOD_NATIVE;
        if (!xg_global_evidence_add_method(p->evidence, &method))
            return false;
        method_count++;
        if (!producer_enqueue_body(p, method_func_id, module_id, decl_id, class_id,
                                   method.method_id, hash_name32(m->name), method.signature_key,
                                   method.source_node_id, (uint32_t) method_node->line,
                                   XG_BODY_METHOD, m->body, m, NULL))
            return false;
    }
    for (int i = 0; i < cls->field_count; i++) {
        const AstNode *field_node = cls->fields ? cls->fields[i] : NULL;
        const FieldDeclNode *field;
        XgClassFieldSummary summary;
        if (!field_node || field_node->type != AST_FIELD_DECL)
            continue;
        field = &field_node->as.field_decl;
        memset(&summary, 0, sizeof(summary));
        summary.field_id = (XgFieldId) (p->evidence->nclass_fields + 1);
        summary.module_id = module_id;
        summary.source_node_id = producer_source_node_id(module_id, field_node);
        summary.owner_class_id = class_id;
        summary.name_id = hash_name32(field->name);
        summary.type_key = hash_tref32(field->field_type);
        summary.decl_ordinal = (uint32_t) i;
        summary.instance_slot = field->is_static ? UINT32_MAX : instance_field_count++;
        class_field_fill_type_facts(&summary, field->field_type);
        summary.flags = class_field_flags(field, summary.semantic_kind, summary.flags);
        if (!xg_global_evidence_add_class_field(p->evidence, &summary))
            return false;
        field_count++;
        if (!producer_add_interface_object_uses_for_type_ref(
                p, XG_NO_ID, (uint32_t) field_node->line, NULL, field->field_type,
                XG_INTERFACE_OBJECT_USE_FIELD, 0))
            return false;
    }

    memset(&csum, 0, sizeof(csum));
    csum.module_id = module_id;
    csum.decl_id = decl_id;
    csum.class_id = class_id;
    csum.name_id = hash_name32(cls->name);
    {
        XgClassNameRow *parent =
            producer_lookup_class_row_scoped(p, module_id, hash_name32(cls->super_name), true);
        csum.parent_class_id = parent ? parent->class_id : XG_NO_ID;
    }
    if (cls->explicit_final)
        csum.flags |= XG_CLASS_EXPLICIT_FINAL;
    if (cls->is_native)
        csum.flags |= XG_CLASS_NATIVE;
    if (cls->is_generic_skeleton)
        csum.flags |= XG_CLASS_GENERIC_SKELETON;
    if (cls->is_monomorphized) {
        const char *origin_name = cls->generic_origin_name;
        csum.flags |= XG_CLASS_MONOMORPHIZED;
        csum.generic_origin_class_id = producer_lookup_class(p, origin_name);
        csum.generic_origin_name_id = hash_name32(origin_name);
        csum.generic_type_key = hash_generic_inst_name_type_key(
            origin_name, cls->mono_type_arg_names, cls->mono_type_arg_count, XG_GENERIC_INST_CLASS);
        csum.generic_type_arg_key_start =
            hash_name_list32(cls->mono_type_arg_names, cls->mono_type_arg_count);
        csum.generic_type_arg_count =
            (uint16_t) (cls->mono_type_arg_count < UINT16_MAX ? cls->mono_type_arg_count
                                                              : UINT16_MAX);
    }
    csum.field_start = field_count > 0 ? field_start : 0;
    csum.field_count = field_count;
    csum.method_start = method_count > 0 ? method_start : 0;
    csum.method_count = method_count;
    csum.interface_start = cls->interface_count > 0 ? p->evidence->ninterface_impls + 1 : 0;
    csum.interface_count = (uint32_t) cls->interface_count;
    csum.decl_kind = (uint8_t) kind;
    for (int i = 0; i < cls->interface_count; i++) {
        XgInterfaceImplSummary impl;
        const XrTypeRef *iface = cls->interfaces ? cls->interfaces[i] : NULL;
        uint32_t name_id = hash_name32(iface ? iface->name : NULL);
        memset(&impl, 0, sizeof(impl));
        impl.implementor_class_id = class_id;
        impl.interface_id = (XgInterfaceId) name_id;
        impl.name_id = name_id;
        impl.type_key = hash_tref32(iface);
        impl.source_span_id = (uint32_t) node->line;
        if (!xg_global_evidence_add_interface_impl(p->evidence, &impl))
            return false;
    }
    if (!xg_global_evidence_add_class(p->evidence, &csum))
        return false;
    if (!producer_add_decl_derives(p, module_id, decl_id, (uint32_t) node->line, cls->name,
                                   derive_flags, cls, class_id))
        return false;
    if (!add_monomorphized_class_instantiation(p, module_id, node, cls, class_id))
        return false;
    return producer_register_class(p, module_id, cls->name, cls->super_name, class_id,
                                   p->evidence->nclasses - 1);
}

static bool add_interface_decl(XgProducer *p, XgModuleId module_id, const AstNode *node) {
    const InterfaceDeclNode *iface = &node->as.interface_decl;
    XgDeclSummary decl;
    XgInterfaceId interface_id = (XgInterfaceId) hash_name32(iface->name);
    memset(&decl, 0, sizeof(decl));
    decl.module_id = module_id;
    decl.source_node_id = producer_source_node_id(module_id, node);
    decl.decl_id = (XgDeclId) (p->evidence->ndecls + 1);
    decl.kind = XG_DECL_INTERFACE;
    decl.name_id = interface_id;
    decl.signature_key = (uint32_t) iface->method_count;
    decl.source_span_id = (uint32_t) node->line;
    if (!xg_global_evidence_add_decl(p->evidence, &decl))
        return false;
    for (int i = 0; i < iface->extends_count; i++) {
        const XrTypeRef *parent = iface->extends ? iface->extends[i] : NULL;
        const char *parent_name = xr_tref_head_name(parent);
        XgInterfaceExtendsSummary edge;
        memset(&edge, 0, sizeof(edge));
        edge.child_interface_id = interface_id;
        edge.parent_interface_id = (XgInterfaceId) hash_name32(parent_name);
        edge.name_id = edge.parent_interface_id;
        edge.type_key = hash_tref32(parent);
        edge.source_span_id = (uint32_t) node->line;
        if (!xg_global_evidence_add_interface_extends(p->evidence, &edge))
            return false;
    }
    for (int i = 0; i < iface->method_count; i++) {
        const AstNode *method_node = iface->methods ? iface->methods[i] : NULL;
        const InterfaceMethodNode *method;
        XgInterfaceMethodSummary summary;
        if (!method_node || method_node->type != AST_INTERFACE_METHOD)
            continue;
        method = &method_node->as.interface_method;
        memset(&summary, 0, sizeof(summary));
        summary.interface_method_id = (XgInterfaceMethodId) (p->evidence->ninterface_methods + 1);
        summary.owner_interface_id = interface_id;
        summary.name_id = hash_name32(method->name);
        summary.signature_key = hash_interface_method_signature(method);
        summary.ordinal = (uint32_t) i;
        summary.source_span_id = (uint32_t) method_node->line;
        if (!xg_global_evidence_add_interface_method(p->evidence, &summary))
            return false;
    }
    return producer_register_interface(p, module_id, iface->name, iface);
}

static bool add_enum_decl(XgProducer *p, XgModuleId module_id, const AstNode *node) {
    const EnumDeclNode *e = &node->as.enum_decl;
    XgDeclSummary decl;
    uint32_t derive_flags = attrs_derive_flags(e->attributes, e->attr_count);
    memset(&decl, 0, sizeof(decl));
    decl.module_id = module_id;
    decl.source_node_id = producer_source_node_id(module_id, node);
    decl.decl_id = (XgDeclId) (p->evidence->ndecls + 1);
    decl.kind = XG_DECL_ENUM;
    decl.name_id = hash_name32(e->name);
    decl.signature_key = (uint32_t) e->member_count;
    decl.source_span_id = (uint32_t) node->line;
    if (derive_flags != 0)
        decl.flags |= XG_DECL_DERIVE;
    decl.derive_flags = derive_flags;
    if (!xg_global_evidence_add_decl(p->evidence, &decl))
        return false;
    return producer_add_decl_derives(p, module_id, decl.decl_id, (uint32_t) node->line, e->name,
                                     derive_flags, NULL, 0);
}

static bool add_import_link_dependencies(XgProducer *p, XgModuleId module_id, const AstNode *node) {
    const ImportStmtNode *import;
    if (!p || !node || node->type != AST_IMPORT_STMT)
        return true;
    import = &node->as.import_stmt;
    if (!producer_stdlib_module_known(import->module_name))
        return true;
    if (!producer_add_link_dependency(p, module_id, XG_NO_ID, (uint32_t) node->line,
                                      XG_LINK_DEP_STDLIB_MODULE, import->module_name))
        return false;
    if (import->member_count == 0) {
        return producer_register_stdlib_import(p, module_id, import->alias, import->module_name,
                                               NULL);
    }
    for (int i = 0; i < import->member_count; i++) {
        const ImportMember *member = &import->members[i];
        const char *local_name = member->alias ? member->alias : member->name;
        if (!producer_add_stdlib_symbol_dependency(p, module_id, (uint32_t) node->line,
                                                   import->module_name, member->name))
            return false;
        if (!producer_register_stdlib_import(p, module_id, local_name, import->module_name,
                                             member->name))
            return false;
    }
    return true;
}

static bool add_type_alias_record_shape(XgProducer *p, XgModuleId module_id, const AstNode *node) {
    const TypeAliasNode *alias;
    if (!p || !p->evidence || !node || node->type != AST_TYPE_ALIAS)
        return true;
    alias = &node->as.type_alias;
    if (!alias->name || alias->type_param_count > 0 ||
        body_type_alias_record_field_count(alias) <= 0)
        return true;
    if (body_add_record_shape_for_type_alias(p->evidence, module_id, alias,
                                             (uint32_t) node->line) == XG_NO_ID)
        return false;
    if (body_add_json_record_bridge_shape_for_type_alias(p->evidence, module_id, alias,
                                                         (uint32_t) node->line) == XG_NO_ID)
        return false;
    return true;
}

static bool module_stmt_has_runtime_body(const AstNode *stmt) {
    if (!stmt)
        return false;
    if (stmt->type == AST_EXPORT_STMT && stmt->as.export_stmt.declaration)
        return module_stmt_has_runtime_body(stmt->as.export_stmt.declaration);
    switch (stmt->type) {
        case AST_FUNCTION_DECL:
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
        case AST_UNION_DECL:
        case AST_INTERFACE_DECL:
        case AST_ENUM_DECL:
        case AST_IMPORT_STMT:
        case AST_EXPORT_STMT:
        case AST_TYPE_ALIAS:
            return false;
        default:
            return true;
    }
}

static bool add_module_decl_stmt(XgProducer *p, XgModuleId module_id, const AstNode *stmt,
                                 bool *handled) {
    if (handled)
        *handled = true;
    if (!stmt)
        return true;
    if (stmt->type == AST_EXPORT_STMT && stmt->as.export_stmt.declaration)
        return add_module_decl_stmt(p, module_id, stmt->as.export_stmt.declaration, handled);
    switch (stmt->type) {
        case AST_FUNCTION_DECL:
            return add_function_decl(p, module_id, stmt);
        case AST_CLASS_DECL:
            return add_class_like_decl(p, module_id, stmt, XG_DECL_CLASS);
        case AST_STRUCT_DECL:
            return add_class_like_decl(p, module_id, stmt, XG_DECL_STRUCT);
        case AST_UNION_DECL:
            return add_class_like_decl(p, module_id, stmt, XG_DECL_UNION);
        case AST_INTERFACE_DECL:
            return add_interface_decl(p, module_id, stmt);
        case AST_ENUM_DECL:
            return add_enum_decl(p, module_id, stmt);
        case AST_IMPORT_STMT:
            return add_import_link_dependencies(p, module_id, stmt);
        case AST_TYPE_ALIAS:
            return add_type_alias_record_shape(p, module_id, stmt);
        default:
            if (handled)
                *handled = false;
            return true;
    }
}

static bool add_module_ast(XgProducer *p, XgModuleId module_id, const AstNode *ast) {
    bool has_module_body = false;
    if (!ast || ast->type != AST_PROGRAM)
        return true;
    for (int i = 0; i < ast->as.program.count; i++) {
        const AstNode *stmt = ast->as.program.statements[i];
        if (!stmt)
            continue;
        bool handled = false;
        if (!add_module_decl_stmt(p, module_id, stmt, &handled))
            return false;
        if (!handled && module_stmt_has_runtime_body(stmt))
            has_module_body = true;
    }
    if (has_module_body) {
        XgFuncId module_func_id = producer_next_func_id(p);
        if (!producer_enqueue_body(p, module_func_id, module_id, XG_NO_ID, XG_NO_ID, XG_NO_ID,
                                   hash_name32("<module-init>"), 0, 0, 0, XG_BODY_MODULE_INIT, ast,
                                   NULL, NULL))
            return false;
    }
    return true;
}

static uint64_t module_source_hash(const XrModuleSpec *spec);

static uint64_t fold_graph_module_source(uint64_t h, uint64_t module_id, const XrModuleSpec *spec) {
    size_t len = 0;
    char *source = NULL;
    const char *embedded = NULL;
    h = fold_u64(h, module_id);
    if (!spec)
        return h;
    if (spec->source_path)
        h = fold_bytes(h, spec->source_path, strlen(spec->source_path));
    if (spec->embedded_source && spec->canonical)
        embedded = xr_get_embedded_stdlib(spec->canonical);
    if (embedded) {
        h = fold_bytes(h, embedded, strlen(embedded));
    } else {
        source = spec->source_path ? xr_file_read_all(spec->source_path, "rb", &len) : NULL;
        if (source) {
            h = fold_bytes(h, source, len);
            xr_free(source);
        }
    }
    return h;
}

static uint64_t source_hash_for_graph(const XrModuleGraph *graph) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    if (!graph)
        return h;
    for (int ti = 0; ti < graph->topo_count; ti++) {
        int idx = graph->topo_order[ti];
        h = fold_graph_module_source(h, (uint64_t) (ti + 1), &graph->specs[idx]);
    }
    return h;
}

static uint64_t module_source_hash(const XrModuleSpec *spec) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    if (!spec)
        return h;
    h = fold_u64(h, (uint64_t) spec->kind);
    if (spec->canonical)
        h = fold_bytes(h, spec->canonical, strlen(spec->canonical));
    if (spec->source_path)
        h = fold_bytes(h, spec->source_path, strlen(spec->source_path));
    return h ? h : 1;
}

XR_FUNC bool xg_module_summary_from_module_spec(XgModuleSummary *out_summary, XgModuleId module_id,
                                                const XrModuleSpec *spec) {
    const char *name;
    if (!out_summary || !spec || module_id == XG_NO_ID)
        return false;
    memset(out_summary, 0, sizeof(*out_summary));
    name = spec->canonical ? spec->canonical : spec->source_path;
    out_summary->module_id = module_id;
    out_summary->name_id = hash_name32(name ? name : "<memory-module>");
    out_summary->canonical_hash = hash_text64(spec->canonical);
    out_summary->source_hash = module_source_hash(spec);
    out_summary->kind = (uint8_t) spec->kind;
    out_summary->flags = spec->embedded_source ? XG_MODULE_EMBEDDED_SOURCE : 0;
    return true;
}

static bool add_module_summary(XgGlobalEvidence *evidence, XgModuleId module_id,
                               const XrModuleSpec *spec) {
    XgModuleSummary row;
    if (!evidence || !xg_module_summary_from_module_spec(&row, module_id, spec))
        return false;
    return xg_global_evidence_add_module(evidence, &row) != NULL;
}

static bool module_identity_is_imported(const XgModuleSummary *imported_modules,
                                        uint32_t imported_module_count,
                                        const XgModuleSummary *candidate) {
    if (!candidate || !imported_modules || imported_module_count == 0)
        return false;
    for (uint32_t i = 0; i < imported_module_count; i++) {
        if (xg_module_summary_identity_matches(&imported_modules[i], candidate))
            return true;
    }
    return false;
}

XR_FUNC bool xg_standalone_build_key_from_module_spec(XgBuildKey *out_key, const XrModuleSpec *spec,
                                                      uint32_t profile,
                                                      uint64_t imported_summary_hash) {
    const XrModuleSpec *specs[1];
    if (!spec)
        return false;
    specs[0] = spec;
    return xg_build_key_from_ordered_module_specs(out_key, specs, 1, profile,
                                                  imported_summary_hash);
}

XR_FUNC bool xg_build_key_from_ordered_module_specs(XgBuildKey *out_key,
                                                    const XrModuleSpec *const *specs,
                                                    uint32_t spec_count, uint32_t profile,
                                                    uint64_t imported_summary_hash) {
    XgBuildKey key;
    uint64_t source_hash = XR_FNV64_OFFSET_BASIS;
    if (!out_key || !specs || spec_count == 0)
        return false;
    for (uint32_t i = 0; i < spec_count; i++) {
        if (!specs[i])
            return false;
        source_hash = fold_graph_module_source(source_hash, (uint64_t) (i + 1), specs[i]);
    }
    memset(&key, 0, sizeof(key));
    key.source_hash = source_hash;
    key.compiler_semver_hash = UINT64_C(0x0000017200000001);
    key.profile_hash = fold_u64(XR_FNV64_OFFSET_BASIS, profile);
    key.imported_summary_hash = imported_summary_hash;
    key.module_id = 1;
    key.profile = profile;
    *out_key = key;
    return true;
}

XR_FUNC bool xg_build_key_from_module_graph(XgBuildKey *out_key, const XrModuleGraph *graph,
                                            uint32_t profile, uint64_t imported_summary_hash) {
    XgBuildKey key;
    if (!out_key || !graph)
        return false;
    memset(&key, 0, sizeof(key));
    key.source_hash = source_hash_for_graph(graph);
    key.compiler_semver_hash = UINT64_C(0x0000017200000001);
    key.profile_hash = fold_u64(XR_FNV64_OFFSET_BASIS, profile);
    key.imported_summary_hash = imported_summary_hash;
    key.module_id = (XgModuleId) (graph->entry_index >= 0 ? graph->entry_index + 1 : 0);
    key.profile = profile;
    *out_key = key;
    return true;
}

XR_FUNC bool xg_global_evidence_build_from_module_graph(XgGlobalEvidence *evidence,
                                                        const XrModuleGraph *graph,
                                                        uint32_t profile,
                                                        uint64_t imported_summary_hash) {
    return xg_global_evidence_build_from_module_graph_with_imported_modules(
        evidence, graph, profile, imported_summary_hash, NULL, 0);
}

XR_FUNC bool xg_global_evidence_build_from_module_graph_with_imported_modules(
    XgGlobalEvidence *evidence, const XrModuleGraph *graph, uint32_t profile,
    uint64_t imported_summary_hash, const XgModuleSummary *imported_modules,
    uint32_t imported_module_count) {
    XgBuildKey key;
    XgProducer producer;
    if (!evidence || !graph || (imported_module_count > 0 && !imported_modules))
        return false;
    if (!xg_build_key_from_module_graph(&key, graph, profile, imported_summary_hash))
        return false;

    xg_global_evidence_init(evidence, key);
    memset(&producer, 0, sizeof(producer));
    producer.evidence = evidence;
    producer.next_func_id = 1;

    for (int ti = 0; ti < graph->topo_count; ti++) {
        int idx = graph->topo_order[ti];
        const XrModuleSpec *spec = &graph->specs[idx];
        XgModuleId module_id = (XgModuleId) (ti + 1);
        XgModuleSummary module_summary;
        if (!xg_module_summary_from_module_spec(&module_summary, module_id, spec) ||
            !xg_global_evidence_add_module(evidence, &module_summary)) {
            xr_free(producer.classes);
            xr_free(producer.interfaces);
            xr_free(producer.funcs);
            xr_free(producer.stdlib_imports);
            producer_free_bodies(&producer);
            xg_global_evidence_free(evidence);
            return false;
        }
        if (module_identity_is_imported(imported_modules, imported_module_count, &module_summary))
            continue;
        if (!add_module_ast(&producer, module_id, spec->ast)) {
            xr_free(producer.classes);
            xr_free(producer.interfaces);
            xr_free(producer.funcs);
            xr_free(producer.stdlib_imports);
            producer_free_bodies(&producer);
            xg_global_evidence_free(evidence);
            return false;
        }
    }

    if (!producer_finalize_class_graph(&producer) || !producer_emit_body_summaries(&producer)) {
        xr_free(producer.classes);
        xr_free(producer.interfaces);
        xr_free(producer.funcs);
        xr_free(producer.stdlib_imports);
        producer_free_bodies(&producer);
        xg_global_evidence_free(evidence);
        return false;
    }
    xr_free(producer.classes);
    xr_free(producer.interfaces);
    xr_free(producer.funcs);
    xr_free(producer.stdlib_imports);
    producer_free_bodies(&producer);
    return true;
}

static const XgDeclSummary *evidence_find_decl_by_id(const XgGlobalEvidence *ev, XgDeclId id) {
    if (!ev || id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->ndecls; i++) {
        if (ev->decls[i].decl_id == id)
            return &ev->decls[i];
    }
    return NULL;
}

static const XgDeclSummary *evidence_find_matching_decl(const XgGlobalEvidence *ev,
                                                        const XgDeclSummary *src) {
    const XgDeclSummary *match = NULL;
    if (!ev || !src || src->source_node_id == 0)
        return NULL;
    for (uint32_t i = 0; i < ev->ndecls; i++) {
        const XgDeclSummary *decl = &ev->decls[i];
        if (decl->module_id != src->module_id || decl->source_node_id != src->source_node_id)
            continue;
        if (match)
            return NULL;
        match = decl;
    }
    return match;
}

static const XgBodySummary *evidence_find_body_by_func_id(const XgGlobalEvidence *ev,
                                                          XgFuncId func_id) {
    if (!ev || func_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->nbodies; i++) {
        if (ev->bodies[i].func_id == func_id)
            return &ev->bodies[i];
    }
    return NULL;
}

static const XgBodySummary *evidence_find_matching_body(const XgGlobalEvidence *ev,
                                                        const XgBodySummary *src) {
    const XgBodySummary *match = NULL;
    if (!ev || !src || (src->source_node_id == 0 && src->kind != XG_BODY_MODULE_INIT))
        return NULL;
    for (uint32_t i = 0; i < ev->nbodies; i++) {
        const XgBodySummary *body = &ev->bodies[i];
        if (body->module_id != src->module_id || body->source_node_id != src->source_node_id)
            continue;
        if (match)
            return NULL;
        match = body;
    }
    return match;
}

static const XgClassSummary *evidence_find_class_by_id(const XgGlobalEvidence *ev,
                                                       XgClassId class_id) {
    if (!ev || class_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->nclasses; i++) {
        if (ev->classes[i].class_id == class_id)
            return &ev->classes[i];
    }
    return NULL;
}

static const XgClassSummary *evidence_find_matching_class(const XgGlobalEvidence *ev,
                                                          const XgClassSummary *src,
                                                          XgDeclId remapped_decl_id) {
    if (!ev || !src)
        return NULL;
    for (uint32_t i = 0; i < ev->nclasses; i++) {
        const XgClassSummary *cls = &ev->classes[i];
        if (cls->module_id == src->module_id && cls->decl_kind == src->decl_kind &&
            cls->name_id == src->name_id &&
            (remapped_decl_id == XG_NO_ID || cls->decl_id == remapped_decl_id))
            return cls;
    }
    return NULL;
}

static const XgMethodSummary *evidence_find_method_by_id(const XgGlobalEvidence *ev,
                                                         XgMethodId method_id) {
    if (!ev || method_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->nmethods; i++) {
        if (ev->methods[i].method_id == method_id)
            return &ev->methods[i];
    }
    return NULL;
}

static const XgMethodSummary *evidence_find_matching_method(const XgGlobalEvidence *ev,
                                                            const XgMethodSummary *src,
                                                            XgClassId remapped_owner_class_id) {
    const XgMethodSummary *match = NULL;
    if (!ev || !src || src->source_node_id == 0 || remapped_owner_class_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->nmethods; i++) {
        const XgMethodSummary *method = &ev->methods[i];
        if (method->owner_class_id != remapped_owner_class_id ||
            method->source_node_id != src->source_node_id)
            continue;
        if (match)
            return NULL;
        match = method;
    }
    return match;
}

static const XgCallsiteSummary *evidence_find_callsite_by_id(const XgGlobalEvidence *ev,
                                                             XgCallsiteId callsite_id) {
    if (!ev || callsite_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < ev->ncallsites; i++) {
        if (ev->callsites[i].callsite_id == callsite_id)
            return &ev->callsites[i];
    }
    return NULL;
}

static const XgCallsiteSummary *
evidence_find_matching_callsite(const XgGlobalEvidence *ev, const XgCallsiteSummary *src,
                                const XgBodySummary *remapped_owner_body) {
    const XgCallsiteSummary *match = NULL;
    if (!ev || !src || src->source_node_id == 0 || !remapped_owner_body)
        return NULL;
    for (uint32_t i = 0; i < ev->ncallsites; i++) {
        const XgCallsiteSummary *call = &ev->callsites[i];
        if (call->owner_func_id != remapped_owner_body->func_id)
            continue;
        if (call->source_node_id != src->source_node_id)
            continue;
        if (match)
            return NULL;
        match = call;
    }
    return match;
}

static bool evidence_has_equivalent_generic_inst(const XgGlobalEvidence *ev,
                                                 const XgGenericInstSummary *inst) {
    if (!ev || !inst)
        return false;
    for (uint32_t i = 0; i < ev->ngeneric_insts; i++) {
        const XgGenericInstSummary *row = &ev->generic_insts[i];
        if (row->module_id != inst->module_id || row->kind != inst->kind ||
            row->name_id != inst->name_id || row->type_key != inst->type_key ||
            row->type_arg_key_start != inst->type_arg_key_start ||
            row->type_arg_count != inst->type_arg_count ||
            row->origin_decl_id != inst->origin_decl_id ||
            row->origin_func_id != inst->origin_func_id ||
            row->origin_method_id != inst->origin_method_id ||
            row->origin_class_id != inst->origin_class_id ||
            row->constraint_interface_id != inst->constraint_interface_id)
            continue;
        if (inst->specialized_func_id != XG_NO_ID || row->specialized_func_id != XG_NO_ID) {
            if (row->specialized_func_id == inst->specialized_func_id)
                return true;
            continue;
        }
        if (inst->specialized_class_id != XG_NO_ID || row->specialized_class_id != XG_NO_ID) {
            if (row->specialized_class_id == inst->specialized_class_id)
                return true;
            continue;
        }
        if (row->root_callsite_id == inst->root_callsite_id)
            return true;
    }
    return false;
}

static uint32_t evidence_body_size_estimate(const XgBodySummary *body) {
    if (!body)
        return 1;
    return 1u + body->callsite_count;
}

static uint64_t evidence_generic_body_use_hash(const XgGenericInstSummary *inst,
                                               const XgCallsiteSummary *call,
                                               const XgBodySummary *origin_body,
                                               const XgBodySummary *specialized_body) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    h = fold_u64(h, inst ? inst->module_id : 0);
    h = fold_u64(h, inst ? inst->kind : 0);
    h = fold_u64(h, inst ? inst->name_id : 0);
    h = fold_u64(h, inst ? inst->type_key : 0);
    h = fold_u64(h, inst ? inst->type_arg_key_start : 0);
    h = fold_u64(h, inst ? inst->type_arg_count : 0);
    h = fold_u64(h, call ? call->callsite_id : 0);
    h = fold_u64(h, call ? call->static_target_func_id : 0);
    h = fold_u64(h, origin_body ? origin_body->body_hash : 0);
    h = fold_u64(h, specialized_body ? specialized_body->body_hash : 0);
    return h ? h : 1;
}

static bool evidence_add_generic_function_deepen_rows(XgGlobalEvidence *dst,
                                                      const XgGenericInstSummary *inst,
                                                      const XgCallsiteSummary *call,
                                                      const XgBodySummary *owner_body,
                                                      const XgBodySummary *origin_body,
                                                      const XgBodySummary *specialized_body) {
    XgGenericBodyUseSummary body_use;
    XgGenericCodeSizeSummary code_size;
    uint32_t origin_size;
    uint32_t specialized_size;
    if (!dst || !inst || !call || !origin_body || !specialized_body ||
        inst->kind != XG_GENERIC_INST_FUNCTION || call->static_target_func_id == XG_NO_ID ||
        call->static_target_func_id == origin_body->func_id ||
        call->static_target_func_id != specialized_body->func_id)
        return true;

    origin_size = evidence_body_size_estimate(origin_body);
    specialized_size = evidence_body_size_estimate(specialized_body);

    memset(&body_use, 0, sizeof(body_use));
    body_use.use_id = (XgGenericBodyUseId) (dst->ngeneric_body_uses + 1);
    body_use.generic_inst_id = inst->generic_inst_id;
    body_use.module_id = inst->module_id;
    body_use.owner_func_id = owner_body ? owner_body->func_id : call->owner_func_id;
    body_use.origin_body_func_id = origin_body->func_id;
    body_use.specialized_body_func_id = specialized_body->func_id;
    body_use.root_callsite_id = call->callsite_id;
    body_use.type_key = inst->type_key;
    body_use.type_arg_key_start = inst->type_arg_key_start;
    body_use.type_arg_count = inst->type_arg_count;
    body_use.estimated_body_size = specialized_size;
    body_use.flags = XG_GENERIC_BODY_EXPLICIT_ROOT;
    body_use.body_use_hash =
        evidence_generic_body_use_hash(inst, call, origin_body, specialized_body);
    if (!xg_global_evidence_add_generic_body_use(dst, &body_use))
        return false;

    memset(&code_size, 0, sizeof(code_size));
    code_size.code_size_id = (XgGenericCodeSizeId) (dst->ngeneric_code_sizes + 1);
    code_size.generic_inst_id = inst->generic_inst_id;
    code_size.module_id = inst->module_id;
    code_size.body_use_id = body_use.use_id;
    code_size.origin_body_size_estimate = origin_size;
    code_size.specialized_body_size_estimate = specialized_size;
    code_size.instantiation_count = 1;
    code_size.threshold = 64;
    if ((uint64_t) specialized_size * (uint64_t) code_size.instantiation_count <=
        (uint64_t) code_size.threshold)
        code_size.flags = XG_GENERIC_CODESIZE_ALLOW_CLONE;
    return xg_global_evidence_add_generic_code_size(dst, &code_size) != NULL;
}

XR_FUNC bool xg_global_evidence_merge_generic_inst_roots(XgGlobalEvidence *dst,
                                                         const XgGlobalEvidence *roots) {
    if (!dst || !roots)
        return false;
    for (uint32_t i = 0; i < roots->ngeneric_insts; i++) {
        const XgGenericInstSummary *src = &roots->generic_insts[i];
        XgGenericInstSummary mapped = *src;
        const XgDeclSummary *src_decl = evidence_find_decl_by_id(roots, src->origin_decl_id);
        const XgDeclSummary *dst_decl = evidence_find_matching_decl(dst, src_decl);
        const XgBodySummary *src_origin_body =
            evidence_find_body_by_func_id(roots, src->origin_func_id);
        const XgBodySummary *dst_origin_body = NULL;
        const XgClassSummary *src_class = evidence_find_class_by_id(roots, src->origin_class_id);
        const XgClassSummary *dst_class = NULL;
        const XgMethodSummary *src_method =
            evidence_find_method_by_id(roots, src->origin_method_id);
        const XgMethodSummary *dst_method = NULL;
        const XgCallsiteSummary *src_call =
            evidence_find_callsite_by_id(roots, src->root_callsite_id);
        const XgBodySummary *src_owner_body =
            src_call ? evidence_find_body_by_func_id(roots, src_call->owner_func_id) : NULL;
        const XgBodySummary *dst_owner_body = NULL;
        const XgCallsiteSummary *dst_call = NULL;
        const XgBodySummary *dst_specialized_body = NULL;

        mapped.generic_inst_id = (XgGenericInstId) (dst->ngeneric_insts + 1);
        mapped.origin_decl_id = dst_decl ? dst_decl->decl_id : XG_NO_ID;

        if (src_origin_body)
            dst_origin_body = evidence_find_matching_body(dst, src_origin_body);
        mapped.origin_func_id = dst_origin_body ? dst_origin_body->func_id : XG_NO_ID;

        if (src_class) {
            XgDeclId remapped_class_decl = mapped.origin_decl_id;
            if (src_class->decl_id != src->origin_decl_id) {
                const XgDeclSummary *class_decl =
                    evidence_find_decl_by_id(roots, src_class->decl_id);
                const XgDeclSummary *mapped_class_decl =
                    evidence_find_matching_decl(dst, class_decl);
                remapped_class_decl = mapped_class_decl ? mapped_class_decl->decl_id : XG_NO_ID;
            }
            dst_class = evidence_find_matching_class(dst, src_class, remapped_class_decl);
        }
        mapped.origin_class_id = dst_class ? dst_class->class_id : XG_NO_ID;

        if (src_method) {
            XgClassId remapped_owner_class = XG_NO_ID;
            const XgClassSummary *src_owner_class =
                evidence_find_class_by_id(roots, src_method->owner_class_id);
            if (src_owner_class) {
                const XgDeclSummary *owner_decl =
                    evidence_find_decl_by_id(roots, src_owner_class->decl_id);
                const XgDeclSummary *mapped_owner_decl =
                    evidence_find_matching_decl(dst, owner_decl);
                const XgClassSummary *dst_owner_class = evidence_find_matching_class(
                    dst, src_owner_class,
                    mapped_owner_decl ? mapped_owner_decl->decl_id : XG_NO_ID);
                remapped_owner_class = dst_owner_class ? dst_owner_class->class_id : XG_NO_ID;
            }
            dst_method = evidence_find_matching_method(dst, src_method, remapped_owner_class);
        }
        mapped.origin_method_id = dst_method ? dst_method->method_id : XG_NO_ID;

        if (src_owner_body)
            dst_owner_body = evidence_find_matching_body(dst, src_owner_body);
        dst_call = evidence_find_matching_callsite(dst, src_call, dst_owner_body);
        mapped.root_callsite_id = dst_call ? dst_call->callsite_id : XG_NO_ID;

        mapped.specialized_func_id = XG_NO_ID;
        mapped.specialized_class_id = XG_NO_ID;
        mapped.flags &= ~(XG_GENERIC_INST_SPECIALIZED_BODY | XG_GENERIC_INST_SPECIALIZED_ABI |
                          XG_GENERIC_INST_CONCRETE_STORAGE);
        if (mapped.kind == XG_GENERIC_INST_FUNCTION && dst_call &&
            dst_call->kind == XG_CALL_DIRECT_FUNC && dst_call->static_target_func_id != XG_NO_ID &&
            dst_call->static_target_func_id != mapped.origin_func_id) {
            dst_specialized_body =
                evidence_find_body_by_func_id(dst, dst_call->static_target_func_id);
            if (dst_specialized_body) {
                mapped.specialized_func_id = dst_specialized_body->func_id;
                mapped.flags |= XG_GENERIC_INST_SPECIALIZED_BODY | XG_GENERIC_INST_SPECIALIZED_ABI;
            }
        }

        if (evidence_has_equivalent_generic_inst(dst, &mapped))
            continue;
        if (!xg_global_evidence_add_generic_inst(dst, &mapped))
            return false;
        if (!evidence_add_generic_function_deepen_rows(dst, &mapped, dst_call, dst_owner_body,
                                                       dst_origin_body, dst_specialized_body))
            return false;
    }
    return true;
}
