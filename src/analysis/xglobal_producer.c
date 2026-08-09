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
#include "../os/os_fs.h"
#include "../os/os_proc.h"
#include "../frontend/analyzer/xbuiltin_receiver_registry.h"
#include "../frontend/analyzer/xa_selection.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../frontend/parser/xast.h"
#include "../frontend/parser/xtype_ref.h"
#include "../module/xmodule_graph.h"
#include "../module/xstdlib_embedded.h"
#include "../shared/xr_derive_flags.h"
#include "../shared/xr_hash_core.h"
#include "../stdlib/xstdlib_metadata.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    XG_SMALL_MAP_LITERAL_MAX = 4,
    XG_DENSE_ENUM_MEMBER_MAX = 256
};

#define XG_COMPILER_SEMVER_HASH UINT64_C(0x0000017200000005)

typedef struct XgClassNameRow {
    XgModuleId module_id;
    const char *name;
    const char *super_name;
    const AstNode *class_node;
    XgClassId class_id;
    uint32_t summary_index;
} XgClassNameRow;

typedef struct XgFuncNameRow {
    XgModuleId module_id;
    const char *name;
    const char *extern_dylib;
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

typedef struct XgEnumNameRow {
    XgModuleId module_id;
    const char *name;
    uint32_t type_key;
    const EnumDeclNode *decl;
} XgEnumNameRow;

typedef struct XgStdlibImportRow {
    XgModuleId module_id;
    const char *local_name;
    const char *module_name;
    const char *member_name;
} XgStdlibImportRow;

typedef struct XgLocalType XgLocalType;
typedef struct XgLocalName XgLocalName;

typedef struct XgPendingOpenObjectAccess {
    XgObjectAccessId access_id;
    XgFuncId owner_func_id;
    XgObjectShapeId constraint_shape_id;
    uint16_t param_ordinal;
} XgPendingOpenObjectAccess;

typedef struct XgProducer {
    XgGlobalEvidence *evidence;
    XgClassNameRow *classes;
    uint32_t nclasses;
    uint32_t class_cap;
    XgInterfaceNameRow *interfaces;
    uint32_t ninterfaces;
    uint32_t interface_cap;
    XgEnumNameRow *enums;
    uint32_t nenums;
    uint32_t enum_cap;
    XgFuncNameRow *funcs;
    uint32_t nfuncs;
    uint32_t func_cap;
    XgStdlibImportRow *stdlib_imports;
    uint32_t nstdlib_imports;
    uint32_t stdlib_import_cap;
    struct XgPendingBody *bodies;
    uint32_t nbodies;
    uint32_t body_cap;
    XgPendingOpenObjectAccess *open_object_accesses;
    uint32_t nopen_object_accesses;
    uint32_t open_object_access_cap;
    XgFuncId next_func_id;
    XaAnalyzer *analyzer;
} XgProducer;

typedef struct XgPendingBody {
    XgFuncId func_id;
    XgFuncId lexical_parent_func_id;
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
    XaSymbolLinks *links;
    XgLocalType *captured_locals;
    uint32_t captured_local_count;
    XgLocalName *captured_name_locals;
    uint32_t captured_name_local_count;
} XgPendingBody;

static const XgPendingBody *producer_find_child_function_body(const XgProducer *producer,
                                                              XgFuncId parent_func_id,
                                                              uint32_t name_id) {
    const XgPendingBody *match = NULL;
    if (!producer || parent_func_id == XG_NO_ID || name_id == 0)
        return NULL;
    for (uint32_t i = 0; i < producer->nbodies; i++) {
        const XgPendingBody *pending = &producer->bodies[i];
        if (pending->lexical_parent_func_id != parent_func_id || pending->name_id != name_id ||
            pending->kind != XG_BODY_FUNCTION)
            continue;
        if (match)
            return NULL;
        match = pending;
    }
    return match;
}

struct XgLocalType {
    const char *name;
    const char *nominal_name;
    XgClassId class_id;
    XgInterfaceId interface_id;
    uint32_t type_key;
    XgObjectShapeId object_shape_id;
    const ObjectLiteralNode *object_shape_literal;
    XgMapShapeId map_shape_id;
    uint8_t map_container_kind;
    uint32_t map_receiver_type_key;
    uint32_t map_key_type_key;
    uint32_t map_value_type_key;
    uint8_t sequence_kind;
    uint32_t sequence_elem_type_key;
    XgInterfaceId sequence_elem_interface_id;
    /* R2-3: element CLASS of Array<C>/sequence locals. Lets for-in items and
     * a[i] receivers resolve their static class so polymorphic method calls
     * get a dispatch plan instead of silently falling back to a static
     * direct bind in the AOT backend. */
    XgClassId sequence_elem_class_id;
    uint32_t sequence_storage_id;
    bool sequence_elem_managed_ref;
    XgObjectShapeId sequence_elem_object_shape_id;
    const ObjectLiteralNode *sequence_elem_object_shape_literal;
    uint8_t sequence_elem_map_container_kind;
    uint32_t sequence_elem_map_key_type_key;
    uint32_t sequence_elem_map_value_type_key;
    bool sequence_fresh_empty;
    uint16_t param_ordinal;
    uint8_t object_row_mode;
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
    uint8_t body_kind;
    XgLocalType *locals;
    uint32_t nlocals;
    uint32_t local_cap;
    XgLocalName *name_locals;
    uint32_t nname_locals;
    uint32_t name_local_cap;
    uint32_t inherited_name_local_count;
    uint32_t callsite_start;
    uint32_t callsite_count;
    uint32_t key_access_count;
    uint32_t interface_object_use_count;
    uint32_t sequence_access_count;
    uint32_t capacity_op_count;
    uint32_t bulk_op_count;
    uint32_t encoding_op_count;
    const AstNode *counted_loop_body;
    const AstNode *counted_loop_count_expr;
    uint32_t counted_loop_id;
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

static uint32_t hash_param_storage_requirements32(const XaSymbolLinks *links) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    bool has_requirement = false;
    if (!links || !links->param_effects || links->param_effect_count <= 0)
        return 0;
    h = fold_u64(h, (uint64_t) links->param_effect_count);
    for (int i = 0; i < links->param_effect_count; i++) {
        uint8_t owner = links->param_effects[i].storage_domain;
        if (owner != XR_STORAGE_DOMAIN_UNKNOWN)
            has_requirement = true;
        h = fold_u64(h, (uint64_t) (uint32_t) i);
        h = fold_u64(h, (uint64_t) owner);
    }
    return has_requirement ? hash_folded32(h) : 0;
}

static bool add_param_storage_summaries(XgProducer *producer, const XaSymbolLinks *links,
                                        XgFuncId owner_func_id, uint32_t *out_start,
                                        uint32_t *out_count) {
    bool has_requirement = false;
    uint32_t start = 0;
    uint32_t count = 0;
    if (out_start)
        *out_start = 0;
    if (out_count)
        *out_count = 0;
    if (!producer || !producer->evidence || owner_func_id == XG_NO_ID)
        return false;
    if (!links || !links->param_effects || links->param_effect_count <= 0)
        return true;
    for (int i = 0; i < links->param_effect_count; i++) {
        if (links->param_effects[i].storage_domain != XR_STORAGE_DOMAIN_UNKNOWN) {
            has_requirement = true;
            break;
        }
    }
    if (!has_requirement)
        return true;
    start = producer->evidence->nparam_storages + 1;
    count = (uint32_t) links->param_effect_count;
    for (uint32_t i = 0; i < count; i++) {
        XgParamStorageSummary row;
        memset(&row, 0, sizeof(row));
        row.requirement_id = start + i;
        row.owner_func_id = owner_func_id;
        row.param_index = i;
        row.storage_domain = links->param_effects[i].storage_domain;
        if (!xg_global_evidence_add_param_storage(producer->evidence, &row))
            return false;
    }
    if (out_start)
        *out_start = start;
    if (out_count)
        *out_count = count;
    return true;
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

/* Republish the analyzer's return-ownership conclusion into the evidence.
 *
 * Only a `complete` conclusion crosses: an incomplete one carries no fact, and
 * publishing it as anything but UNKNOWN would let a consumer read a guess as a
 * proof. NULL_JOIN is analyzer-internal -- it means every return was a null
 * literal -- and never reaches a caller as an ownership answer. */
static XgReturnOwnership producer_return_ownership(const XaSymbolLinks *links) {
    XgReturnOwnership out = {XG_RETURN_OWNERSHIP_UNKNOWN, -1, 0};
    if (!links || !links->return_ownership.complete)
        return out;
    switch ((XaReturnOwnershipKind) links->return_ownership.kind) {
        case XA_RETURN_OWNERSHIP_OWNED:
            out.kind = XG_RETURN_OWNERSHIP_OWNED;
            break;
        case XA_RETURN_OWNERSHIP_BORROWED_PARAM:
            out.kind = XG_RETURN_OWNERSHIP_BORROWED_PARAM;
            break;
        case XA_RETURN_OWNERSHIP_BORROWED_STATIC:
            out.kind = XG_RETURN_OWNERSHIP_BORROWED_STATIC;
            break;
        /* No default: a new analyzer kind must fail the -Wswitch build here
         * rather than be silently republished as UNKNOWN. */
        case XA_RETURN_OWNERSHIP_UNKNOWN:
            return out;
    }
    out.param_index = links->return_ownership.param_index;
    out.complete = 1;
    return out;
}

static XaSymbolLinks *producer_function_links(const XgProducer *p, const FunctionDeclNode *fn) {
    XaSymbol *sym = NULL;
    if (!p || !p->analyzer || !fn)
        return NULL;
    if (fn->symbol_id && p->analyzer->global_scope)
        sym = xa_scope_lookup_by_id(p->analyzer->global_scope, fn->symbol_id);
    if (!sym && fn->name)
        sym = xa_scope_lookup(p->analyzer->global_scope, fn->name);
    return sym && sym->kind == XA_SYM_FUNCTION ? xa_analyzer_get_links(p->analyzer, sym) : NULL;
}

static XaSymbolLinks *producer_class_links(const XgProducer *p, const ClassDeclNode *cls) {
    XaSymbol *sym = NULL;
    if (!p || !p->analyzer || !cls)
        return NULL;
    if (cls->symbol_id && p->analyzer->global_scope)
        sym = xa_scope_lookup_by_id(p->analyzer->global_scope, cls->symbol_id);
    if (!sym && cls->name)
        sym = xa_scope_lookup(p->analyzer->global_scope, cls->name);
    return sym && sym->kind == XA_SYM_CLASS ? xa_analyzer_get_links(p->analyzer, sym) : NULL;
}

static XaSymbolLinks *producer_method_links(const XgProducer *p, XrClassInfo *info,
                                            const MethodDeclNode *method) {
    XaSymbol *sym;
    if (!p || !p->analyzer || !info || !method || !method->name)
        return NULL;
    sym = xa_class_info_lookup_member(info, method->name);
    return sym && sym->kind == XA_SYM_METHOD ? xa_analyzer_get_links(p->analyzer, sym) : NULL;
}

static bool producer_decl_source_seen(const XgProducer *p, XgModuleId module_id,
                                      uint32_t source_node_id) {
    if (!p || !p->evidence || module_id == XG_NO_ID || source_node_id == 0)
        return false;
    for (uint32_t i = 0; i < p->evidence->ndecls; i++) {
        const XgDeclSummary *decl = &p->evidence->decls[i];
        if (decl->module_id == module_id && decl->source_node_id == source_node_id)
            return true;
    }
    return false;
}

static uint32_t producer_unique_decl_source_node_id(const XgProducer *p, XgModuleId module_id,
                                                    uint32_t source_node_id, uint32_t kind,
                                                    uint32_t name_id, uint32_t signature_key) {
    uint32_t candidate = source_node_id;
    uint32_t salt = 1;
    if (!p || !p->evidence || module_id == XG_NO_ID || source_node_id == 0)
        return source_node_id;
    while (producer_decl_source_seen(p, module_id, candidate)) {
        uint64_t h = XR_FNV64_OFFSET_BASIS;
        h = fold_u64(h, module_id);
        h = fold_u64(h, source_node_id);
        h = fold_u64(h, kind);
        h = fold_u64(h, name_id);
        h = fold_u64(h, signature_key);
        h = fold_u64(h, p->evidence->ndecls);
        h = fold_u64(h, salt++);
        candidate = hash_folded32(h);
    }
    return candidate;
}

static XgModuleId producer_class_module_id(const XgProducer *p, XgClassId class_id) {
    if (!p || !p->evidence || class_id == XG_NO_ID)
        return XG_NO_ID;
    for (uint32_t i = 0; i < p->evidence->nclasses; i++) {
        const XgClassSummary *cls = &p->evidence->classes[i];
        if (cls->class_id == class_id)
            return cls->module_id;
    }
    return XG_NO_ID;
}

static bool producer_method_source_seen(const XgProducer *p, XgModuleId module_id,
                                        XgClassId current_class_id, uint32_t source_node_id) {
    if (!p || !p->evidence || module_id == XG_NO_ID || source_node_id == 0)
        return false;
    for (uint32_t i = 0; i < p->evidence->nmethods; i++) {
        const XgMethodSummary *method = &p->evidence->methods[i];
        XgModuleId owner_module = method->owner_class_id == current_class_id
                                      ? module_id
                                      : producer_class_module_id(p, method->owner_class_id);
        if (owner_module == module_id && method->source_node_id == source_node_id)
            return true;
    }
    return false;
}

static uint32_t producer_unique_method_source_node_id(const XgProducer *p, XgModuleId module_id,
                                                      XgClassId owner_class_id,
                                                      uint32_t source_node_id, uint32_t name_id,
                                                      uint32_t signature_key) {
    uint32_t candidate = source_node_id;
    uint32_t salt = 1;
    if (!p || !p->evidence || module_id == XG_NO_ID || source_node_id == 0)
        return source_node_id;
    while (producer_method_source_seen(p, module_id, owner_class_id, candidate)) {
        uint64_t h = XR_FNV64_OFFSET_BASIS;
        h = fold_u64(h, module_id);
        h = fold_u64(h, source_node_id);
        h = fold_u64(h, owner_class_id);
        h = fold_u64(h, name_id);
        h = fold_u64(h, signature_key);
        h = fold_u64(h, p->evidence->nmethods);
        h = fold_u64(h, salt++);
        candidate = hash_folded32(h);
    }
    return candidate;
}

static bool producer_class_field_source_seen(const XgProducer *p, XgModuleId module_id,
                                             uint32_t source_node_id) {
    if (!p || !p->evidence || module_id == XG_NO_ID || source_node_id == 0)
        return false;
    for (uint32_t i = 0; i < p->evidence->nclass_fields; i++) {
        const XgClassFieldSummary *field = &p->evidence->class_fields[i];
        if (field->module_id == module_id && field->source_node_id == source_node_id)
            return true;
    }
    return false;
}

static uint32_t
producer_unique_class_field_source_node_id(const XgProducer *p, XgModuleId module_id,
                                           uint32_t source_node_id, XgClassId owner_class_id,
                                           uint32_t name_id, uint32_t decl_ordinal) {
    uint32_t candidate = source_node_id;
    uint32_t salt = 1;
    if (!p || !p->evidence || module_id == XG_NO_ID || source_node_id == 0)
        return source_node_id;
    while (producer_class_field_source_seen(p, module_id, candidate)) {
        uint64_t h = XR_FNV64_OFFSET_BASIS;
        h = fold_u64(h, module_id);
        h = fold_u64(h, source_node_id);
        h = fold_u64(h, owner_class_id);
        h = fold_u64(h, name_id);
        h = fold_u64(h, decl_ordinal);
        h = fold_u64(h, salt++);
        candidate = hash_folded32(h);
    }
    return candidate;
}

static bool producer_stdlib_module_known(const char *name) {
    return xr_stdlib_metadata_link_dependency_module_known(name);
}

static bool producer_vm_control_module_known(const char *name) {
    return name && (strcmp(name, "runtime") == 0 || strcmp(name, "test_yield") == 0);
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
    if (t->kind == XR_TREF_INT || t->kind == XR_TREF_INT_WIDTH || t->kind == XR_TREF_FLOAT ||
        t->kind == XR_TREF_FLOAT_WIDTH) {
        /* Source spelling and the legacy default-vs-exact TRef shape are not
         * semantic identity. int/i64, float/f64 and byte/u8 therefore share a
         * TypeKey, while every distinct scalar representation remains unique. */
        h = fold_u64(h, UINT64_C(0x5343414c4152)); /* "SCALAR" */
        h = fold_u64(h, t->scalar_rep);
    } else {
        h = fold_u64(h, t->kind);
        h = fold_u64(h, 0);
    }
    h = fold_u64(h, t->fixed_length);
    h = fold_u64(h, (uint64_t) t->object_row_mode);
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
    if (t->field_readonly) {
        bool has_readonly_field = false;
        for (uint8_t i = 0; i < t->nchildren; i++)
            has_readonly_field = has_readonly_field || t->field_readonly[i];
        if (has_readonly_field) {
            h = fold_u64(h, UINT64_C(0x726561646f6e6c79)); /* "readonly" */
            for (uint8_t i = 0; i < t->nchildren; i++)
                h = fold_u64(h, t->field_readonly[i] ? 1 : 0);
        }
    }
    if (t->kind == XR_TREF_FUNCTION && t->function_param_modes && t->nchildren > 0) {
        bool has_explicit_mode = false;
        for (uint8_t i = 0; i + 1 < t->nchildren; i++)
            has_explicit_mode = has_explicit_mode || t->function_param_modes[i] != XR_PARAM_READ;
        if (has_explicit_mode) {
            h = fold_u64(h, UINT64_C(0x706172616d6d6f64)); /* "parammod" */
            for (uint8_t i = 0; i + 1 < t->nchildren; i++)
                h = fold_u64(h, t->function_param_modes[i]);
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
    if (kind == XR_TREF_INT || kind == XR_TREF_FLOAT) {
        h = fold_u64(h, UINT64_C(0x5343414c4152)); /* "SCALAR" */
        h = fold_u64(h, kind == XR_TREF_INT ? XR_NATIVE_I64 : XR_NATIVE_F64);
    } else {
        h = fold_u64(h, kind);
        h = fold_u64(h, 0);
    }
    h = fold_u64(h, 0);
    h = fold_u64(h, 0);
    if (name)
        h = fold_bytes(h, name, strlen(name));
    for (int i = 0; i < child_count; i++)
        h = hash_tref(h, children ? children[i] : NULL);
    return hash_folded32(h);
}

static uint32_t hash_synthetic_width_tref32(uint8_t kind, uint8_t scalar_rep) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    if (kind == XR_TREF_INT || kind == XR_TREF_INT_WIDTH || kind == XR_TREF_FLOAT ||
        kind == XR_TREF_FLOAT_WIDTH)
        h = fold_u64(h, UINT64_C(0x5343414c4152)); /* "SCALAR" */
    else
        h = fold_u64(h, kind);
    h = fold_u64(h, scalar_rep);
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

static uint8_t class_field_int_semantic_kind(uint8_t scalar_rep) {
    switch (scalar_rep) {
        case XR_NATIVE_I8:
            return XG_CLASS_FIELD_TYPE_I8;
        case XR_NATIVE_U8:
            return XG_CLASS_FIELD_TYPE_U8;
        case XR_NATIVE_I16:
            return XG_CLASS_FIELD_TYPE_I16;
        case XR_NATIVE_U16:
            return XG_CLASS_FIELD_TYPE_U16;
        case XR_NATIVE_I32:
            return XG_CLASS_FIELD_TYPE_I32;
        case XR_NATIVE_U32:
            return XG_CLASS_FIELD_TYPE_U32;
        case XR_NATIVE_U64:
            return XG_CLASS_FIELD_TYPE_U64;
        case XR_NATIVE_ISIZE:
            return XG_CLASS_FIELD_TYPE_ISIZE;
        case XR_NATIVE_USIZE:
            return XG_CLASS_FIELD_TYPE_USIZE;
        case XR_NATIVE_I64:
        default:
            return XG_CLASS_FIELD_TYPE_I64;
    }
}

static uint8_t class_field_named_semantic_kind(const char *name) {
    if (!name)
        return XG_CLASS_FIELD_TYPE_DYNAMIC;
    if (strcmp(name, "Array") == 0 || strcmp(name, "Slice") == 0)
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
        "Array", "Slice", "Map", "Set", "string", "String",
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

static bool tref_contains_error(const XrTypeRef *type, uint32_t depth) {
    if (!type || depth > 64)
        return false;
    if (type->kind == XR_TREF_ERROR)
        return true;
    for (int i = 0; i < type->nchildren; i++) {
        if (tref_contains_error(type->children ? type->children[i] : NULL, depth + 1))
            return true;
    }
    return false;
}

static bool class_field_fill_type_facts(XgClassFieldSummary *row, const XrTypeRef *type) {
    const XrTypeRef *target;
    if (!row)
        return false;
    row->semantic_kind = XG_CLASS_FIELD_TYPE_DYNAMIC;
    if (!type)
        return true;
    if (tref_contains_error(type, 0))
        return false;
    if (type->kind == XR_TREF_CONST) {
        bool ok = class_field_fill_type_facts(row, type->nchildren > 0 ? type->children[0] : NULL);
        row->flags |= XG_CLASS_FIELD_CONST;
        return ok;
    }
    row->scalar_rep = type->scalar_rep;
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
            row->semantic_kind = class_field_int_semantic_kind(type->scalar_rep);
            break;
        case XR_TREF_FLOAT:
            row->semantic_kind = XG_CLASS_FIELD_TYPE_F64;
            break;
        case XR_TREF_FLOAT_WIDTH:
            row->semantic_kind = type->scalar_rep == XR_NATIVE_F32 ? XG_CLASS_FIELD_TYPE_F32
                                                                   : XG_CLASS_FIELD_TYPE_F64;
            break;
        case XR_TREF_BOOL:
            row->semantic_kind = XG_CLASS_FIELD_TYPE_BOOL;
            break;
        case XR_TREF_RUNE:
            row->semantic_kind = XG_CLASS_FIELD_TYPE_RUNE;
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
        default:
            row->semantic_kind = XG_CLASS_FIELD_TYPE_DYNAMIC;
            break;
    }
    return true;
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
        case XG_CLASS_FIELD_TYPE_RUNE:
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

static uint32_t hash_method_signature_parts(XrParamNode **params, int param_count,
                                            XrTypeRef *return_type, bool is_static,
                                            bool is_constructor) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    h = fold_u64(h, (uint64_t) param_count);
    h = fold_u64(h, is_static ? 1 : 0);
    h = fold_u64(h, is_constructor ? 1 : 0);
    for (int i = 0; i < param_count; i++) {
        XrParamNode *param = params ? params[i] : NULL;
        XrParamMode mode = param ? param->passing_mode : XR_PARAM_READ;
        h = fold_u64(h, xr_param_mode_is_valid(mode) ? mode : XR_PARAM_READ);
        h = hash_tref(h, param ? param->type : NULL);
    }
    h = hash_tref(h, return_type);
    return (uint32_t) (h ^ (h >> 32));
}

static uint32_t hash_method_signature(const MethodDeclNode *m) {
    if (!m)
        return 0;
    return hash_method_signature_parts(m->params, m->param_count, m->return_type, m->is_static,
                                       m->is_constructor);
}

static uint32_t hash_interface_method_signature(const InterfaceMethodNode *m) {
    if (!m)
        return 0;
    return hash_method_signature_parts(m->params, m->param_count, m->return_type, false, false);
}

static uint32_t hash_function_signature(const FunctionDeclNode *f) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    if (!f)
        return 0;
    h = fold_u64(h, (uint64_t) f->param_count);
    for (int i = 0; i < f->param_count; i++) {
        XrParamNode *p = f->params ? f->params[i] : NULL;
        XrParamMode mode = p ? p->passing_mode : XR_PARAM_READ;
        h = fold_u64(h, xr_param_mode_is_valid(mode) ? mode : XR_PARAM_READ);
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

static bool producer_register_func(XgProducer *p, XgModuleId module_id, const char *name,
                                   const char *extern_dylib, XgFuncId func_id, XgDeclId decl_id,
                                   uint32_t decl_flags) {
    if (!name || func_id == XG_NO_ID)
        return true;
    if (!producer_reserve_funcs(p, p->nfuncs + 1))
        return false;
    p->funcs[p->nfuncs].module_id = module_id;
    p->funcs[p->nfuncs].name = name;
    p->funcs[p->nfuncs].extern_dylib = extern_dylib;
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

static bool producer_reserve_enums(XgProducer *p, uint32_t needed) {
    uint32_t new_cap;
    XgEnumNameRow *rows;
    if (p->enum_cap >= needed)
        return true;
    new_cap = p->enum_cap < 8 ? 8 : p->enum_cap;
    while (new_cap < needed)
        new_cap *= 2;
    rows = (XgEnumNameRow *) xr_realloc(p->enums, (size_t) new_cap * sizeof(*rows));
    if (!rows)
        return false;
    p->enums = rows;
    p->enum_cap = new_cap;
    return true;
}

static bool producer_register_enum(XgProducer *p, XgModuleId module_id, const char *name,
                                   const EnumDeclNode *decl, uint32_t type_key) {
    if (!name)
        return true;
    for (uint32_t i = 0; i < p->nenums; i++) {
        XgEnumNameRow *existing = &p->enums[i];
        if (existing->module_id == module_id && existing->name &&
            strcmp(existing->name, name) == 0) {
            existing->decl = decl;
            existing->type_key = type_key;
            return true;
        }
    }
    if (!producer_reserve_enums(p, p->nenums + 1))
        return false;
    p->enums[p->nenums].module_id = module_id;
    p->enums[p->nenums].name = name;
    p->enums[p->nenums].type_key = type_key;
    p->enums[p->nenums].decl = decl;
    p->nenums++;
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

static uint64_t stable_tref_key(const XrTypeRef *t) {
    char canonical[512];
    int length;
    if (!t)
        return 0;
    length = xr_tref_to_string_buf(t, canonical, (int) sizeof(canonical));
    if (length <= 0)
        return 0;
    uint64_t key = xr_hash_bytes64(canonical, (size_t) length);
    return key ? key : 1;
}

static bool producer_reserve_open_object_accesses(XgProducer *p, uint32_t needed) {
    uint32_t new_cap;
    XgPendingOpenObjectAccess *rows;
    if (!p || p->open_object_access_cap >= needed)
        return p != NULL;
    new_cap = p->open_object_access_cap < 8 ? 8 : p->open_object_access_cap;
    while (new_cap < needed)
        new_cap *= 2;
    rows = (XgPendingOpenObjectAccess *) xr_realloc(p->open_object_accesses,
                                                    (size_t) new_cap * sizeof(*rows));
    if (!rows)
        return false;
    p->open_object_accesses = rows;
    p->open_object_access_cap = new_cap;
    return true;
}

static bool producer_enqueue_body(XgProducer *p, XgFuncId func_id, XgModuleId module_id,
                                  XgDeclId owner_decl_id, XgClassId current_class_id,
                                  XgMethodId owner_method_id, uint32_t name_id,
                                  uint32_t signature_key, uint32_t source_node_id,
                                  uint32_t source_span_id, uint8_t kind, const AstNode *body,
                                  const MethodDeclNode *method, const FunctionDeclNode *function,
                                  XaSymbolLinks *links) {
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
    row->links = links;
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
        for (uint32_t i = 0; i < row->captured_local_count; i++) {
            XgLocalType *captured = &row->captured_locals[i];

            /* A closure boundary is an escape boundary for flow-sensitive Json
             * refinements.  Preserve the declared Json type, but force the
             * nested body to use dynamic Json access unless it proves a fresh
             * shape of its own.  Structural object shapes are type guarantees
             * and therefore remain valid across the same boundary. */
            if (captured->type_key == hash_named_type_key32("Json", NULL, 0)) {
                captured->object_shape_id = XG_NO_ID;
                captured->object_shape_literal = NULL;
            }
            if (captured->sequence_elem_type_key == hash_named_type_key32("Json", NULL, 0)) {
                captured->sequence_elem_object_shape_id = XG_NO_ID;
                captured->sequence_elem_object_shape_literal = NULL;
            }
        }
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
    xr_free(p->open_object_accesses);
    p->bodies = NULL;
    p->open_object_accesses = NULL;
    p->nbodies = 0;
    p->body_cap = 0;
    p->nopen_object_accesses = 0;
    p->open_object_access_cap = 0;
}

static bool producer_register_class(XgProducer *p, XgModuleId module_id, const char *name,
                                    const char *super_name, const AstNode *class_node,
                                    XgClassId class_id, uint32_t summary_index) {
    if (!producer_reserve_classes(p, p->nclasses + 1))
        return false;
    p->classes[p->nclasses].module_id = module_id;
    p->classes[p->nclasses].name = name;
    p->classes[p->nclasses].super_name = super_name;
    p->classes[p->nclasses].class_node = class_node;
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

static XgEnumNameRow *producer_lookup_enum_row_scoped(const XgProducer *p, XgModuleId module_id,
                                                      uint32_t name_id, bool allow_global_unique) {
    XgEnumNameRow *match = NULL;
    if (!p || name_id == 0)
        return NULL;
    for (uint32_t i = 0; i < p->nenums; i++) {
        XgEnumNameRow *row = &p->enums[i];
        if (row->module_id != module_id || hash_name32(row->name) != name_id)
            continue;
        if (match)
            return NULL;
        match = row;
    }
    if (match || !allow_global_unique)
        return match;
    for (uint32_t i = 0; i < p->nenums; i++) {
        XgEnumNameRow *row = &p->enums[i];
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
                            const char *nominal_name, bool inferred) {
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
    row->nominal_name = nominal_name;
    row->class_id = class_id;
    row->interface_id = interface_id;
    row->type_key = type_key;
    row->object_shape_id = XG_NO_ID;
    row->object_shape_literal = NULL;
    row->map_shape_id = XG_NO_ID;
    row->map_container_kind = 0;
    row->map_receiver_type_key = 0;
    row->map_key_type_key = 0;
    row->map_value_type_key = 0;
    row->sequence_kind = 0;
    row->sequence_elem_type_key = 0;
    row->sequence_elem_interface_id = XG_NO_ID;
    row->sequence_elem_class_id = XG_NO_ID;
    row->sequence_elem_managed_ref = false;
    row->sequence_elem_object_shape_id = XG_NO_ID;
    row->sequence_elem_object_shape_literal = NULL;
    row->sequence_elem_map_container_kind = 0;
    row->sequence_elem_map_key_type_key = 0;
    row->sequence_elem_map_value_type_key = 0;
    row->param_ordinal = UINT16_MAX;
    row->object_row_mode = XR_OBJECT_ROW_EXACT;
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

/* A binding inherited by a nested function is an upvalue, not a function-local
 * temporary.  Ownership still matters for an owned reference local, though:
 * rebinding Array/string/class/etc. can execute retain/release traffic. */
static bool body_has_owned_symbol_local(XgBodyCollect *bc, const char *name, uint32_t symbol_id) {
    if (!bc || !name)
        return false;
    for (uint32_t i = bc->nname_locals; i > bc->inherited_name_local_count; i--) {
        const XgLocalName *row = &bc->name_locals[i - 1];
        if (symbol_id != 0 && row->symbol_id != 0) {
            if (row->symbol_id == symbol_id)
                return true;
            continue;
        }
        if (row->name && strcmp(row->name, name) == 0)
            return true;
    }
    return false;
}

/* Only analyzer-proven, non-null scalar locals are safe to classify as a pure
 * register rebind.  Fail closed for unresolved types and every ownership-
 * carrying representation so function attributes never hide ARC effects. */
static bool body_owned_local_rebind_is_scalar(XgBodyCollect *bc, const char *name,
                                              uint32_t symbol_id) {
    XaSymbol *symbol;
    XrType *type;

    if (!body_has_owned_symbol_local(bc, name, symbol_id) || symbol_id == 0 || !bc->producer ||
        !bc->producer->analyzer)
        return false;
    symbol = xa_scope_lookup_by_id(bc->producer->analyzer->global_scope, symbol_id);
    type = symbol ? xa_analyzer_get_type(bc->producer->analyzer, symbol) : NULL;
    if (symbol && symbol->kind == XA_SYM_PARAMETER && symbol->passing_mode == XR_PARAM_REF)
        return false;
    if (!type || type->is_nullable)
        return false;
    switch (type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
            return true;
        default:
            return false;
    }
}

static void body_note_variable_read(XgBodyCollect *bc, const VariableNode *var) {
    XaSymbol *symbol = NULL;
    XaSymbolLinks *links = NULL;

    if (!bc || !var || !var->name)
        return;
    if (body_has_symbol_local(bc, var->name, var->symbol_id))
        return;
    if (bc->producer && bc->producer->analyzer && var->symbol_id != 0) {
        symbol = xa_scope_lookup_by_id(bc->producer->analyzer->global_scope, var->symbol_id);
        links = symbol ? xa_analyzer_get_links(bc->producer->analyzer, symbol) : NULL;
        if (symbol && symbol->is_const && !symbol->is_rebindable && links && links->has_ct_value)
            return;
    }
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
        (void) body_push_local(bc, name, symbol_id, class_id, interface_id, type_key, NULL, true);
        return;
    }
    if (!row->inferred)
        return;
    row->class_id = class_id;
    row->interface_id = interface_id;
    row->type_key = type_key;
    row->object_shape_id = XG_NO_ID;
    row->object_shape_literal = NULL;
    row->map_shape_id = XG_NO_ID;
    row->map_container_kind = 0;
    row->map_receiver_type_key = 0;
    row->map_key_type_key = 0;
    row->map_value_type_key = 0;
    row->sequence_elem_interface_id = XG_NO_ID;
    row->sequence_elem_class_id = XG_NO_ID;
    row->sequence_elem_object_shape_id = XG_NO_ID;
    row->sequence_elem_object_shape_literal = NULL;
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

static const XgClassFieldSummary *
body_find_class_field_in_hierarchy(XgBodyCollect *bc, XgClassId class_id, uint32_t field_name_id);
static const XgClassSummary *body_find_class_summary(XgBodyCollect *bc, XgClassId class_id);
static XgLocalType *body_lookup_local_sequence(XgBodyCollect *bc, const AstNode *expr);

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
        case AST_MEMBER_ACCESS: {
            XgClassId receiver_class = body_resolve_expr_class(bc, expr->as.member_access.object);
            const char *field_name = expr->as.member_access.name;
            const XgClassFieldSummary *field = body_find_class_field_in_hierarchy(
                bc, receiver_class, field_name ? hash_name32(field_name) : 0);
            return field ? field->target_class_id : XG_NO_ID;
        }
        case AST_INDEX_GET: {
            /* a[i] on a tracked Array<C> local: the element class (R2-3,
             * mirrors the interface resolution below). */
            XgLocalType *local = body_lookup_local_sequence(bc, expr->as.index_get.array);
            return local ? local->sequence_elem_class_id : XG_NO_ID;
        }
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
        case AST_MEMBER_ACCESS: {
            XgClassId receiver_class = body_resolve_expr_class(bc, expr->as.member_access.object);
            const char *field_name = expr->as.member_access.name;
            const XgClassFieldSummary *field = body_find_class_field_in_hierarchy(
                bc, receiver_class, field_name ? hash_name32(field_name) : 0);
            return field ? field->target_interface_id : XG_NO_ID;
        }
        case AST_INDEX_GET: {
            XgLocalType *local = body_lookup_local_sequence(bc, expr->as.index_get.array);
            return local ? local->sequence_elem_interface_id : XG_NO_ID;
        }
        case AST_GROUPING:
            return body_resolve_expr_interface(bc, expr->as.grouping);
        default:
            return XG_NO_ID;
    }
}

static uint32_t body_resolve_expr_nominal_name_id(XgBodyCollect *bc, const AstNode *expr) {
    const XgClassSummary *cls = body_find_class_summary(bc, body_resolve_expr_class(bc, expr));
    if (cls && cls->name_id != 0)
        return cls->name_id;
    if (!bc || !expr)
        return 0;
    switch (expr->type) {
        case AST_VARIABLE: {
            XgLocalType *local = body_find_local(bc, expr->as.variable.name);
            return local && local->nominal_name ? hash_name32(local->nominal_name) : 0;
        }
        case AST_MEMBER_ACCESS: {
            XgClassId receiver_class = body_resolve_expr_class(bc, expr->as.member_access.object);
            const char *field_name = expr->as.member_access.name;
            const XgClassFieldSummary *field = body_find_class_field_in_hierarchy(
                bc, receiver_class, field_name ? hash_name32(field_name) : 0);
            return field ? field->target_name_id : 0;
        }
        case AST_NEW_EXPR:
            return expr->as.new_expr.class_name ? hash_name32(expr->as.new_expr.class_name) : 0;
        case AST_CALL_EXPR:
            return expr->as.call_expr.callee && expr->as.call_expr.callee->type == AST_VARIABLE
                       ? hash_name32(expr->as.call_expr.callee->as.variable.name)
                       : 0;
        case AST_AS_EXPR:
            return expr->as.as_expr.type && expr->as.as_expr.type->name
                       ? hash_name32(expr->as.as_expr.type->name)
                       : 0;
        case AST_GROUPING:
            return body_resolve_expr_nominal_name_id(bc, expr->as.grouping);
        default:
            return 0;
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

static bool body_is_compiler_owned_native_member(uint32_t type_name_id, const char *member_name,
                                                 bool member_is_static) {
    if (type_name_id == 0 || !member_name)
        return false;
#define XA_NATIVE_MEMBER_CONTRACT(type, member, is_static, allocation, effect, errors_csv)         \
    if (member_is_static == (is_static) && type_name_id == hash_name32(type) &&                    \
        strcmp(member_name, member) == 0)                                                          \
        return true;
#include "../frontend/analyzer/xa_native_member_contract.def"
#undef XA_NATIVE_MEMBER_CONTRACT
    return false;
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
    uint32_t instance_ordinal = 0;
    h = fold_u64(h, type_key);
    h = fold_u64(h, derive_kind);
    if (!cls || cls->field_count <= 0 || !cls->fields)
        return h ? h : 1;
    for (int i = 0; i < cls->field_count; i++) {
        const AstNode *field_node = cls->fields[i];
        const FieldDeclNode *field =
            field_node && field_node->type == AST_FIELD_DECL ? &field_node->as.field_decl : NULL;
        if (!field || field->is_static)
            continue;
        h = fold_u64(h, instance_ordinal++);
        h = fold_u64(h, field ? hash_name32(field->name) : 0);
        h = fold_u64(h, field ? hash_tref32(field->field_type) : 0);
        h = fold_u64(h, derived_field_flags(field));
    }
    h = fold_u64(h, instance_ordinal);
    return h ? h : 1;
}

static const XgClassFieldSummary *
producer_find_class_field(const XgProducer *p, XgClassId owner_class_id, uint32_t decl_ordinal) {
    if (!p || !p->evidence || owner_class_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < p->evidence->nclass_fields; i++) {
        const XgClassFieldSummary *field = &p->evidence->class_fields[i];
        if (field->owner_class_id == owner_class_id && field->decl_ordinal == decl_ordinal)
            return field;
    }
    return NULL;
}

static uint16_t producer_derive_instance_field_count(const ClassDeclNode *cls) {
    uint32_t count = 0;
    if (!cls || cls->field_count <= 0 || !cls->fields)
        return 0;
    for (int i = 0; i < cls->field_count; i++) {
        const AstNode *field_node = cls->fields[i];
        const FieldDeclNode *field =
            field_node && field_node->type == AST_FIELD_DECL ? &field_node->as.field_decl : NULL;
        if (field && !field->is_static && count < UINT16_MAX)
            count++;
    }
    return (uint16_t) count;
}

static bool producer_add_derive_fields(XgProducer *p, XgDeriveId derive_id,
                                       const ClassDeclNode *cls, XgClassId owner_class_id) {
    uint16_t instance_ordinal = 0;
    if (!p || !p->evidence || !cls || cls->field_count <= 0)
        return true;
    for (int i = 0; i < cls->field_count; i++) {
        const AstNode *field_node = cls->fields ? cls->fields[i] : NULL;
        const FieldDeclNode *field =
            field_node && field_node->type == AST_FIELD_DECL ? &field_node->as.field_decl : NULL;
        const XgClassFieldSummary *source_field;
        XgDerivedFieldSummary row;
        if (!field || field->is_static)
            continue;
        source_field = producer_find_class_field(p, owner_class_id, (uint32_t) i);
        memset(&row, 0, sizeof(row));
        row.field_id = (XgDerivedFieldId) (p->evidence->nderived_fields + 1);
        row.derive_id = derive_id;
        row.field_ordinal = instance_ordinal++;
        row.name_id = field ? hash_name32(field->name) : 0;
        row.type_key = field ? hash_tref32(field->field_type) : 0;
        row.source_field_id = source_field ? source_field->field_id : XG_NO_ID;
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
        uint16_t field_count;
        if ((derive_flags & flag) == 0 || !derive_kind_from_flag(flag, &kind))
            continue;
        field_count = producer_derive_instance_field_count(cls);
        derive_id = (XgDeriveId) (p->evidence->nderives + 1);
        memset(&row, 0, sizeof(row));
        row.derive_id = derive_id;
        row.module_id = module_id;
        row.owner_decl_id = owner_decl_id;
        row.source_span_id = source_span_id;
        row.type_key = type_key;
        row.derive_kind = kind;
        row.field_start = field_count > 0 ? p->evidence->nderived_fields + 1 : 0;
        row.field_count = field_count;
        row.flags = XG_DERIVE_OPT_IN;
        row.derive_hash = hash_derive_decl(type_key, kind, cls);
        if (!xg_global_evidence_add_derive(p->evidence, &row))
            return false;
        if (!producer_add_derive_fields(p, derive_id, cls, owner_class_id))
            return false;
    }
    return true;
}

static bool body_type_key_has_derive_kind(XgBodyCollect *bc, uint32_t type_key,
                                          uint8_t derive_kind) {
    if (!bc || !bc->evidence || type_key == 0)
        return false;
    for (uint32_t i = 0; i < bc->evidence->nderives; i++) {
        const XgDeriveSummary *derive = &bc->evidence->derives[i];
        if (derive->type_key == type_key && derive->derive_kind == derive_kind)
            return true;
    }
    return false;
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

static bool body_call_uses_coro_runtime(XgBodyCollect *bc, const CallExprNode *call) {
    if (!bc || !call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    const MemberAccessNode *member = &call->callee->as.member_access;
    return body_member_receiver_is_module(member, "Coro") && !body_has_name_local(bc, "Coro");
}

static bool body_call_is_coro_local_new(XgBodyCollect *bc, const CallExprNode *call) {
    if (!body_call_uses_coro_runtime(bc, call))
        return false;
    const MemberAccessNode *member = &call->callee->as.member_access;
    return member->name && strcmp(member->name, "Local") == 0;
}

static bool body_call_is_coro_local_set(XgBodyCollect *bc, const CallExprNode *call) {
    if (!bc || !call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    const MemberAccessNode *member = &call->callee->as.member_access;
    return member->name && strcmp(member->name, "set") == 0 &&
           body_resolve_expr_nominal_name_id(bc, member->object) == hash_name32("CoroLocal");
}

static bool body_call_is_coro_pool_submit(XgBodyCollect *bc, const CallExprNode *call) {
    if (!bc || !call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    const MemberAccessNode *member = &call->callee->as.member_access;
    return member->name && strcmp(member->name, "submit") == 0 &&
           body_member_receiver_is_module(member, "CoroPool") &&
           !body_has_name_local(bc, "CoroPool");
}

static uint32_t body_capabilities_for_builtin_member_constructor(const MemberAccessNode *member) {
    if (!body_member_receiver_is_module(member, "sync"))
        return 0;
    return body_capabilities_for_builtin_constructor(member->name);
}

static bool body_global_builtin_call_is_leaf_intrinsic(const char *name, int arg_count) {
    if (!name)
        return false;
    if (arg_count == 1 &&
        (strcmp(name, "int") == 0 || strcmp(name, "float") == 0 || strcmp(name, "bool") == 0 ||
         strcmp(name, "rune") == 0 || strcmp(name, "string") == 0 || strcmp(name, "typeOf") == 0 ||
         strcmp(name, "typeName") == 0))
        return true;
    return false;
}

static bool body_builtin_receiver_pod_span_elem(const XrType *type) {
    if (!type || type->is_nullable)
        return false;
    switch (type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
            return true;
        default:
            return false;
    }
}

static bool body_builtin_receiver_matches(const XrType *receiver, XaBuiltinReceiverKind kind) {
    switch (kind) {
        case XA_BUILTIN_RECEIVER_EXACT_INTEGER:
            return receiver && receiver->kind == XR_KIND_INT && !receiver->is_nullable;
        case XA_BUILTIN_RECEIVER_EXACT_UNSIGNED_INTEGER:
            return xr_type_is_exact_unsigned_integer(receiver);
        case XA_BUILTIN_RECEIVER_U8_ARRAY:
            return xr_type_is_u8_array(receiver);
        case XA_BUILTIN_RECEIVER_ARRAY:
            return receiver && XR_TYPE_IS_ARRAY(receiver);
        case XA_BUILTIN_RECEIVER_U8_SLICE:
            return xr_type_is_u8_slice(receiver);
        case XA_BUILTIN_RECEIVER_POD_SLICE:
            return receiver && XR_TYPE_IS_SLICE(receiver) && receiver->container.element_type &&
                   body_builtin_receiver_pod_span_elem(receiver->container.element_type);
    }
    return false;
}

/* Receiver intrinsics are sealed language operations.  Resolve them through
 * the canonical registry so the global effect summary agrees with analyzer
 * and Xi lowering without a second method-name whitelist. */
static const XaBuiltinReceiverMethodSpec *
body_builtin_receiver_method_spec(XgBodyCollect *bc, const MemberAccessNode *member,
                                  int arg_count) {
    XrType *receiver;

    if (!bc || !bc->producer || !bc->producer->analyzer || !member || !member->object ||
        !member->name)
        return NULL;
    receiver = xa_analyzer_get_node_type(bc->producer->analyzer, member->object);
    if (!receiver)
        return NULL;
    for (size_t i = 0; i < xa_builtin_receiver_method_count(); i++) {
        const XaBuiltinReceiverMethodSpec *spec = &xa_builtin_receiver_methods[i];
        if (strcmp(spec->source_name, member->name) != 0 ||
            !body_builtin_receiver_matches(receiver, spec->receiver) ||
            (arg_count >= 0 && (arg_count < spec->min_params ||
                                (!spec->is_variadic && arg_count > spec->param_count))))
            continue;
        return spec;
    }
    return NULL;
}

static bool body_member_access_is_scalar_builtin(XgBodyCollect *bc,
                                                 const MemberAccessNode *member) {
    const XaBuiltinReceiverMethodSpec *spec = body_builtin_receiver_method_spec(bc, member, -1);
    return spec && (spec->receiver == XA_BUILTIN_RECEIVER_EXACT_INTEGER ||
                    spec->receiver == XA_BUILTIN_RECEIVER_EXACT_UNSIGNED_INTEGER);
}

/* Array/fixed-array pointer projection is a compiler-lowered leaf, not a
 * native call. The sealed receiver registry owns both source spelling and
 * lowering identity; consume it here so whole-program effects agree with Xi's
 * direct ARRAY_DATA_PTR lowering. */
static bool body_call_is_array_data_ptr_leaf(XgBodyCollect *bc, const AstNode *node) {
    const CallExprNode *call;
    const MemberAccessNode *member;
    XrType *receiver;
    const XaBuiltinReceiverMethodSpec *ptr_spec;
    const XaBuiltinReceiverMethodSpec *mut_ptr_spec;

    if (!bc || !bc->producer || !bc->producer->analyzer || !node || node->type != AST_CALL_EXPR)
        return false;
    call = &node->as.call_expr;
    if (call->arg_count != 0 || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    member = &call->callee->as.member_access;
    if (!member->object || !member->name)
        return false;
    receiver = xa_analyzer_get_node_type(bc->producer->analyzer, member->object);
    if (!receiver || (receiver->kind != XR_KIND_ARRAY && receiver->kind != XR_KIND_FIXED_ARRAY))
        return false;
    ptr_spec = xa_builtin_receiver_method_by_id(XA_BUILTIN_RECEIVER_METHOD_ARRAY_PTR);
    mut_ptr_spec = xa_builtin_receiver_method_by_id(XA_BUILTIN_RECEIVER_METHOD_ARRAY_MUT_PTR);
    return (ptr_spec && strcmp(member->name, ptr_spec->source_name) == 0) ||
           (mut_ptr_spec && strcmp(member->name, mut_ptr_spec->source_name) == 0);
}

/* Stdlib script modules can call private native primitives as ordinary identifiers.
 * Those declarations are injected into the analyzer scope from core.def and therefore
 * have no AST declaration for the closed-world producer to register.  Preserve their
 * analyzer-owned identity here so an effectful native call cannot be mistaken for an
 * unresolved, effect-free function value. */
static bool body_variable_is_stdlib_native_function(XgBodyCollect *bc,
                                                    const VariableNode *variable) {
    XaSymbol *symbol;
    XaSymbolLinks *links;
    if (!bc || !bc->producer || !bc->producer->analyzer || !variable || variable->symbol_id == 0)
        return false;
    symbol = xa_scope_lookup_by_id(bc->producer->analyzer->global_scope, variable->symbol_id);
    if (!symbol || symbol->kind != XA_SYM_FUNCTION || !symbol->is_builtin)
        return false;
    links = xa_analyzer_get_links(bc->producer->analyzer, symbol);
    return links && links->function_decl_node == NULL && links->module_name &&
           links->module_name[0] != '\0' && links->import_member_name &&
           links->import_member_name[0] != '\0';
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

static const XrTypeRef *body_pending_return_type_ref(const XgPendingBody *body) {
    if (!body)
        return NULL;
    if (body->function)
        return body->function->return_type;
    if (body->method)
        return body->method->return_type;
    return NULL;
}

static const XgPendingBody *body_find_call_body(XgBodyCollect *bc, const CallExprNode *call) {
    const AstNode *callee;
    if (!bc || !call || !call->callee)
        return NULL;
    callee = call->callee;
    if (callee->type == AST_VARIABLE && callee->as.variable.name) {
        XgFuncNameRow *target = producer_lookup_func_row(bc->producer, callee->as.variable.name);
        return producer_find_function_body(bc->producer, target ? target->func_id : XG_NO_ID);
    }
    if (callee->type == AST_MEMBER_ACCESS && callee->as.member_access.name) {
        const MemberAccessNode *member = &callee->as.member_access;
        XgClassId receiver_class = body_resolve_expr_class(bc, member->object);
        XgMethodSummary *method = producer_find_method_by_name_in_hierarchy(
            bc->producer, receiver_class, hash_name32(member->name), false);
        return producer_find_method_body(bc->producer, method ? method->method_id : XG_NO_ID);
    }
    return NULL;
}

static const XrTypeRef *body_call_return_type_ref(XgBodyCollect *bc, const CallExprNode *call) {
    return body_pending_return_type_ref(body_find_call_body(bc, call));
}

static bool body_expr_enum_access_parts(XgBodyCollect *bc, const AstNode *expr,
                                        const char **out_enum_name, const char **out_member_name);

static const XrTypeRef *body_expr_type_ref(XgBodyCollect *bc, const AstNode *expr) {
    if (!bc || !expr)
        return NULL;
    switch (expr->type) {
        case AST_AS_EXPR:
            return expr->as.as_expr.type;
        case AST_GROUPING:
            return body_expr_type_ref(bc, expr->as.grouping);
        case AST_COMPTIME_EXPR:
            return body_expr_type_ref(bc, expr->as.comptime_expr.expr);
        case AST_MOVE_EXPR:
            return body_expr_type_ref(bc, expr->as.move_expr.expr);
        case AST_UNSAFE_EXPR:
            return body_expr_type_ref(bc, expr->as.unsafe_expr.operand);
        case AST_FORCE_UNWRAP:
            return body_expr_type_ref(bc, expr->as.unary.operand);
        case AST_CALL_EXPR:
            return body_call_return_type_ref(bc, &expr->as.call_expr);
        default:
            return NULL;
    }
}

static const ObjectLiteralNode *body_static_object_literal(const AstNode *node);
static uint32_t body_struct_object_type_key(const ObjectLiteralNode *obj);

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
        case AST_LITERAL_RUNE:
            return hash_synthetic_tref32(XR_TREF_RUNE, NULL, NULL, 0);
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
            return hash_synthetic_tref32(XR_TREF_BOOL, NULL, NULL, 0);
        case AST_LITERAL_NULL:
            return hash_synthetic_tref32(XR_TREF_NULL, NULL, NULL, 0);
        case AST_LITERAL_BIGINT:
            return hash_named_type_key32("BigInt", NULL, 0);
        case AST_LITERAL_REGEX:
            return hash_named_type_key32("Regex", NULL, 0);
        case AST_ENUM_ACCESS:
        case AST_MEMBER_ACCESS: {
            const char *enum_name = NULL;
            if (body_expr_enum_access_parts(bc, expr, &enum_name, NULL))
                return hash_named_type_key32(enum_name, NULL, 0);
            break;
        }
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
                if (member->name &&
                    (strcmp(member->name, "decode") == 0 || strcmp(member->name, "parse") == 0) &&
                    member->object && member->object->type == AST_VARIABLE &&
                    member->object->as.variable.name &&
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

static uint64_t body_expr_stable_type_key(const XgBodyCollect *bc, const AstNode *expr) {
    XrType *type;
    if (!bc || !bc->producer || !bc->producer->analyzer || !expr)
        return 0;
    type = xa_analyzer_get_node_type(bc->producer->analyzer, expr);
    return type ? xr_type_stable_key(type) : 0;
}

static bool body_finalize_object_shape_identity(XgGlobalEvidence *evidence,
                                                XgObjectShapeId shape_id);

static bool body_add_options_shape_fields(XgBodyCollect *bc, XgObjectShapeId shape_id,
                                          const CallExprNode *call, uint16_t field_count,
                                          bool supplied_only) {
    if (!bc || !bc->evidence || shape_id == XG_NO_ID)
        return false;
    for (uint16_t ordinal = 0; ordinal < field_count; ordinal++) {
        char name[32];
        XgObjectFieldSummary field;
        int written = snprintf(name, sizeof(name), "$arg%u", (unsigned) ordinal);
        if (written <= 0 || (size_t) written >= sizeof(name))
            return false;
        memset(&field, 0, sizeof(field));
        field.field_id = (XgObjectFieldId) (bc->evidence->nobject_fields + 1);
        field.shape_id = shape_id;
        field.field_ordinal = ordinal;
        field.name_id = hash_name32(name);
        field.stable_name_key = xg_object_stable_name_key(name);
        if (call && call->arguments && ordinal < (uint16_t) call->arg_count)
            field.type_key = body_expr_type_key(bc, call->arguments[ordinal]);
        if (call && call->arguments && ordinal < (uint16_t) call->arg_count)
            field.stable_type_key = body_expr_stable_type_key(bc, call->arguments[ordinal]);
        field.flags = XG_OBJECT_FIELD_STATIC_KEY |
                      (supplied_only ? XG_OBJECT_FIELD_REQUIRED : XG_OBJECT_FIELD_OPTIONAL);
        if (!xg_global_evidence_add_object_field(bc->evidence, &field))
            return false;
    }
    return body_finalize_object_shape_identity(bc->evidence, shape_id);
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

    XgObjectShapeId param_shape_id = (XgObjectShapeId) (bc->evidence->nobject_shapes + 1);
    XgObjectShapeId supplied_shape_id = (XgObjectShapeId) (bc->evidence->nobject_shapes + 2);
    uint32_t supplied_type_key =
        body_call_arg_type_key_start(bc, call->arguments, (int) supplied_count);
    XgObjectShapeSummary param_shape;
    XgObjectShapeSummary supplied_shape;
    XgOptionsBagSummary options;

    memset(&param_shape, 0, sizeof(param_shape));
    param_shape.object_shape_id = param_shape_id;
    param_shape.module_id = bc->module_id;
    param_shape.owner_func_id = bc->owner_func_id;
    param_shape.source_span_id = callsite->source_span_id;
    param_shape.type_key = body_options_shape_type_key("options:param", callsite,
                                                       callsite->arg_type_key_start, param_count);
    param_shape.field_name_start =
        body_options_count_mask_id(callsite->callsite_id, "options:param-fields", 0, param_count);
    param_shape.field_count = param_count;
    param_shape.shape_kind = XG_OBJECT_SHAPE_OPTIONS;
    param_shape.domain = XG_OBJECT_DOMAIN_STRUCT;
    param_shape.provenance = XG_OBJECT_SHAPE_OPTIONS;
    param_shape.concrete_exact = 1;
    param_shape.flags =
        XG_OBJECT_SHAPE_SEALED | XG_OBJECT_SHAPE_STATIC_KEYS | XG_OBJECT_SHAPE_HAS_OPTIONS;
    param_shape.shape_hash =
        body_options_shape_hash("options:param", callsite, param_shape.type_key, param_count);
    param_shape.stable_type_key = param_shape.type_key;
    param_shape.stable_shape_key = param_shape.shape_hash;

    memset(&supplied_shape, 0, sizeof(supplied_shape));
    supplied_shape.object_shape_id = supplied_shape_id;
    supplied_shape.module_id = bc->module_id;
    supplied_shape.owner_func_id = bc->owner_func_id;
    supplied_shape.source_span_id = callsite->source_span_id;
    supplied_shape.type_key = body_options_shape_type_key("options:supplied", callsite,
                                                          supplied_type_key, supplied_count);
    supplied_shape.field_name_start = body_options_count_mask_id(
        callsite->callsite_id, "options:supplied-fields", 0, supplied_count);
    supplied_shape.field_count = supplied_count;
    supplied_shape.shape_kind = XG_OBJECT_SHAPE_LITERAL;
    supplied_shape.domain = XG_OBJECT_DOMAIN_STRUCT;
    supplied_shape.provenance = XG_OBJECT_SHAPE_LITERAL;
    supplied_shape.concrete_exact = 1;
    supplied_shape.fresh = 1;
    supplied_shape.flags =
        XG_OBJECT_SHAPE_SEALED | XG_OBJECT_SHAPE_STATIC_KEYS | XG_OBJECT_SHAPE_HAS_OPTIONS;
    supplied_shape.shape_hash = body_options_shape_hash("options:supplied", callsite,
                                                        supplied_shape.type_key, supplied_count);
    supplied_shape.stable_type_key = supplied_shape.type_key;
    supplied_shape.stable_shape_key = supplied_shape.shape_hash;

    if (!xg_global_evidence_add_object_shape(bc->evidence, &param_shape))
        return;
    if (!xg_global_evidence_add_object_shape(bc->evidence, &supplied_shape))
        return;
    if (!body_add_options_shape_fields(bc, param_shape_id, call, param_count, false) ||
        !body_add_options_shape_fields(bc, supplied_shape_id, call, supplied_count, true))
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
    return hash_synthetic_width_tref32(XR_TREF_INT_WIDTH, XR_NATIVE_U8);
}

static uint32_t body_rune_type_key(void) {
    return hash_synthetic_tref32(XR_TREF_RUNE, NULL, NULL, 0);
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
            *out_elem_type_key = body_rune_type_key();
        return true;
    }
    if (type->kind == XR_TREF_NAMED && type->name) {
        if (strcmp(type->name, "StringBuilder") == 0) {
            if (out_sequence_kind)
                *out_sequence_kind = XG_SEQ_STRING_BUILDER;
            if (out_elem_type_key)
                *out_elem_type_key = body_rune_type_key();
            return true;
        }
    }
    if ((type->kind == XR_TREF_GENERIC || type->kind == XR_TREF_NAMED) && type->name &&
        type->children && type->nchildren > 0) {
        if (strcmp(type->name, "Array") == 0) {
            uint32_t elem_key = hash_tref32(type->children[0]);
            if (out_sequence_kind)
                *out_sequence_kind =
                    elem_key == body_uint8_type_key() ? XG_SEQ_BYTES : XG_SEQ_ARRAY;
            if (out_elem_type_key)
                *out_elem_type_key = elem_key;
            return true;
        }
        if (strcmp(type->name, "Slice") == 0) {
            uint32_t elem_key = hash_tref32(type->children[0]);
            if (out_sequence_kind)
                *out_sequence_kind =
                    elem_key == body_uint8_type_key() ? XG_SEQ_BYTE_SLICE : XG_SEQ_SLICE;
            if (out_elem_type_key)
                *out_elem_type_key = elem_key;
            return true;
        }
    }
    return false;
}

static bool body_expr_is_string_builder_constructor(const AstNode *expr) {
    const AstNode *callee;
    if (!expr)
        return false;
    if (expr->type == AST_NEW_EXPR)
        return expr->as.new_expr.class_name &&
               strcmp(expr->as.new_expr.class_name, "StringBuilder") == 0;
    if (expr->type != AST_CALL_EXPR)
        return false;
    callee = expr->as.call_expr.callee;
    return callee && callee->type == AST_VARIABLE && callee->as.variable.name &&
           strcmp(callee->as.variable.name, "StringBuilder") == 0;
}

static const XrTypeRef *body_type_ref_sequence_elem_type_ref(const XrTypeRef *type) {
    if (!type || (type->kind != XR_TREF_GENERIC && type->kind != XR_TREF_NAMED) || !type->name ||
        !type->children || type->nchildren == 0)
        return NULL;
    if (strcmp(type->name, "Array") == 0 || strcmp(type->name, "Slice") == 0)
        return type->children[0];
    return NULL;
}

static bool body_type_ref_is_json_array(const XrTypeRef *type) {
    uint8_t sequence_kind = 0;
    const XrTypeRef *elem_type = body_type_ref_sequence_elem_type_ref(type);
    return body_type_ref_sequence_parts(type, &sequence_kind, NULL) &&
           sequence_kind == XG_SEQ_ARRAY && body_type_ref_is_json(elem_type);
}

static bool body_type_key_is_pod_array_lane(uint32_t type_key) {
    static const uint8_t int_widths[] = {
        XR_NATIVE_I64, XR_NATIVE_BOOL, XR_NATIVE_I8,  XR_NATIVE_I16,   XR_NATIVE_I32,  XR_NATIVE_U8,
        XR_NATIVE_U16, XR_NATIVE_U32,  XR_NATIVE_U64, XR_NATIVE_ISIZE, XR_NATIVE_USIZE};
    static const uint8_t float_widths[] = {XR_NATIVE_F64, XR_NATIVE_F32};
    if (type_key == hash_synthetic_tref32(XR_TREF_INT, NULL, NULL, 0) ||
        type_key == hash_synthetic_tref32(XR_TREF_FLOAT, NULL, NULL, 0) ||
        type_key == hash_synthetic_tref32(XR_TREF_BOOL, NULL, NULL, 0) ||
        type_key == hash_synthetic_tref32(XR_TREF_RUNE, NULL, NULL, 0))
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

static bool body_type_ref_contains_type_param(const XrTypeRef *type) {
    if (!type)
        return false;
    if (type->kind == XR_TREF_TYPE_PARAM || type->kind == XR_TREF_ERROR)
        return true;
    for (uint8_t i = 0; i < type->nchildren; i++) {
        if (body_type_ref_contains_type_param(type->children ? type->children[i] : NULL))
            return true;
    }
    return false;
}

static bool body_type_ref_is_managed_storage_ref(const XrTypeRef *type) {
    if (!type)
        return false;
    switch ((XrTypeRefKind) type->kind) {
        case XR_TREF_STRING:
        case XR_TREF_NAMED:
        case XR_TREF_GENERIC:
        case XR_TREF_FUNCTION:
        case XR_TREF_OBJECT:
            return true;
        case XR_TREF_OPTIONAL:
        case XR_TREF_UNION:
        case XR_TREF_TUPLE:
        case XR_TREF_FIXED_ARRAY:
            for (uint8_t i = 0; i < type->nchildren; i++) {
                if (body_type_ref_is_managed_storage_ref(type->children ? type->children[i] : NULL))
                    return true;
            }
            return false;
        default:
            return false;
    }
}

static uint64_t body_generic_storage_hash(const XgGenericStorageSummary *storage) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    if (!storage)
        return 0;
    h = fold_u64(h, storage->generic_inst_id);
    h = fold_u64(h, storage->module_id);
    h = fold_u64(h, storage->storage_kind);
    h = fold_u64(h, storage->origin_type_key);
    h = fold_u64(h, storage->specialized_type_key);
    h = fold_u64(h, storage->elem_type_key);
    h = fold_u64(h, storage->key_type_key);
    h = fold_u64(h, storage->value_type_key);
    h = fold_u64(h, storage->container_plan_id);
    h = fold_u64(h, storage->flags);
    return h ? h : 1;
}

static bool body_type_ref_map_parts(const XrTypeRef *type, uint8_t *out_container_kind,
                                    uint32_t *out_key_type_key, uint32_t *out_value_type_key);

static void body_add_generic_container_storage(XgBodyCollect *bc, const XrTypeRef *type,
                                               uint32_t source_span_id,
                                               uint32_t container_plan_id) {
    XrTypeRef *type_args[2] = {NULL, NULL};
    XgGenericInstSummary inst;
    XgGenericStorageSummary storage;
    const char *container_name;
    bool has_managed_ref = false;
    uint8_t storage_kind;
    uint8_t map_container_kind = 0;
    uint16_t type_arg_count;
    uint32_t elem_type_key = 0;
    uint32_t key_type_key = 0;
    uint32_t value_type_key = 0;
    uint32_t origin_type_key;
    uint32_t specialized_type_key;
    if (!bc || !bc->evidence || !type || type->kind != XR_TREF_GENERIC || !type->name ||
        !type->children || type->nchildren == 0 || body_type_ref_contains_type_param(type))
        return;

    if (strcmp(type->name, "Array") == 0 && type->nchildren >= 1) {
        container_name = "Array";
        storage_kind = XG_GENERIC_STORAGE_ARRAY;
        type_arg_count = 1;
        type_args[0] = type->children[0];
        elem_type_key = hash_tref32(type_args[0]);
    } else if (body_type_ref_map_parts(type, &map_container_kind, &key_type_key, &value_type_key)) {
        storage_kind = map_container_kind == XG_MAP_CONTAINER_SET ? XG_GENERIC_STORAGE_SET
                                                                  : XG_GENERIC_STORAGE_MAP;
        container_name = storage_kind == XG_GENERIC_STORAGE_SET ? "Set" : "Map";
        type_arg_count = storage_kind == XG_GENERIC_STORAGE_SET ? 1 : 2;
        type_args[0] = type->children[0];
        type_args[1] = type_arg_count == 2 ? type->children[1] : NULL;
        if (storage_kind == XG_GENERIC_STORAGE_SET)
            elem_type_key = key_type_key;
    } else {
        return;
    }

    origin_type_key = hash_named_type_key32(container_name, NULL, 0);
    specialized_type_key = hash_named_type_key32(container_name, type_args, type_arg_count);

    memset(&inst, 0, sizeof(inst));
    inst.generic_inst_id = (XgGenericInstId) (bc->evidence->ngeneric_insts + 1);
    inst.module_id = bc->module_id;
    inst.name_id = hash_name32(container_name);
    inst.type_key = hash_generic_inst_type_key(container_name, type_args, type_arg_count,
                                               XG_GENERIC_INST_CONTAINER);
    inst.type_arg_key_start = hash_tref_list32(type_args, type_arg_count);
    inst.type_arg_count = type_arg_count;
    inst.source_span_id = source_span_id;
    inst.kind = XG_GENERIC_INST_CONTAINER;
    inst.flags = XG_GENERIC_INST_CONCRETE_TYPES | XG_GENERIC_INST_CONCRETE_STORAGE;
    if (!xg_global_evidence_add_generic_inst(bc->evidence, &inst))
        return;

    memset(&storage, 0, sizeof(storage));
    storage.storage_id = (XgGenericStorageId) (bc->evidence->ngeneric_storages + 1);
    storage.generic_inst_id = inst.generic_inst_id;
    storage.module_id = bc->module_id;
    storage.storage_kind = storage_kind;
    storage.origin_type_key = origin_type_key;
    storage.specialized_type_key = specialized_type_key;
    storage.elem_type_key = elem_type_key;
    storage.key_type_key = key_type_key;
    storage.value_type_key = value_type_key;
    storage.container_plan_id = container_plan_id;
    if ((storage_kind == XG_GENERIC_STORAGE_ARRAY &&
         body_type_key_is_pod_array_lane(elem_type_key)) ||
        (storage_kind == XG_GENERIC_STORAGE_MAP && body_type_key_is_pod_array_lane(key_type_key) &&
         body_type_key_is_pod_array_lane(value_type_key)) ||
        (storage_kind == XG_GENERIC_STORAGE_SET && body_type_key_is_pod_array_lane(key_type_key))) {
        storage.flags = XG_GENERIC_STORAGE_TYPED_INLINE | XG_GENERIC_STORAGE_POD;
    } else {
        for (uint16_t i = 0; i < type_arg_count; i++) {
            if (body_type_ref_is_managed_storage_ref(type_args[i])) {
                has_managed_ref = true;
                break;
            }
        }
        if (has_managed_ref)
            storage.flags = XG_GENERIC_STORAGE_REF_LANE | XG_GENERIC_STORAGE_MANAGED_REF;
        else
            storage.flags = XG_GENERIC_STORAGE_BOXED;
    }
    storage.storage_hash = body_generic_storage_hash(&storage);
    (void) xg_global_evidence_add_generic_storage(bc->evidence, &storage);
}

static void body_link_generic_array_storage_plans(XgBodyCollect *bc, uint32_t generic_storage_start,
                                                  uint32_t sequence_access_start) {
    if (!bc || !bc->evidence)
        return;
    for (uint32_t i = generic_storage_start; i < bc->evidence->ngeneric_storages; i++) {
        XgGenericStorageSummary *storage = &bc->evidence->generic_storages[i];
        if (storage->storage_kind != XG_GENERIC_STORAGE_ARRAY ||
            storage->container_plan_id != XG_NO_ID)
            continue;
        for (uint32_t j = sequence_access_start; j < bc->evidence->nsequence_accesses; j++) {
            const XgSequenceAccessSummary *sequence = &bc->evidence->sequence_accesses[j];
            if (sequence->owner_func_id != bc->owner_func_id ||
                (sequence->sequence_kind != XG_SEQ_ARRAY &&
                 sequence->sequence_kind != XG_SEQ_BYTES) ||
                sequence->receiver_type_key != storage->specialized_type_key ||
                sequence->elem_type_key != storage->elem_type_key)
                continue;
            storage->container_plan_id = sequence->access_id;
            storage->storage_hash = body_generic_storage_hash(storage);
            break;
        }
    }
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

static bool body_object_literal_has_spread(const ObjectLiteralNode *obj) {
    if (!obj)
        return false;
    for (int i = 0; i < obj->count; i++) {
        if (body_object_literal_entry_is_spread(obj, i))
            return true;
    }
    return false;
}

static int body_object_literal_static_field_index(const ObjectLiteralNode *obj, const char *name) {
    int ordinal = 0;
    if (!obj || !name)
        return -1;
    for (int i = 0; i < obj->count; i++) {
        const char *key = body_object_literal_static_key(obj, i);
        if (key && strcmp(key, name) == 0)
            return ordinal;
        if (key)
            ordinal++;
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

static uint32_t body_json_static_field_count(const ObjectLiteralNode *obj) {
    uint32_t count = 0;
    if (!obj)
        return 0;
    for (int i = 0; i < obj->count; i++) {
        if (body_object_literal_static_key(obj, i))
            count++;
    }
    return count;
}

static uint64_t body_json_shape_hash(const ObjectLiteralNode *obj) {
    uint32_t count = body_json_static_field_count(obj);
    if (count > UINT16_MAX)
        count = UINT16_MAX;
    uint64_t h = xg_json_shape_hash_begin(count);
    uint32_t ordinal = 0;
    for (int i = 0; obj && i < obj->count; i++) {
        const char *key = body_object_literal_static_key(obj, i);
        if (key && ordinal < count) {
            h = xg_json_shape_hash_add_field(h, 0, hash_name32(key), 0);
            ordinal++;
        }
    }
    return h;
}

static const ObjectLiteralNode *body_static_json_array_element_literal(const AstNode *node) {
    const ArrayLiteralNode *array;
    const ObjectLiteralNode *first;
    uint64_t first_hash;
    if (!node)
        return NULL;
    while (node && node->type == AST_GROUPING)
        node = node->as.grouping;
    if (!node || node->type != AST_ARRAY_LITERAL)
        return NULL;
    array = &node->as.array_literal;
    if (array->is_repeat)
        return body_static_object_literal(array->repeat_value);
    if (array->count <= 0 || !array->elements)
        return NULL;
    first = body_static_object_literal(array->elements[0]);
    if (!first)
        return NULL;
    first_hash = body_json_shape_hash(first);
    for (int i = 1; i < array->count; i++) {
        const ObjectLiteralNode *next = body_static_object_literal(array->elements[i]);
        if (!next || next->count != first->count || body_json_shape_hash(next) != first_hash)
            return NULL;
    }
    return first;
}

static const XrTypeRef *body_type_alias_object_type_ref(const TypeAliasNode *alias) {
    if (!alias || !alias->resolved_type || alias->resolved_type->kind != XR_TREF_OBJECT)
        return NULL;
    return alias->resolved_type;
}

static int body_type_alias_object_field_count(const TypeAliasNode *alias) {
    const XrTypeRef *object_type;
    if (!alias)
        return 0;
    if (alias->field_count > 0 && alias->field_names)
        return alias->field_count;
    object_type = body_type_alias_object_type_ref(alias);
    if (!object_type || !object_type->field_names || !object_type->children)
        return 0;
    return (int) object_type->nchildren;
}

static const char *body_type_alias_object_field_name(const TypeAliasNode *alias, int index) {
    const XrTypeRef *object_type;
    if (!alias || index < 0)
        return NULL;
    if (alias->field_names && index < alias->field_count)
        return alias->field_names[index];
    object_type = body_type_alias_object_type_ref(alias);
    if (!object_type || !object_type->field_names || index >= (int) object_type->nchildren)
        return NULL;
    return object_type->field_names[index];
}

static const XrTypeRef *body_type_alias_object_field_type(const TypeAliasNode *alias, int index) {
    const XrTypeRef *object_type;
    if (!alias || index < 0)
        return NULL;
    if (alias->field_types && index < alias->field_count)
        return alias->field_types[index];
    object_type = body_type_alias_object_type_ref(alias);
    if (!object_type || !object_type->children || index >= (int) object_type->nchildren)
        return NULL;
    return object_type->children[index];
}

static bool body_type_alias_object_field_optional(const TypeAliasNode *alias, int index) {
    return alias && alias->field_optional && index >= 0 && index < alias->field_count &&
           alias->field_optional[index];
}

static bool body_type_alias_object_field_readonly(const TypeAliasNode *alias, int index) {
    const XrTypeRef *object_type = body_type_alias_object_type_ref(alias);
    return object_type && object_type->field_readonly && index >= 0 &&
           index < (int) object_type->nchildren && object_type->field_readonly[index];
}

static uint64_t body_type_alias_object_shape_hash(const TypeAliasNode *alias) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    int count = body_type_alias_object_field_count(alias);
    h = fold_u64(h, (uint64_t) count);
    for (int i = 0; i < count; i++) {
        const char *name = body_type_alias_object_field_name(alias, i);
        uint32_t name_id = name ? hash_name32(name) : 0;
        const XrTypeRef *field_type = body_type_alias_object_field_type(alias, i);
        uint32_t type_key = field_type ? hash_tref32(field_type) : 0;
        bool optional = body_type_alias_object_field_optional(alias, i);
        bool readonly = body_type_alias_object_field_readonly(alias, i);
        h = fold_u64(h, name_id);
        h = fold_u64(h, type_key);
        h = fold_u64(h, optional ? 1 : 0);
        h = fold_u64(h, readonly ? 1 : 0);
    }
    return h ? h : 1;
}

static uint32_t body_type_alias_field_name_start(const TypeAliasNode *alias) {
    return (uint32_t) (body_type_alias_object_shape_hash(alias) & UINT32_MAX);
}

static uint32_t body_type_alias_object_type_key(const TypeAliasNode *alias) {
    const XrTypeRef *object_type = body_type_alias_object_type_ref(alias);
    if (object_type)
        return hash_tref32(object_type);
    return alias && alias->name ? hash_named_type_key32(alias->name, NULL, 0) : 0;
}

static const XgObjectShapeSummary *
body_find_object_shape_for_type_key(XgBodyCollect *bc, uint32_t type_key, uint8_t shape_kind) {
    if (!bc || !bc->evidence || type_key == 0)
        return NULL;
    for (uint32_t i = 0; i < bc->evidence->nobject_shapes; i++) {
        const XgObjectShapeSummary *shape = &bc->evidence->object_shapes[i];
        if (shape->type_key == type_key && shape->shape_kind == shape_kind)
            return shape;
    }
    return NULL;
}

static uint32_t body_expr_type_key(XgBodyCollect *bc, const AstNode *expr);
static XgLocalType *body_lookup_local_sequence(XgBodyCollect *bc, const AstNode *expr);
static XgObjectShapeId body_lookup_object_shape(XgBodyCollect *bc, const AstNode *expr,
                                                const ObjectLiteralNode **out_literal);
static int body_object_shape_static_field_index(XgBodyCollect *bc, XgObjectShapeId shape_id,
                                                const ObjectLiteralNode *literal, const char *name);
static const XgClassFieldSummary *
body_find_class_field_in_hierarchy(XgBodyCollect *bc, XgClassId class_id, uint32_t field_name_id);

static XgObjectShapeId body_add_object_shape_for_literal_domain(XgBodyCollect *bc,
                                                                const ObjectLiteralNode *obj,
                                                                uint32_t source_span_id,
                                                                uint32_t type_key, uint8_t domain);

static XgObjectShapeId body_add_json_shape_for_literal(XgBodyCollect *bc,
                                                       const ObjectLiteralNode *obj,
                                                       uint32_t source_span_id, uint32_t type_key) {
    return body_add_object_shape_for_literal_domain(bc, obj, source_span_id, type_key,
                                                    XG_OBJECT_DOMAIN_JSON);
}

typedef struct XgReturnObjectLiteralLocal {
    const char *name;
    uint32_t symbol_id;
    const ObjectLiteralNode *literal;
} XgReturnObjectLiteralLocal;

typedef const ObjectLiteralNode *(*XgReturnLiteralResolver)(const AstNode *node);

typedef struct XgReturnObjectLiteralScan {
    XgReturnObjectLiteralLocal locals[64];
    XgReturnLiteralResolver resolver;
    uint32_t nlocals;
    uint32_t return_count;
    const ObjectLiteralNode *literal;
    uint64_t literal_shape_hash;
    int literal_field_count;
    bool seen;
    bool failed;
    bool terminated;
} XgReturnObjectLiteralScan;

static const ObjectLiteralNode *return_literal_static_literal(const XgReturnObjectLiteralScan *scan,
                                                              const AstNode *node) {
    if (!scan || !scan->resolver)
        return body_static_object_literal(node);
    return scan->resolver(node);
}

static int return_literal_local_index(const XgReturnObjectLiteralScan *scan, const char *name,
                                      uint32_t symbol_id) {
    if (!scan || !name)
        return -1;
    for (uint32_t i = scan->nlocals; i > 0; i--) {
        const XgReturnObjectLiteralLocal *local = &scan->locals[i - 1];
        if (symbol_id != 0 && local->symbol_id != 0) {
            if (local->symbol_id == symbol_id)
                return (int) (i - 1);
            continue;
        }
        if (local->name && strcmp(local->name, name) == 0)
            return (int) (i - 1);
    }
    return -1;
}

static bool body_add_object_access_with_case(XgBodyCollect *bc, XgObjectAccessSummary *access,
                                             const XgObjectShapeSummary *shape) {
    XgObjectAccessCaseSummary access_case;
    if (!bc || !bc->evidence || !access || !shape ||
        access->receiver_shape_id != shape->object_shape_id)
        return false;
    access->receiver_param_ordinal = UINT16_MAX;
    access->constraint_shape_id = shape->object_shape_id;
    access->receiver_shape_count = 1;
    access->receiver_shape_set_id = access->object_access_id;
    access->mutation_epoch = shape->mutation_epoch;
    if (!xg_global_evidence_add_object_access(bc->evidence, access))
        return false;
    memset(&access_case, 0, sizeof(access_case));
    access_case.case_id = (XgObjectAccessCaseId) (bc->evidence->nobject_access_cases + 1);
    access_case.object_access_id = access->object_access_id;
    access_case.receiver_shape_set_id = access->receiver_shape_set_id;
    access_case.receiver_shape_id = shape->object_shape_id;
    access_case.stable_shape_key = shape->stable_shape_key;
    access_case.mutation_epoch = shape->mutation_epoch;
    access_case.field_ordinal = access->field_ordinal;
    access_case.domain = shape->domain;
    if (!xg_global_evidence_add_object_access_case(bc->evidence, &access_case)) {
        bc->evidence->nobject_accesses--;
        return false;
    }
    return true;
}

static bool body_add_resolved_object_field_access(XgBodyCollect *bc, const AstNode *node,
                                                  bool mutating, XgObjectShapeId shape_id,
                                                  const char *name, uint16_t field_ordinal) {
    const XgObjectShapeSummary *shape;
    XgObjectAccessSummary row;

    if (!bc || !bc->evidence || !node || shape_id == XG_NO_ID || !name)
        return false;
    shape = xg_global_evidence_find_object_shape(bc->evidence, shape_id);
    if (!shape)
        return false;
    memset(&row, 0, sizeof(row));
    row.object_access_id = (XgObjectAccessId) (bc->evidence->nobject_accesses + 1);
    row.module_id = bc->module_id;
    row.owner_func_id = bc->owner_func_id;
    row.receiver_shape_id = shape_id;
    row.source_span_id = (uint32_t) node->line;
    row.field_name_id = hash_name32(name);
    row.field_ordinal = field_ordinal;
    row.access_kind = mutating ? XG_OBJECT_ACCESS_FIELD_SET : XG_OBJECT_ACCESS_FIELD_GET;
    row.domain = shape->domain;
    row.syntax = (node->type == AST_INDEX_GET || node->type == AST_INDEX_SET)
                     ? XG_OBJECT_ACCESS_SYNTAX_STATIC_INDEX
                     : XG_OBJECT_ACCESS_SYNTAX_DOT;
    row.flags = XG_OBJECT_ACCESS_STATIC_FIELD | XG_OBJECT_ACCESS_RECEIVER_SHAPE_PROVEN;
    if (mutating)
        row.flags |= XG_OBJECT_ACCESS_MUTATING;
    return body_add_object_access_with_case(bc, &row, shape);
}

static bool body_add_open_row_object_field_access(XgBodyCollect *bc, const AstNode *node,
                                                  bool mutating,
                                                  XgObjectShapeId constraint_shape_id,
                                                  const char *name, uint16_t constraint_ordinal,
                                                  uint16_t param_ordinal) {
    XgObjectAccessSummary row;
    XgPendingOpenObjectAccess *pending;
    const XgObjectShapeSummary *constraint;

    if (!bc || !bc->producer || !bc->evidence || !node || !name ||
        constraint_shape_id == XG_NO_ID || param_ordinal == UINT16_MAX)
        return false;
    constraint = xg_global_evidence_find_object_shape(bc->evidence, constraint_shape_id);
    if (!constraint || (constraint->flags & XG_OBJECT_SHAPE_OPEN_ROW) == 0)
        return false;
    memset(&row, 0, sizeof(row));
    row.object_access_id = (XgObjectAccessId) (bc->evidence->nobject_accesses + 1);
    row.module_id = bc->module_id;
    row.owner_func_id = bc->owner_func_id;
    row.receiver_shape_id = constraint_shape_id;
    row.constraint_shape_id = constraint_shape_id;
    row.source_span_id = (uint32_t) node->line;
    row.field_name_id = hash_name32(name);
    row.field_ordinal = constraint_ordinal;
    row.access_kind = mutating ? XG_OBJECT_ACCESS_FIELD_SET : XG_OBJECT_ACCESS_FIELD_GET;
    row.domain = XG_OBJECT_DOMAIN_STRUCT;
    row.syntax = (node->type == AST_INDEX_GET || node->type == AST_INDEX_SET)
                     ? XG_OBJECT_ACCESS_SYNTAX_STATIC_INDEX
                     : XG_OBJECT_ACCESS_SYNTAX_DOT;
    row.flags = XG_OBJECT_ACCESS_STATIC_FIELD | XG_OBJECT_ACCESS_OPEN_ROW;
    row.receiver_param_ordinal = param_ordinal;
    if (mutating)
        row.flags |= XG_OBJECT_ACCESS_MUTATING;
    if (!xg_global_evidence_add_object_access(bc->evidence, &row) ||
        !producer_reserve_open_object_accesses(bc->producer,
                                               bc->producer->nopen_object_accesses + 1)) {
        if (bc->evidence->nobject_accesses > 0 &&
            bc->evidence->object_accesses[bc->evidence->nobject_accesses - 1].object_access_id ==
                row.object_access_id)
            bc->evidence->nobject_accesses--;
        return false;
    }
    pending = &bc->producer->open_object_accesses[bc->producer->nopen_object_accesses++];
    pending->access_id = row.object_access_id;
    pending->owner_func_id = row.owner_func_id;
    pending->constraint_shape_id = constraint_shape_id;
    pending->param_ordinal = param_ordinal;
    return true;
}

static bool return_literal_push_local(XgReturnObjectLiteralScan *scan, const char *name,
                                      uint32_t symbol_id, const ObjectLiteralNode *literal) {
    if (!scan || !name || !literal)
        return true;
    if (scan->nlocals >= (uint32_t) (sizeof(scan->locals) / sizeof(scan->locals[0]))) {
        scan->failed = true;
        return false;
    }
    scan->locals[scan->nlocals].name = name;
    scan->locals[scan->nlocals].symbol_id = symbol_id;
    scan->locals[scan->nlocals].literal = literal;
    scan->nlocals++;
    return true;
}

static bool return_literal_record(XgReturnObjectLiteralScan *scan,
                                  const ObjectLiteralNode *literal) {
    uint64_t shape_hash;
    if (!scan || !literal)
        return false;
    shape_hash = body_json_shape_hash(literal);
    if (scan->seen &&
        (scan->literal_field_count != literal->count || scan->literal_shape_hash != shape_hash)) {
        scan->failed = true;
        return false;
    }
    scan->literal = literal;
    scan->literal_shape_hash = shape_hash;
    scan->literal_field_count = literal->count;
    scan->seen = true;
    scan->return_count++;
    return true;
}

static bool return_literal_same_shape(const ObjectLiteralNode *left,
                                      const ObjectLiteralNode *right) {
    if (!left || !right)
        return left == right;
    return left->count == right->count && body_json_shape_hash(left) == body_json_shape_hash(right);
}

static bool return_literal_merge_branch_locals(XgReturnObjectLiteralScan *scan,
                                               const XgReturnObjectLiteralScan *then_scan,
                                               const XgReturnObjectLiteralScan *else_scan,
                                               uint32_t base_locals) {
    if (!scan || !then_scan || !else_scan)
        return false;
    for (uint32_t i = 0; i < base_locals; i++) {
        const ObjectLiteralNode *then_literal = then_scan->locals[i].literal;
        const ObjectLiteralNode *else_literal = else_scan->locals[i].literal;
        scan->locals[i].literal = return_literal_same_shape(then_literal, else_literal)
                                      ? (then_literal ? then_literal : else_literal)
                                      : NULL;
    }
    scan->nlocals = base_locals;
    return true;
}

static bool return_literal_record_branch_return(XgReturnObjectLiteralScan *scan,
                                                const XgReturnObjectLiteralScan *branch_scan,
                                                uint32_t base_returns) {
    if (!scan || !branch_scan)
        return false;
    if (branch_scan->return_count <= base_returns)
        return true;
    return return_literal_record(scan, branch_scan->literal);
}

static bool return_literal_scan_node(XgReturnObjectLiteralScan *scan, const AstNode *node);
static bool return_literal_resolve_return_value(XgReturnObjectLiteralScan *scan,
                                                const AstNode *value,
                                                const ObjectLiteralNode **out_literal);
static bool return_literal_resolve_match_value(XgReturnObjectLiteralScan *scan,
                                               const AstNode *value,
                                               const ObjectLiteralNode **out_literal);
static bool return_literal_scan_match_expr(XgReturnObjectLiteralScan *scan, const AstNode *value);

static bool return_literal_scan_node_list(XgReturnObjectLiteralScan *scan, AstNode *const *nodes,
                                          int count) {
    if (!scan || scan->failed)
        return false;
    if (scan->terminated)
        return true;
    for (int i = 0; i < count; i++) {
        if (!return_literal_scan_node(scan, nodes ? nodes[i] : NULL))
            return false;
        if (scan->terminated)
            break;
    }
    return true;
}

static bool return_literal_scan_assignment(XgReturnObjectLiteralScan *scan, const char *name,
                                           uint32_t symbol_id, const AstNode *value) {
    int local_index;
    const ObjectLiteralNode *literal;
    if (!scan)
        return false;
    local_index = return_literal_local_index(scan, name, symbol_id);
    if (local_index >= 0) {
        XgReturnObjectLiteralScan resolved_scan = *scan;
        if (return_literal_resolve_return_value(&resolved_scan, value, &literal) && literal) {
            *scan = resolved_scan;
            local_index = return_literal_local_index(scan, name, symbol_id);
            if (local_index < 0)
                return false;
            scan->locals[local_index].literal = literal;
            return true;
        }
        literal = return_literal_static_literal(scan, value);
        if (literal) {
            scan->locals[local_index].literal = literal;
            return true;
        }
        if (!return_literal_scan_node(scan, value))
            return false;
        scan->locals[local_index].literal = NULL;
        return true;
    }
    return return_literal_scan_node(scan, value);
}

static bool return_literal_scan_tracked_local_read(XgReturnObjectLiteralScan *scan,
                                                   const AstNode *node) {
    if (!scan || !node)
        return true;
    if (node->type == AST_VARIABLE && node->as.variable.name &&
        return_literal_local_index(scan, node->as.variable.name, node->as.variable.symbol_id) >=
            0) {
        scan->failed = true;
        return false;
    }
    return true;
}

static bool return_literal_resolve_return_value(XgReturnObjectLiteralScan *scan,
                                                const AstNode *value,
                                                const ObjectLiteralNode **out_literal) {
    const ObjectLiteralNode *literal;
    const ObjectLiteralNode *then_literal;
    const ObjectLiteralNode *else_literal;
    int local_index;
    if (!scan || !out_literal)
        return false;
    *out_literal = NULL;
    if (!value)
        return false;
    switch (value->type) {
        case AST_GROUPING:
            return return_literal_resolve_return_value(scan, value->as.grouping, out_literal);
        case AST_MOVE_EXPR:
            return return_literal_resolve_return_value(scan, value->as.move_expr.expr, out_literal);
        case AST_UNSAFE_EXPR:
            return return_literal_resolve_return_value(scan, value->as.unsafe_expr.operand,
                                                       out_literal);
        case AST_FORCE_UNWRAP:
            return return_literal_resolve_return_value(scan, value->as.unary.operand, out_literal);
        case AST_TERNARY:
            if (!return_literal_scan_node(scan, value->as.ternary.condition) || scan->failed)
                return false;
            if (!return_literal_resolve_return_value(scan, value->as.ternary.true_expr,
                                                     &then_literal) ||
                !then_literal)
                return false;
            if (!return_literal_resolve_return_value(scan, value->as.ternary.false_expr,
                                                     &else_literal) ||
                !else_literal)
                return false;
            if (!return_literal_same_shape(then_literal, else_literal)) {
                scan->failed = true;
                return false;
            }
            *out_literal = then_literal;
            return true;
        case AST_MATCH_EXPR:
            return return_literal_resolve_match_value(scan, value, out_literal);
        default:
            break;
    }
    literal = return_literal_static_literal(scan, value);
    if (literal) {
        *out_literal = literal;
        return true;
    }
    if (value->type != AST_VARIABLE || !value->as.variable.name)
        return false;
    local_index =
        return_literal_local_index(scan, value->as.variable.name, value->as.variable.symbol_id);
    if (local_index < 0 || !scan->locals[local_index].literal)
        return false;
    *out_literal = scan->locals[local_index].literal;
    return true;
}

static bool return_literal_resolve_match_value(XgReturnObjectLiteralScan *scan,
                                               const AstNode *value,
                                               const ObjectLiteralNode **out_literal) {
    uint32_t base_locals;
    XgReturnObjectLiteralScan base_scan;
    XgReturnObjectLiteralScan merged_scan;
    bool have_merged_scan = false;
    const ObjectLiteralNode *match_literal = NULL;
    if (!scan || !value || value->type != AST_MATCH_EXPR || !out_literal)
        return false;
    *out_literal = NULL;
    if (!return_literal_scan_node(scan, value->as.match_expr.expr) || scan->failed)
        return false;
    if (value->as.match_expr.arm_count <= 0 || !value->as.match_expr.arms)
        return false;
    base_locals = scan->nlocals;
    base_scan = *scan;
    for (int i = 0; i < value->as.match_expr.arm_count; i++) {
        AstNode *arm_node = value->as.match_expr.arms[i];
        if (!arm_node || arm_node->type != AST_MATCH_ARM)
            return false;
        MatchArmNode *arm = &arm_node->as.match_arm;
        XgReturnObjectLiteralScan arm_scan = base_scan;
        const ObjectLiteralNode *arm_literal = NULL;
        if (arm->guard && (!return_literal_scan_node(&arm_scan, arm->guard) || arm_scan.failed))
            return false;
        if (!return_literal_resolve_return_value(&arm_scan, arm->body, &arm_literal) ||
            !arm_literal)
            return false;
        if (match_literal && !return_literal_same_shape(match_literal, arm_literal)) {
            scan->failed = true;
            return false;
        }
        if (!match_literal)
            match_literal = arm_literal;
        if (!have_merged_scan) {
            merged_scan = arm_scan;
            have_merged_scan = true;
        } else {
            XgReturnObjectLiteralScan tmp_scan = base_scan;
            if (!return_literal_merge_branch_locals(&tmp_scan, &merged_scan, &arm_scan,
                                                    base_locals))
                return false;
            merged_scan = tmp_scan;
        }
    }
    if (!have_merged_scan || !match_literal)
        return false;
    *scan = merged_scan;
    *out_literal = match_literal;
    return true;
}

static bool return_literal_scan_match_expr(XgReturnObjectLiteralScan *scan, const AstNode *value) {
    uint32_t base_locals;
    uint32_t base_returns;
    XgReturnObjectLiteralScan base_scan;
    XgReturnObjectLiteralScan merged_fallthrough;
    bool have_fallthrough = false;
    if (!scan || !value || value->type != AST_MATCH_EXPR)
        return false;
    if (!return_literal_scan_node(scan, value->as.match_expr.expr) || scan->failed)
        return false;
    if (value->as.match_expr.arm_count <= 0 || !value->as.match_expr.arms)
        return false;
    base_locals = scan->nlocals;
    base_returns = scan->return_count;
    base_scan = *scan;
    for (int i = 0; i < value->as.match_expr.arm_count; i++) {
        AstNode *arm_node = value->as.match_expr.arms[i];
        if (!arm_node || arm_node->type != AST_MATCH_ARM)
            return false;
        MatchArmNode *arm = &arm_node->as.match_arm;
        XgReturnObjectLiteralScan arm_scan = base_scan;
        if (arm->guard && (!return_literal_scan_node(&arm_scan, arm->guard) || arm_scan.failed))
            return false;
        if (!return_literal_scan_node(&arm_scan, arm->body) || arm_scan.failed)
            return false;
        if (arm_scan.return_count > base_returns &&
            !return_literal_record_branch_return(scan, &arm_scan, base_returns))
            return false;
        if (!arm_scan.terminated) {
            if (!have_fallthrough) {
                merged_fallthrough = arm_scan;
                have_fallthrough = true;
            } else {
                XgReturnObjectLiteralScan tmp_scan = base_scan;
                if (!return_literal_merge_branch_locals(&tmp_scan, &merged_fallthrough, &arm_scan,
                                                        base_locals))
                    return false;
                merged_fallthrough = tmp_scan;
            }
        }
    }
    if (have_fallthrough) {
        for (uint32_t i = 0; i < base_locals; i++)
            scan->locals[i].literal = merged_fallthrough.locals[i].literal;
        scan->nlocals = base_locals;
        scan->terminated = false;
    } else {
        scan->nlocals = base_locals;
        scan->terminated = true;
    }
    return true;
}

static bool return_literal_scan_if_stmt(XgReturnObjectLiteralScan *scan, const IfStmtNode *stmt) {
    XgReturnObjectLiteralScan then_scan;
    XgReturnObjectLiteralScan else_scan;
    uint32_t base_locals;
    uint32_t base_returns;
    bool then_returned;
    bool else_returned;
    bool then_falls_through;
    bool else_falls_through;
    bool record_then_return;
    bool record_else_return;
    if (!scan || !stmt || !stmt->then_branch)
        return false;
    if (!return_literal_scan_node(scan, stmt->condition))
        return false;
    base_locals = scan->nlocals;
    base_returns = scan->return_count;
    then_scan = *scan;
    else_scan = *scan;
    if (!return_literal_scan_node(&then_scan, stmt->then_branch) || then_scan.failed)
        return false;
    if (stmt->else_branch &&
        (!return_literal_scan_node(&else_scan, stmt->else_branch) || else_scan.failed))
        return false;
    then_returned = then_scan.return_count > base_returns;
    else_returned = else_scan.return_count > base_returns;
    then_falls_through = !then_scan.terminated;
    else_falls_through = !else_scan.terminated;
    record_then_return = then_returned;
    record_else_return = else_returned;
    if (!then_returned && !else_returned) {
        scan->terminated = false;
        return return_literal_merge_branch_locals(scan, &then_scan, &else_scan, base_locals);
    }

    if (!then_falls_through && !else_falls_through) {
        scan->nlocals = base_locals;
        if (!return_literal_record_branch_return(scan, &then_scan, base_returns) ||
            !return_literal_record_branch_return(scan, &else_scan, base_returns))
            return false;
        scan->terminated = true;
        return true;
    }

    if (then_falls_through && else_falls_through) {
        if (!return_literal_merge_branch_locals(scan, &then_scan, &else_scan, base_locals))
            return false;
    } else if (then_falls_through) {
        *scan = then_scan;
        scan->nlocals = base_locals;
        record_then_return = false;
    } else {
        *scan = else_scan;
        scan->nlocals = base_locals;
        record_else_return = false;
    }
    scan->terminated = false;
    if (record_then_return && !return_literal_record_branch_return(scan, &then_scan, base_returns))
        return false;
    if (record_else_return && !return_literal_record_branch_return(scan, &else_scan, base_returns))
        return false;
    return true;
}

static bool return_literal_scan_node(XgReturnObjectLiteralScan *scan, const AstNode *node) {
    if (!scan || scan->failed)
        return false;
    if (scan->terminated)
        return true;
    if (!node)
        return true;
    switch (node->type) {
        case AST_BLOCK: {
            uint32_t base_locals = scan->nlocals;
            bool ok = return_literal_scan_node_list(scan, node->as.block.statements,
                                                    node->as.block.count);
            scan->nlocals = base_locals;
            return ok;
        }
        case AST_EXPR_STMT:
            return return_literal_scan_node(scan, node->as.expr_stmt);
        case AST_VAR_DECL:
        case AST_CONST_DECL: {
            const AstNode *initializer = node->as.var_decl.initializer;
            const ObjectLiteralNode *literal = return_literal_static_literal(scan, initializer);
            if (literal)
                return return_literal_push_local(scan, node->as.var_decl.name,
                                                 node->as.var_decl.symbol_id, literal);
            if (initializer) {
                XgReturnObjectLiteralScan resolved_scan = *scan;
                if (return_literal_resolve_return_value(&resolved_scan, initializer, &literal) &&
                    literal) {
                    *scan = resolved_scan;
                    return return_literal_push_local(scan, node->as.var_decl.name,
                                                     node->as.var_decl.symbol_id, literal);
                }
            }
            return return_literal_scan_node(scan, initializer);
        }
        case AST_ASSIGNMENT:
            return return_literal_scan_assignment(scan, node->as.assignment.name,
                                                  node->as.assignment.symbol_id,
                                                  node->as.assignment.value);
        case AST_COMPOUND_ASSIGNMENT:
            if (!return_literal_scan_assignment(scan, node->as.compound_assignment.name,
                                                node->as.compound_assignment.symbol_id,
                                                node->as.compound_assignment.value))
                return false;
            return return_literal_scan_node(scan, node->as.compound_assignment.object);
        case AST_INC:
            return return_literal_scan_assignment(scan, node->as.inc.name, node->as.inc.symbol_id,
                                                  NULL);
        case AST_DEC:
            return return_literal_scan_assignment(scan, node->as.dec.name, node->as.dec.symbol_id,
                                                  NULL);
        case AST_RETURN_STMT: {
            const ObjectLiteralNode *literal;
            if (node->as.return_stmt.value_count != 1 || !node->as.return_stmt.values)
                return false;
            if (!return_literal_resolve_return_value(scan, node->as.return_stmt.values[0],
                                                     &literal))
                return false;
            if (!return_literal_record(scan, literal))
                return false;
            scan->terminated = true;
            return true;
        }
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            return true;
        case AST_VARIABLE:
            return return_literal_scan_tracked_local_read(scan, node);
        case AST_GROUPING:
            return return_literal_scan_node(scan, node->as.grouping);
        case AST_MOVE_EXPR:
            return return_literal_scan_node(scan, node->as.move_expr.expr);
        case AST_UNSAFE_EXPR:
            return return_literal_scan_node(scan, node->as.unsafe_expr.operand);
        case AST_FORCE_UNWRAP:
            return return_literal_scan_node(scan, node->as.unary.operand);
        case AST_CALL_EXPR:
            return return_literal_scan_node(scan, node->as.call_expr.callee) &&
                   return_literal_scan_node_list(scan, node->as.call_expr.arguments,
                                                 node->as.call_expr.arg_count);
        case AST_MEMBER_ACCESS:
            return return_literal_scan_node(scan, node->as.member_access.object);
        case AST_MEMBER_SET:
            return return_literal_scan_node(scan, node->as.member_set.object) &&
                   return_literal_scan_node(scan, node->as.member_set.value);
        case AST_INDEX_GET:
            return return_literal_scan_node(scan, node->as.index_get.array) &&
                   return_literal_scan_node(scan, node->as.index_get.index);
        case AST_INDEX_SET:
            return return_literal_scan_node(scan, node->as.index_set.array) &&
                   return_literal_scan_node(scan, node->as.index_set.index) &&
                   return_literal_scan_node(scan, node->as.index_set.value);
        case AST_ARRAY_LITERAL:
            return return_literal_scan_node_list(scan, node->as.array_literal.elements,
                                                 node->as.array_literal.count) &&
                   return_literal_scan_node(scan, node->as.array_literal.repeat_value) &&
                   return_literal_scan_node(scan, node->as.array_literal.repeat_count);
        case AST_TERNARY:
            return return_literal_scan_node(scan, node->as.ternary.condition) &&
                   return_literal_scan_node(scan, node->as.ternary.true_expr) &&
                   return_literal_scan_node(scan, node->as.ternary.false_expr);
        case AST_OBJECT_LITERAL:
            for (int i = 0; i < node->as.object_literal.count; i++) {
                if (node->as.object_literal.computed && node->as.object_literal.computed[i] &&
                    !return_literal_scan_node(scan, node->as.object_literal.keys
                                                        ? node->as.object_literal.keys[i]
                                                        : NULL))
                    return false;
                if (!return_literal_scan_node(scan, node->as.object_literal.values
                                                        ? node->as.object_literal.values[i]
                                                        : NULL))
                    return false;
            }
            return true;
        case AST_IF_STMT:
            return return_literal_scan_if_stmt(scan, &node->as.if_stmt);
        case AST_WHILE_STMT:
        case AST_FOR_STMT:
        case AST_FOR_IN_STMT:
        case AST_TRY_CATCH:
        case AST_SELECT_STMT:
        case AST_SCOPE_BLOCK:
            scan->failed = true;
            return false;
        case AST_MATCH_EXPR:
            return return_literal_scan_match_expr(scan, node);
        default:
            return true;
    }
}

static bool body_find_unique_return_literal(const AstNode *node, XgReturnLiteralResolver resolver,
                                            const ObjectLiteralNode **out_literal, bool *out_seen) {
    XgReturnObjectLiteralScan scan;
    if (!out_literal || !out_seen)
        return true;
    memset(&scan, 0, sizeof(scan));
    scan.resolver = resolver ? resolver : body_static_object_literal;
    scan.literal = *out_literal;
    scan.seen = *out_seen;
    if (!return_literal_scan_node(&scan, node) || scan.failed)
        return false;
    *out_literal = scan.literal;
    *out_seen = scan.seen;
    return true;
}

static bool body_find_unique_return_object_literal(const AstNode *node,
                                                   const ObjectLiteralNode **out_literal,
                                                   bool *out_seen) {
    return body_find_unique_return_literal(node, body_static_object_literal, out_literal, out_seen);
}

static bool body_find_unique_return_json_array_element_literal(
    const AstNode *node, const ObjectLiteralNode **out_literal, bool *out_seen) {
    return body_find_unique_return_literal(node, body_static_json_array_element_literal,
                                           out_literal, out_seen);
}

static const ObjectLiteralNode *body_unique_pending_json_return_literal(const XgPendingBody *body) {
    const ObjectLiteralNode *literal = NULL;
    bool seen = false;
    if (!body || !body_type_ref_is_json(body_pending_return_type_ref(body)))
        return NULL;
    if (!body_find_unique_return_object_literal(body->body, &literal, &seen))
        return NULL;
    return seen ? literal : NULL;
}

static const ObjectLiteralNode *
body_unique_pending_json_array_return_literal(const XgPendingBody *body) {
    const ObjectLiteralNode *literal = NULL;
    bool seen = false;
    if (!body || !body_type_ref_is_json_array(body_pending_return_type_ref(body)))
        return NULL;
    if (!body_find_unique_return_json_array_element_literal(body->body, &literal, &seen))
        return NULL;
    return seen ? literal : NULL;
}

static XgObjectShapeId body_lookup_call_json_return_shape(XgBodyCollect *bc, const AstNode *expr,
                                                          const ObjectLiteralNode **out_literal) {
    const XgPendingBody *body;
    const ObjectLiteralNode *literal;
    uint32_t type_key;
    const XrTypeRef *return_type;
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
        case AST_AS_EXPR:
            return body_lookup_call_json_return_shape(bc, expr->as.as_expr.expr, out_literal);
        case AST_COMPTIME_EXPR:
            return body_lookup_call_json_return_shape(bc, expr->as.comptime_expr.expr, out_literal);
        default:
            break;
    }
    if (expr->type != AST_CALL_EXPR)
        return XG_NO_ID;
    body = body_find_call_body(bc, &expr->as.call_expr);
    literal = body_unique_pending_json_return_literal(body);
    if (!literal)
        return XG_NO_ID;
    return_type = body_pending_return_type_ref(body);
    type_key = return_type ? hash_tref32(return_type) : hash_named_type_key32("Json", NULL, 0);
    if (out_literal)
        *out_literal = literal;
    return body_add_json_shape_for_literal(bc, literal, (uint32_t) expr->line, type_key);
}

static XgObjectShapeId body_lookup_call_sequence_json_shape(XgBodyCollect *bc, const AstNode *expr,
                                                            const ObjectLiteralNode **out_literal) {
    const XgPendingBody *body;
    const ObjectLiteralNode *literal;
    const XrTypeRef *elem_type;
    uint32_t elem_type_key;
    if (out_literal)
        *out_literal = NULL;
    if (!bc || !expr)
        return XG_NO_ID;
    switch (expr->type) {
        case AST_GROUPING:
            return body_lookup_call_sequence_json_shape(bc, expr->as.grouping, out_literal);
        case AST_MOVE_EXPR:
            return body_lookup_call_sequence_json_shape(bc, expr->as.move_expr.expr, out_literal);
        case AST_UNSAFE_EXPR:
            return body_lookup_call_sequence_json_shape(bc, expr->as.unsafe_expr.operand,
                                                        out_literal);
        case AST_FORCE_UNWRAP:
            return body_lookup_call_sequence_json_shape(bc, expr->as.unary.operand, out_literal);
        case AST_AS_EXPR:
            return body_lookup_call_sequence_json_shape(bc, expr->as.as_expr.expr, out_literal);
        case AST_COMPTIME_EXPR:
            return body_lookup_call_sequence_json_shape(bc, expr->as.comptime_expr.expr,
                                                        out_literal);
        default:
            break;
    }
    if (expr->type != AST_CALL_EXPR)
        return XG_NO_ID;
    body = body_find_call_body(bc, &expr->as.call_expr);
    literal = body_unique_pending_json_array_return_literal(body);
    if (!literal)
        return XG_NO_ID;
    elem_type = body_type_ref_sequence_elem_type_ref(body_pending_return_type_ref(body));
    elem_type_key = elem_type ? hash_tref32(elem_type) : hash_named_type_key32("Json", NULL, 0);
    if (out_literal)
        *out_literal = literal;
    return body_add_json_shape_for_literal(bc, literal, (uint32_t) expr->line, elem_type_key);
}

static const AstNode *body_json_receiver_unwrap(const AstNode *expr) {
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
            case AST_AS_EXPR:
                expr = expr->as.as_expr.expr;
                break;
            case AST_COMPTIME_EXPR:
                expr = expr->as.comptime_expr.expr;
                break;
            default:
                return expr;
        }
    }
    return NULL;
}

static XgObjectShapeId
body_lookup_static_sequence_json_shape(XgBodyCollect *bc, const AstNode *expr,
                                       const ObjectLiteralNode **out_literal) {
    const AstNode *array_expr = body_json_receiver_unwrap(expr);
    const ObjectLiteralNode *literal = body_static_json_array_element_literal(array_expr);
    if (out_literal)
        *out_literal = NULL;
    if (!bc || !array_expr || !literal)
        return XG_NO_ID;
    if (out_literal)
        *out_literal = literal;
    return body_add_json_shape_for_literal(bc, literal, (uint32_t) array_expr->line,
                                           hash_named_type_key32("Json", NULL, 0));
}

static XgObjectShapeId body_lookup_static_json_shape_for_type_key(XgBodyCollect *bc,
                                                                  uint32_t type_key) {
    const XgObjectShapeSummary *shape =
        body_find_object_shape_for_type_key(bc, type_key, XG_OBJECT_SHAPE_STATIC);
    return shape ? shape->object_shape_id : XG_NO_ID;
}

static XgObjectShapeId body_lookup_local_json_shape(XgBodyCollect *bc, const AstNode *expr,
                                                    const ObjectLiteralNode **out_literal);
static bool body_object_shape_id_same_shape(XgBodyCollect *bc, XgObjectShapeId left_id,
                                            XgObjectShapeId right_id);

static XgObjectShapeId
body_lookup_sequence_json_ternary_arm_shape(XgBodyCollect *bc, const AstNode *expr,
                                            const ObjectLiteralNode **out_literal) {
    XgLocalType *local;
    XgObjectShapeId shape_id;
    if (out_literal)
        *out_literal = NULL;
    if (!bc || !expr)
        return XG_NO_ID;
    local = body_lookup_local_sequence(bc, expr);
    if (local && local->sequence_elem_type_key != 0) {
        if (local->sequence_elem_object_shape_id != XG_NO_ID) {
            if (out_literal)
                *out_literal = local->sequence_elem_object_shape_literal;
            return local->sequence_elem_object_shape_id;
        }
        return body_lookup_static_json_shape_for_type_key(bc, local->sequence_elem_type_key);
    }
    shape_id = body_lookup_static_sequence_json_shape(bc, expr, out_literal);
    if (shape_id != XG_NO_ID)
        return shape_id;
    return body_lookup_call_sequence_json_shape(bc, expr, out_literal);
}

static XgObjectShapeId
body_lookup_sequence_json_ternary_shape(XgBodyCollect *bc, const AstNode *expr,
                                        const ObjectLiteralNode **out_literal) {
    const AstNode *value = body_json_receiver_unwrap(expr);
    const ObjectLiteralNode *then_literal = NULL;
    const ObjectLiteralNode *else_literal = NULL;
    XgObjectShapeId then_shape_id;
    XgObjectShapeId else_shape_id;
    if (out_literal)
        *out_literal = NULL;
    if (!bc || !value || value->type != AST_TERNARY)
        return XG_NO_ID;
    then_shape_id =
        body_lookup_sequence_json_ternary_arm_shape(bc, value->as.ternary.true_expr, &then_literal);
    if (then_shape_id == XG_NO_ID)
        return XG_NO_ID;
    else_shape_id = body_lookup_sequence_json_ternary_arm_shape(bc, value->as.ternary.false_expr,
                                                                &else_literal);
    if (else_shape_id == XG_NO_ID ||
        !body_object_shape_id_same_shape(bc, then_shape_id, else_shape_id))
        return XG_NO_ID;
    if (out_literal)
        *out_literal = then_literal ? then_literal : else_literal;
    return then_shape_id;
}

static XgObjectShapeId body_lookup_sequence_json_shape(XgBodyCollect *bc, const AstNode *expr,
                                                       const ObjectLiteralNode **out_literal) {
    XgLocalType *local;
    const AstNode *receiver = body_json_receiver_unwrap(expr);
    const AstNode *array_expr;
    if (!bc || !receiver || receiver->type != AST_INDEX_GET)
        return XG_NO_ID;
    array_expr = body_json_receiver_unwrap(receiver->as.index_get.array);
    if (!array_expr)
        return XG_NO_ID;
    local = body_lookup_local_sequence(bc, array_expr);
    if (local && local->sequence_elem_type_key != 0) {
        if (local->sequence_elem_object_shape_id != XG_NO_ID) {
            if (out_literal)
                *out_literal = local->sequence_elem_object_shape_literal;
            return local->sequence_elem_object_shape_id;
        }
        return body_lookup_static_json_shape_for_type_key(bc, local->sequence_elem_type_key);
    }
    {
        XgObjectShapeId shape_id =
            body_lookup_static_sequence_json_shape(bc, array_expr, out_literal);
        if (shape_id != XG_NO_ID)
            return shape_id;
    }
    {
        XgObjectShapeId shape_id =
            body_lookup_sequence_json_ternary_shape(bc, array_expr, out_literal);
        if (shape_id != XG_NO_ID)
            return shape_id;
    }
    return body_lookup_call_sequence_json_shape(bc, array_expr, out_literal);
}

static XgObjectShapeId
body_lookup_static_json_ternary_shape(XgBodyCollect *bc, const AstNode *expr,
                                      const ObjectLiteralNode **out_literal) {
    const AstNode *value = body_json_receiver_unwrap(expr);
    const ObjectLiteralNode *then_literal;
    const ObjectLiteralNode *else_literal;
    if (out_literal)
        *out_literal = NULL;
    if (!bc || !value || value->type != AST_TERNARY)
        return XG_NO_ID;
    then_literal = body_static_object_literal(value->as.ternary.true_expr);
    if (!then_literal &&
        body_lookup_local_json_shape(bc, value->as.ternary.true_expr, &then_literal) == XG_NO_ID)
        return XG_NO_ID;
    else_literal = body_static_object_literal(value->as.ternary.false_expr);
    if (!else_literal &&
        body_lookup_local_json_shape(bc, value->as.ternary.false_expr, &else_literal) == XG_NO_ID)
        return XG_NO_ID;
    if (!return_literal_same_shape(then_literal, else_literal) || !then_literal)
        return XG_NO_ID;
    if (out_literal)
        *out_literal = then_literal;
    return body_add_json_shape_for_literal(bc, then_literal, (uint32_t) value->line,
                                           hash_named_type_key32("Json", NULL, 0));
}

static const ClassDeclNode *producer_lookup_class_decl_node(const XgProducer *p,
                                                            XgClassId class_id) {
    const XgClassNameRow *row;
    if (!p || class_id == XG_NO_ID)
        return NULL;
    row = producer_lookup_class_row_by_id(p, class_id);
    if (!row || !row->class_node)
        return NULL;
    if (row->class_node->type == AST_CLASS_DECL)
        return &row->class_node->as.class_decl;
    if (row->class_node->type == AST_STRUCT_DECL)
        return &row->class_node->as.struct_decl;
    if (row->class_node->type == AST_UNION_DECL)
        return &row->class_node->as.union_decl;
    return NULL;
}

static const ObjectLiteralNode *producer_find_class_field_json_initializer(const XgProducer *p,
                                                                           XgClassId class_id,
                                                                           uint32_t field_name_id) {
    const ClassDeclNode *cls;
    if (!p || class_id == XG_NO_ID || field_name_id == 0)
        return NULL;
    cls = producer_lookup_class_decl_node(p, class_id);
    if (!cls)
        return NULL;
    for (int i = 0; i < cls->field_count; i++) {
        const AstNode *field_node = cls->fields ? cls->fields[i] : NULL;
        const FieldDeclNode *field;
        if (!field_node || field_node->type != AST_FIELD_DECL)
            continue;
        field = &field_node->as.field_decl;
        if (field->is_static || hash_name32(field->name) != field_name_id ||
            !body_type_ref_is_json(field->field_type))
            continue;
        return body_static_object_literal(field->initializer);
    }
    return NULL;
}

typedef struct XgJsonCtorFieldAssignScan {
    uint32_t field_name_id;
    const ObjectLiteralNode *literal;
    uint64_t shape_hash;
    int field_count;
    bool saw_assignment;
    bool failed;
} XgJsonCtorFieldAssignScan;

static bool body_expr_is_this(const AstNode *expr) {
    const AstNode *receiver = body_json_receiver_unwrap(expr);
    return receiver && receiver->type == AST_THIS_EXPR;
}

static bool ctor_scan_node_for_json_field_assignment(XgJsonCtorFieldAssignScan *scan,
                                                     const AstNode *node);

static bool ctor_scan_shapes_match(const XgJsonCtorFieldAssignScan *left,
                                   const XgJsonCtorFieldAssignScan *right) {
    return left && right && left->saw_assignment && right->saw_assignment &&
           left->field_count == right->field_count && left->shape_hash == right->shape_hash;
}

static bool ctor_scan_if_json_field_assignment(XgJsonCtorFieldAssignScan *scan,
                                               const IfStmtNode *stmt) {
    XgJsonCtorFieldAssignScan base;
    XgJsonCtorFieldAssignScan then_scan;
    XgJsonCtorFieldAssignScan else_scan;
    if (!scan || !stmt || !stmt->then_branch) {
        if (scan)
            scan->failed = true;
        return false;
    }
    if (!ctor_scan_node_for_json_field_assignment(scan, stmt->condition))
        return false;

    base = *scan;
    then_scan = base;
    else_scan = base;
    if (!ctor_scan_node_for_json_field_assignment(&then_scan, stmt->then_branch)) {
        scan->failed = true;
        return false;
    }
    if (stmt->else_branch) {
        if (!ctor_scan_node_for_json_field_assignment(&else_scan, stmt->else_branch)) {
            scan->failed = true;
            return false;
        }
    }

    if (base.saw_assignment) {
        *scan = base;
        return true;
    }
    if (!stmt->else_branch || !ctor_scan_shapes_match(&then_scan, &else_scan)) {
        scan->failed = true;
        return false;
    }
    *scan = then_scan;
    return true;
}

static bool ctor_scan_node_list_for_json_field_assignment(XgJsonCtorFieldAssignScan *scan,
                                                          AstNode *const *nodes, int count) {
    if (!scan || scan->failed)
        return false;
    for (int i = 0; i < count; i++) {
        if (!ctor_scan_node_for_json_field_assignment(scan, nodes ? nodes[i] : NULL))
            return false;
    }
    return true;
}

static bool ctor_scan_object_json_field_assignment(XgJsonCtorFieldAssignScan *scan,
                                                   const ObjectLiteralNode *literal) {
    uint64_t shape_hash;
    if (!scan || !literal)
        return false;
    shape_hash = body_json_shape_hash(literal);
    if (!scan->saw_assignment) {
        scan->literal = literal;
        scan->shape_hash = shape_hash;
        scan->field_count = literal->count;
        scan->saw_assignment = true;
        return true;
    }
    if (scan->field_count != literal->count || scan->shape_hash != shape_hash) {
        scan->failed = true;
        return false;
    }
    return true;
}

static bool ctor_scan_json_field_assignment_literal(XgJsonCtorFieldAssignScan *scan,
                                                    const AstNode *value,
                                                    const ObjectLiteralNode **out_literal) {
    const ObjectLiteralNode *then_literal = NULL;
    const ObjectLiteralNode *else_literal = NULL;
    const ObjectLiteralNode *literal;
    if (!scan || !out_literal)
        return false;
    *out_literal = NULL;
    if (!value)
        return false;
    switch (value->type) {
        case AST_GROUPING:
            return ctor_scan_json_field_assignment_literal(scan, value->as.grouping, out_literal);
        case AST_MOVE_EXPR:
            return ctor_scan_json_field_assignment_literal(scan, value->as.move_expr.expr,
                                                           out_literal);
        case AST_UNSAFE_EXPR:
            return ctor_scan_json_field_assignment_literal(scan, value->as.unsafe_expr.operand,
                                                           out_literal);
        case AST_FORCE_UNWRAP:
            return ctor_scan_json_field_assignment_literal(scan, value->as.unary.operand,
                                                           out_literal);
        case AST_TERNARY:
            if (!ctor_scan_node_for_json_field_assignment(scan, value->as.ternary.condition) ||
                scan->failed)
                return false;
            if (!ctor_scan_json_field_assignment_literal(scan, value->as.ternary.true_expr,
                                                         &then_literal) ||
                !then_literal)
                return false;
            if (!ctor_scan_json_field_assignment_literal(scan, value->as.ternary.false_expr,
                                                         &else_literal) ||
                !else_literal)
                return false;
            if (!return_literal_same_shape(then_literal, else_literal)) {
                scan->failed = true;
                return false;
            }
            *out_literal = then_literal;
            return true;
        default:
            break;
    }
    literal = body_static_object_literal(value);
    if (!literal)
        return false;
    *out_literal = literal;
    return true;
}

static bool ctor_scan_member_set_json_field_assignment(XgJsonCtorFieldAssignScan *scan,
                                                       const AstNode *node) {
    const ObjectLiteralNode *literal;
    if (!scan || !node || node->type != AST_MEMBER_SET)
        return true;
    if (!node->as.member_set.member ||
        hash_name32(node->as.member_set.member) != scan->field_name_id ||
        !body_expr_is_this(node->as.member_set.object)) {
        return ctor_scan_node_for_json_field_assignment(scan, node->as.member_set.object) &&
               ctor_scan_node_for_json_field_assignment(scan, node->as.member_set.value);
    }
    if (!ctor_scan_json_field_assignment_literal(scan, node->as.member_set.value, &literal) ||
        !literal) {
        scan->failed = true;
        return false;
    }
    return ctor_scan_object_json_field_assignment(scan, literal);
}

static bool ctor_scan_node_for_json_field_assignment(XgJsonCtorFieldAssignScan *scan,
                                                     const AstNode *node) {
    if (!scan || scan->failed)
        return false;
    if (!node)
        return true;
    switch (node->type) {
        case AST_PROGRAM:
            return ctor_scan_node_list_for_json_field_assignment(scan, node->as.program.statements,
                                                                 node->as.program.count);
        case AST_BLOCK:
            return ctor_scan_node_list_for_json_field_assignment(scan, node->as.block.statements,
                                                                 node->as.block.count);
        case AST_EXPR_STMT:
            return ctor_scan_node_for_json_field_assignment(scan, node->as.expr_stmt);
        case AST_MEMBER_SET:
            return ctor_scan_member_set_json_field_assignment(scan, node);
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            return ctor_scan_node_for_json_field_assignment(scan, node->as.var_decl.initializer);
        case AST_ASSIGNMENT:
            return ctor_scan_node_for_json_field_assignment(scan, node->as.assignment.value);
        case AST_COMPOUND_ASSIGNMENT:
            return ctor_scan_node_for_json_field_assignment(scan,
                                                            node->as.compound_assignment.object) &&
                   ctor_scan_node_for_json_field_assignment(scan,
                                                            node->as.compound_assignment.value);
        case AST_RETURN_STMT:
            return ctor_scan_node_list_for_json_field_assignment(scan, node->as.return_stmt.values,
                                                                 node->as.return_stmt.value_count);
        case AST_CALL_EXPR:
            scan->failed = true;
            return false;
        case AST_MEMBER_ACCESS:
            return ctor_scan_node_for_json_field_assignment(scan, node->as.member_access.object);
        case AST_INDEX_GET:
            return ctor_scan_node_for_json_field_assignment(scan, node->as.index_get.array) &&
                   ctor_scan_node_for_json_field_assignment(scan, node->as.index_get.index);
        case AST_INDEX_SET:
            return ctor_scan_node_for_json_field_assignment(scan, node->as.index_set.array) &&
                   ctor_scan_node_for_json_field_assignment(scan, node->as.index_set.index) &&
                   ctor_scan_node_for_json_field_assignment(scan, node->as.index_set.value);
        case AST_ARRAY_LITERAL:
            return ctor_scan_node_list_for_json_field_assignment(
                       scan, node->as.array_literal.elements, node->as.array_literal.count) &&
                   ctor_scan_node_for_json_field_assignment(scan,
                                                            node->as.array_literal.repeat_value) &&
                   ctor_scan_node_for_json_field_assignment(scan,
                                                            node->as.array_literal.repeat_count);
        case AST_TUPLE_LITERAL:
            return ctor_scan_node_list_for_json_field_assignment(
                scan, node->as.tuple_literal.elements, node->as.tuple_literal.count);
        case AST_SPREAD_EXPR:
            return ctor_scan_node_for_json_field_assignment(scan, node->as.spread_expr.expr);
        case AST_OBJECT_LITERAL:
            for (int i = 0; i < node->as.object_literal.count; i++) {
                if (node->as.object_literal.computed && node->as.object_literal.computed[i] &&
                    !ctor_scan_node_for_json_field_assignment(
                        scan,
                        node->as.object_literal.keys ? node->as.object_literal.keys[i] : NULL))
                    return false;
                if (!ctor_scan_node_for_json_field_assignment(
                        scan,
                        node->as.object_literal.values ? node->as.object_literal.values[i] : NULL))
                    return false;
            }
            return true;
        case AST_MAP_LITERAL:
            for (int i = 0; i < node->as.map_literal.count; i++) {
                if (!ctor_scan_node_for_json_field_assignment(
                        scan, node->as.map_literal.keys ? node->as.map_literal.keys[i] : NULL) ||
                    !ctor_scan_node_for_json_field_assignment(
                        scan, node->as.map_literal.values ? node->as.map_literal.values[i] : NULL))
                    return false;
            }
            return true;
        case AST_SET_LITERAL:
            return ctor_scan_node_list_for_json_field_assignment(
                scan, node->as.set_literal.elements, node->as.set_literal.count);
        case AST_STRUCT_LITERAL:
            return ctor_scan_node_list_for_json_field_assignment(
                scan, node->as.struct_literal.field_values, node->as.struct_literal.field_count);
        case AST_GROUPING:
            return ctor_scan_node_for_json_field_assignment(scan, node->as.grouping);
        case AST_MOVE_EXPR:
            return ctor_scan_node_for_json_field_assignment(scan, node->as.move_expr.expr);
        case AST_UNSAFE_EXPR:
            return ctor_scan_node_for_json_field_assignment(scan, node->as.unsafe_expr.operand);
        case AST_FORCE_UNWRAP:
            return ctor_scan_node_for_json_field_assignment(scan, node->as.unary.operand);
        case AST_TERNARY:
            return ctor_scan_node_for_json_field_assignment(scan, node->as.ternary.condition) &&
                   ctor_scan_node_for_json_field_assignment(scan, node->as.ternary.true_expr) &&
                   ctor_scan_node_for_json_field_assignment(scan, node->as.ternary.false_expr);
        case AST_IF_STMT:
            return ctor_scan_if_json_field_assignment(scan, &node->as.if_stmt);
        case AST_WHILE_STMT:
        case AST_FOR_STMT:
        case AST_FOR_IN_STMT:
        case AST_TRY_CATCH:
        case AST_MATCH_EXPR:
        case AST_SELECT_STMT:
        case AST_SCOPE_BLOCK:
            scan->failed = true;
            return false;
        default:
            return true;
    }
}

static const ObjectLiteralNode *
producer_find_class_field_json_constructor_assignment(const XgProducer *p, XgClassId class_id,
                                                      uint32_t field_name_id) {
    const ClassDeclNode *cls;
    const ObjectLiteralNode *literal = NULL;
    uint64_t shape_hash = 0;
    int field_count = 0;
    bool saw_constructor = false;
    if (!p || class_id == XG_NO_ID || field_name_id == 0)
        return NULL;
    cls = producer_lookup_class_decl_node(p, class_id);
    if (!cls)
        return NULL;
    for (int i = 0; i < cls->method_count; i++) {
        const AstNode *method_node = cls->methods ? cls->methods[i] : NULL;
        const MethodDeclNode *method;
        XgJsonCtorFieldAssignScan scan;
        if (!method_node || method_node->type != AST_METHOD_DECL)
            continue;
        method = &method_node->as.method_decl;
        if (!method->is_constructor || method->is_static || method->is_static_constructor)
            continue;
        saw_constructor = true;
        memset(&scan, 0, sizeof(scan));
        scan.field_name_id = field_name_id;
        if (!method->body || !ctor_scan_node_for_json_field_assignment(&scan, method->body) ||
            !scan.saw_assignment || scan.failed)
            return NULL;
        if (!literal) {
            literal = scan.literal;
            shape_hash = scan.shape_hash;
            field_count = scan.field_count;
            continue;
        }
        if (field_count != scan.field_count || shape_hash != scan.shape_hash)
            return NULL;
    }
    return saw_constructor ? literal : NULL;
}

static bool producer_class_field_json_constructor_writes_match_initializer(
    const XgProducer *p, XgClassId class_id, uint32_t field_name_id,
    const ObjectLiteralNode *initializer) {
    const ClassDeclNode *cls;
    uint64_t shape_hash;
    if (!p || class_id == XG_NO_ID || field_name_id == 0 || !initializer)
        return false;
    cls = producer_lookup_class_decl_node(p, class_id);
    if (!cls)
        return false;
    shape_hash = body_json_shape_hash(initializer);
    for (int i = 0; i < cls->method_count; i++) {
        const AstNode *method_node = cls->methods ? cls->methods[i] : NULL;
        const MethodDeclNode *method;
        XgJsonCtorFieldAssignScan scan;
        if (!method_node || method_node->type != AST_METHOD_DECL)
            continue;
        method = &method_node->as.method_decl;
        if (!method->is_constructor || method->is_static || method->is_static_constructor)
            continue;
        memset(&scan, 0, sizeof(scan));
        scan.field_name_id = field_name_id;
        scan.literal = initializer;
        scan.shape_hash = shape_hash;
        scan.field_count = initializer->count;
        scan.saw_assignment = true;
        if (!method->body || !ctor_scan_node_for_json_field_assignment(&scan, method->body) ||
            scan.failed)
            return false;
    }
    return true;
}

static XgObjectShapeId body_lookup_class_field_json_shape(XgBodyCollect *bc, const AstNode *expr) {
    const AstNode *receiver = body_json_receiver_unwrap(expr);
    XgClassId receiver_class;
    const XgClassFieldSummary *field;
    XgObjectShapeId shape_id;
    const ObjectLiteralNode *initializer;
    if (!bc || !receiver || receiver->type != AST_MEMBER_ACCESS || !receiver->as.member_access.name)
        return XG_NO_ID;
    receiver_class = body_resolve_expr_class(bc, receiver->as.member_access.object);
    field = body_find_class_field_in_hierarchy(bc, receiver_class,
                                               hash_name32(receiver->as.member_access.name));
    if (!field || field->type_key == 0)
        return XG_NO_ID;
    shape_id = body_lookup_static_json_shape_for_type_key(bc, field->type_key);
    if (shape_id != XG_NO_ID)
        return shape_id;
    initializer = producer_find_class_field_json_initializer(bc->producer, field->owner_class_id,
                                                             field->name_id);
    if (initializer) {
        if (!producer_class_field_json_constructor_writes_match_initializer(
                bc->producer, field->owner_class_id, field->name_id, initializer))
            return XG_NO_ID;
    } else {
        initializer = producer_find_class_field_json_constructor_assignment(
            bc->producer, field->owner_class_id, field->name_id);
    }
    if (!initializer)
        return XG_NO_ID;
    return body_add_json_shape_for_literal(bc, initializer, (uint32_t) receiver->line,
                                           field->type_key);
}

static void body_bind_json_shape_local(XgBodyCollect *bc, const char *name,
                                       XgObjectShapeId shape_id, const ObjectLiteralNode *literal) {
    XgLocalType *row;
    if (!bc || !name || shape_id == XG_NO_ID)
        return;
    row = body_find_local(bc, name);
    if (!row)
        return;
    row->object_shape_id = shape_id;
    row->object_shape_literal = literal;
}

static void body_clear_json_shape_local(XgBodyCollect *bc, const char *name) {
    XgLocalType *row;
    if (!bc || !name)
        return;
    row = body_find_local(bc, name);
    if (!row)
        return;
    row->object_shape_id = XG_NO_ID;
    row->object_shape_literal = NULL;
}

static XgObjectShapeId body_lookup_local_json_shape(XgBodyCollect *bc, const AstNode *expr,
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
    if (!row || row->object_shape_id == XG_NO_ID)
        return XG_NO_ID;
    if (out_literal)
        *out_literal = row->object_shape_literal;
    return row->object_shape_id;
}

static XgObjectShapeId body_lookup_json_shape(XgBodyCollect *bc, const AstNode *expr,
                                              const ObjectLiteralNode **out_literal) {
    XgObjectShapeId shape_id = body_lookup_local_json_shape(bc, expr, out_literal);
    if (shape_id != XG_NO_ID)
        return shape_id;
    shape_id = body_lookup_call_json_return_shape(bc, expr, out_literal);
    if (shape_id != XG_NO_ID)
        return shape_id;
    shape_id = body_lookup_static_json_ternary_shape(bc, expr, out_literal);
    if (shape_id != XG_NO_ID)
        return shape_id;
    shape_id = body_lookup_sequence_json_shape(bc, expr, out_literal);
    if (shape_id != XG_NO_ID)
        return shape_id;
    return body_lookup_class_field_json_shape(bc, expr);
}

static int body_json_shape_static_field_index(XgBodyCollect *bc, XgObjectShapeId shape_id,
                                              const ObjectLiteralNode *literal, const char *name) {
    uint32_t name_id;
    if (!bc || !bc->evidence || shape_id == XG_NO_ID || !name)
        return -1;
    if (literal)
        return body_object_literal_static_field_index(literal, name);
    name_id = hash_name32(name);
    if (name_id == 0)
        return -1;
    for (uint32_t i = 0; i < bc->evidence->nobject_fields; i++) {
        const XgObjectFieldSummary *field = &bc->evidence->object_fields[i];
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
        case AST_INDEX_GET: {
            const AstNode *array_expr;
            array_expr = body_json_receiver_unwrap(expr->as.index_get.array);
            row = body_lookup_local_sequence(bc, array_expr);
            if (row)
                return row->sequence_elem_object_shape_id == XG_NO_ID &&
                       row->sequence_elem_type_key == hash_named_type_key32("Json", NULL, 0);
            return array_expr && array_expr->type == AST_CALL_EXPR &&
                   body_type_ref_is_json_array(
                       body_call_return_type_ref(bc, &array_expr->as.call_expr));
        }
        default:
            break;
    }
    if (expr->type != AST_VARIABLE || !expr->as.variable.name)
        return false;
    row = body_find_local(bc, expr->as.variable.name);
    return row && row->object_shape_id == XG_NO_ID && body_local_type_is_json(row);
}

static void body_add_json_member_access(XgBodyCollect *bc, const AstNode *node, bool mutating) {
    const ObjectLiteralNode *literal = NULL;
    XgObjectShapeId shape_id;
    const AstNode *receiver;
    const char *name;
    int field_index;
    XgJsonDynamicAccessSummary row;
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
    if (shape_id != XG_NO_ID && field_index >= 0) {
        const XgObjectShapeSummary *shape =
            xg_global_evidence_find_object_shape(bc->evidence, shape_id);
        if (shape && (shape->flags & XG_OBJECT_SHAPE_HAS_COMPUTED_KEYS) == 0) {
            const ObjectLiteralNode *object_literal = NULL;
            XgObjectShapeId object_shape_id =
                body_lookup_object_shape(bc, receiver, &object_literal);
            if (object_shape_id != XG_NO_ID && body_object_shape_static_field_index(
                                                   bc, object_shape_id, object_literal, name) >= 0)
                return;
            (void) body_add_resolved_object_field_access(bc, node, mutating, shape_id, name,
                                                         (uint16_t) field_index);
            return;
        }
    }
    memset(&row, 0, sizeof(row));
    row.json_dynamic_access_id = (XgJsonDynamicAccessId) (bc->evidence->njson_dynamic_accesses + 1);
    row.module_id = bc->module_id;
    row.owner_func_id = bc->owner_func_id;
    row.receiver_shape_id = shape_id;
    row.source_span_id = (uint32_t) node->line;
    row.key_name_id = hash_name32(name);
    row.result_type_key = 0;
    row.field_ordinal = field_index >= 0 ? (uint16_t) field_index : UINT16_MAX;
    row.access_kind =
        mutating ? XG_JSON_DYNAMIC_ACCESS_FIELD_SET : XG_JSON_DYNAMIC_ACCESS_FIELD_GET;
    row.flags = XG_JSON_DYNAMIC_ACCESS_STATIC_KEY;
    if (shape_id != XG_NO_ID)
        row.flags |= XG_JSON_DYNAMIC_ACCESS_RECEIVER_SHAPE_PROVEN;
    if (mutating)
        row.flags |= XG_JSON_DYNAMIC_ACCESS_MUTATING;
    (void) xg_global_evidence_add_json_dynamic_access(bc->evidence, &row);
}

static void body_add_json_index_access(XgBodyCollect *bc, const AstNode *node, bool mutating) {
    const ObjectLiteralNode *literal = NULL;
    const AstNode *receiver;
    const AstNode *key;
    const char *static_key;
    XgObjectShapeId shape_id;
    int field_index = -1;
    XgJsonDynamicAccessSummary row;
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
        const XgObjectShapeSummary *shape =
            xg_global_evidence_find_object_shape(bc->evidence, shape_id);
        if (!shape)
            return;
        field_index = body_json_shape_static_field_index(bc, shape_id, literal, static_key);
        if (field_index < 0)
            return;
        if ((shape->flags & XG_OBJECT_SHAPE_HAS_COMPUTED_KEYS) == 0) {
            const ObjectLiteralNode *object_literal = NULL;
            XgObjectShapeId object_shape_id =
                body_lookup_object_shape(bc, receiver, &object_literal);
            if (object_shape_id != XG_NO_ID &&
                body_object_shape_static_field_index(bc, object_shape_id, object_literal,
                                                     static_key) >= 0)
                return;
            (void) body_add_resolved_object_field_access(bc, node, mutating, shape_id, static_key,
                                                         (uint16_t) field_index);
            return;
        }
    }
    if (!static_key && shape_id == XG_NO_ID)
        return;
    memset(&row, 0, sizeof(row));
    row.json_dynamic_access_id = (XgJsonDynamicAccessId) (bc->evidence->njson_dynamic_accesses + 1);
    row.module_id = bc->module_id;
    row.owner_func_id = bc->owner_func_id;
    row.receiver_shape_id = shape_id;
    row.source_span_id = (uint32_t) node->line;
    row.key_name_id = static_key ? hash_name32(static_key) : 0;
    row.result_type_key = 0;
    row.field_ordinal = field_index >= 0 ? (uint16_t) field_index : UINT16_MAX;
    row.access_kind =
        mutating ? XG_JSON_DYNAMIC_ACCESS_INDEX_SET : XG_JSON_DYNAMIC_ACCESS_INDEX_GET;
    row.flags = 0;
    if (static_key)
        row.flags |= XG_JSON_DYNAMIC_ACCESS_STATIC_KEY;
    else
        row.flags |= XG_JSON_DYNAMIC_ACCESS_COMPUTED_KEY;
    if (shape_id != XG_NO_ID)
        row.flags |= XG_JSON_DYNAMIC_ACCESS_RECEIVER_SHAPE_PROVEN;
    if (mutating)
        row.flags |= XG_JSON_DYNAMIC_ACCESS_MUTATING;
    (void) xg_global_evidence_add_json_dynamic_access(bc->evidence, &row);
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

/* `expr as T` and `expr is T` against a sealed structural object are decode sites: both
 * lower to the validated structural check that compares the value's field set
 * against T's. They therefore need the same json-codec evidence row the
 * explicit decode call gets, or the AOT backend rejects the site for a missing
 * mandatory plan. */
static void body_add_json_codec_shape_test(XgBodyCollect *bc, const AstNode *node,
                                           const XrTypeRef *target, const AstNode *operand) {
    XgJsonCodecSummary row;
    if (!bc || !bc->evidence || !node || !target)
        return;

    /* Only a target that carries an object bridge shape reaches the structural
     * check; every other cast keeps its own lowering and must not claim a codec
     * site. Emitting a row unconditionally also collides on source identity,
     * because nested casts share a start position. */
    const XrTypeRef *shape_target = target;
    while (shape_target &&
           (shape_target->kind == XR_TREF_OPTIONAL || shape_target->kind == XR_TREF_CONST) &&
           shape_target->nchildren == 1 && shape_target->children) {
        shape_target = shape_target->children[0];
    }
    uint32_t target_type_key = hash_tref32(shape_target);
    const XgObjectShapeSummary *target_shape =
        body_find_object_shape_for_type_key(bc, target_type_key, XG_OBJECT_SHAPE_STATIC);
    if (!target_shape)
        return;

    memset(&row, 0, sizeof(row));
    row.codec_id = (XgJsonCodecId) (bc->evidence->njson_codecs + 1);
    row.module_id = bc->module_id;
    row.owner_func_id = bc->owner_func_id;
    row.source_node_id = producer_source_node_id(bc->module_id, node);
    row.source_span_id = (uint32_t) node->line;
    row.codec_kind = XG_JSON_CODEC_DECODE;
    row.target_type_key = target_type_key;
    row.flags |= XG_JSON_CODEC_HAS_TARGET_TYPE;
    if (operand)
        row.input_type_key = body_expr_type_key(bc, operand);

    row.output_shape_id = target_shape->object_shape_id;
    row.field_count = target_shape->field_count;
    row.flags |= XG_JSON_CODEC_HAS_OUTPUT_SHAPE;

    (void) xg_global_evidence_add_json_codec(bc->evidence, &row);
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
    row.source_node_id = producer_source_node_id(bc->module_id, node);
    row.source_span_id = (uint32_t) node->line;
    if (call->arg_count > 0)
        arg0 = call->arguments[0];
    if (arg0)
        row.input_type_key = body_expr_type_key(bc, arg0);

    if (strcmp(method, "parse") == 0) {
        row.codec_kind = XG_JSON_CODEC_PARSE;
        if (arg0 && arg0->type == AST_LITERAL_STRING)
            row.flags |= XG_JSON_CODEC_STATIC_TEXT;
        if (call->type_arg_count == 1 && call->type_args && call->type_args[0]) {
            row.target_type_key = hash_tref32(call->type_args[0]);
            row.flags |= XG_JSON_CODEC_HAS_TARGET_TYPE;
            const XgObjectShapeSummary *target_shape = body_find_object_shape_for_type_key(
                bc, row.target_type_key, XG_OBJECT_SHAPE_STATIC);
            if (target_shape) {
                row.output_shape_id = target_shape->object_shape_id;
                row.field_count = target_shape->field_count;
                row.flags |= XG_JSON_CODEC_HAS_OUTPUT_SHAPE | XG_JSON_CODEC_TARGET_OBJECT_SHAPE;
            }
        }
    } else if (strcmp(method, "decode") == 0) {
        row.codec_kind = XG_JSON_CODEC_DECODE;
        if (call->type_arg_count > 0 && call->type_args && call->type_args[0]) {
            row.target_type_key = hash_tref32(call->type_args[0]);
            row.flags |= XG_JSON_CODEC_HAS_TARGET_TYPE;
            const XgObjectShapeSummary *target_shape = body_find_object_shape_for_type_key(
                bc, row.target_type_key, XG_OBJECT_SHAPE_STATIC);
            if (target_shape) {
                row.output_shape_id = target_shape->object_shape_id;
                row.field_count = target_shape->field_count;
                row.flags |= XG_JSON_CODEC_HAS_OUTPUT_SHAPE | XG_JSON_CODEC_TARGET_OBJECT_SHAPE;
            }
        }
        if (arg0) {
            const ObjectLiteralNode *literal = NULL;
            row.input_shape_id = body_lookup_local_json_shape(bc, arg0, &literal);
            if (row.input_shape_id != XG_NO_ID) {
                const XgObjectShapeSummary *shape =
                    xg_global_evidence_find_object_shape(bc->evidence, row.input_shape_id);
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
                const XgObjectShapeSummary *shape =
                    xg_global_evidence_find_object_shape(bc->evidence, row.input_shape_id);
                row.flags |= XG_JSON_CODEC_HAS_INPUT_SHAPE;
                if (shape)
                    row.field_count = shape->field_count;
            }
        }
        if (body_type_key_has_derive_kind(bc, row.input_type_key, XG_DERIVE_JSON))
            row.flags |= XG_JSON_CODEC_USES_DERIVE;
    } else if (strcmp(method, "stringify") == 0) {
        row.codec_kind = XG_JSON_CODEC_STRINGIFY;
        if (arg0) {
            const ObjectLiteralNode *literal = NULL;
            row.input_shape_id = body_lookup_local_json_shape(bc, arg0, &literal);
            if (row.input_shape_id != XG_NO_ID) {
                const XgObjectShapeSummary *shape =
                    xg_global_evidence_find_object_shape(bc->evidence, row.input_shape_id);
                row.flags |= XG_JSON_CODEC_HAS_INPUT_SHAPE;
                if (shape)
                    row.field_count = shape->field_count;
            }
        }
        if (body_type_key_has_derive_kind(bc, row.input_type_key, XG_DERIVE_JSON))
            row.flags |= XG_JSON_CODEC_USES_DERIVE;
    } else {
        return;
    }

    (void) xg_global_evidence_add_json_codec(bc->evidence, &row);
}

static uint32_t body_struct_object_type_key(const ObjectLiteralNode *obj) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    static const char tag[] = "struct_object";
    h = fold_bytes(h, tag, sizeof(tag) - 1);
    h = fold_u64(h, body_json_shape_hash(obj));
    return hash_folded32(h);
}

static XgObjectShapeId body_lookup_local_object_shape(XgBodyCollect *bc, const AstNode *expr,
                                                      const ObjectLiteralNode **out_literal);
static XgObjectShapeId body_lookup_object_shape(XgBodyCollect *bc, const AstNode *expr,
                                                const ObjectLiteralNode **out_literal);
static XgObjectShapeId body_lookup_or_add_object_shape(XgBodyCollect *bc, const AstNode *expr,
                                                       const ObjectLiteralNode **out_literal);
static XgObjectShapeId body_add_object_shape_for_literal(XgBodyCollect *bc,
                                                         const ObjectLiteralNode *obj,
                                                         uint32_t source_span_id,
                                                         uint32_t type_key);

static int object_shape_key_index(const char **keys, uint32_t count, const char *key) {
    if (!keys || !key)
        return -1;
    for (uint32_t i = 0; i < count; i++) {
        if (keys[i] && strcmp(keys[i], key) == 0)
            return (int) i;
    }
    return -1;
}

static bool object_shape_add_key(const char **keys, uint32_t capacity, uint32_t *count,
                                 const char *key) {
    if (!keys || !count || !key)
        return false;
    if (object_shape_key_index(keys, *count, key) >= 0)
        return true;
    if (*count >= capacity)
        return false;
    keys[(*count)++] = key;
    return true;
}

/* An open literal is one whose key set is only partly known at compile time:
 * `{ name: "ada", [k]: 1 }` contributes a known `name` slot plus an entry whose
 * key is not decided until the expression runs. Passing allow_computed skips
 * those entries instead of abandoning the whole literal, so the static portion
 * can still be described. */
static bool object_shape_count_candidate_keys(XgBodyCollect *bc, const ObjectLiteralNode *obj,
                                              uint32_t *count, bool allow_computed,
                                              uint32_t depth) {
    if (!obj || !count || depth > 16)
        return false;
    for (int i = 0; i < obj->count; i++) {
        if (body_object_literal_entry_is_spread(obj, i)) {
            const ObjectLiteralNode *source_literal = NULL;
            AstNode *spread = obj->values[i];
            if (!spread ||
                body_lookup_or_add_object_shape(bc, spread->as.spread_expr.expr, &source_literal) ==
                    XG_NO_ID ||
                !source_literal)
                return false;
            if (!object_shape_count_candidate_keys(bc, source_literal, count, allow_computed,
                                                   depth + 1))
                return false;
        } else if (!body_object_literal_static_key(obj, i)) {
            if (!allow_computed)
                return false;
        } else {
            (*count)++;
        }
    }
    return true;
}

static bool object_shape_collect_keys(XgBodyCollect *bc, const ObjectLiteralNode *obj,
                                      const char **keys, uint32_t capacity, uint32_t *count,
                                      bool *has_spread, bool allow_computed, bool *has_computed,
                                      uint32_t depth) {
    if (!obj || (!keys && capacity > 0) || !count || depth > 16)
        return false;
    for (int i = 0; i < obj->count; i++) {
        if (body_object_literal_entry_is_spread(obj, i)) {
            const ObjectLiteralNode *source_literal = NULL;
            AstNode *spread = obj->values[i];
            if (has_spread)
                *has_spread = true;
            if (!spread ||
                body_lookup_or_add_object_shape(bc, spread->as.spread_expr.expr, &source_literal) ==
                    XG_NO_ID ||
                !source_literal)
                return false;
            if (!object_shape_collect_keys(bc, source_literal, keys, capacity, count, has_spread,
                                           allow_computed, has_computed, depth + 1))
                return false;
        } else {
            const char *key = body_object_literal_static_key(obj, i);
            if (!key) {
                if (!allow_computed)
                    return false;
                if (has_computed)
                    *has_computed = true;
                continue;
            }
            if (!object_shape_add_key(keys, capacity, count, key))
                return false;
        }
    }
    return true;
}

static int object_shape_key_compare(const void *left_ptr, const void *right_ptr) {
    const char *left = *(const char *const *) left_ptr;
    const char *right = *(const char *const *) right_ptr;
    uint64_t left_key = xg_object_stable_name_key(left ? left : "");
    uint64_t right_key = xg_object_stable_name_key(right ? right : "");
    if (left_key < right_key)
        return -1;
    if (left_key > right_key)
        return 1;
    uint32_t left_name_id = hash_name32(left ? left : "");
    uint32_t right_name_id = hash_name32(right ? right : "");
    return left_name_id < right_name_id ? -1 : left_name_id > right_name_id ? 1 : 0;
}

static void object_shape_canonicalize_keys(const char **keys, uint32_t count) {
    if (keys && count > 1)
        qsort(keys, count, sizeof(*keys), object_shape_key_compare);
}

static bool object_shape_collect_literal_keys(XgBodyCollect *bc, const ObjectLiteralNode *obj,
                                              uint8_t domain, bool allow_computed,
                                              const char ***out_keys, uint32_t *out_count,
                                              bool *out_has_spread, bool *out_has_computed,
                                              uint64_t *out_hash) {
    uint32_t capacity = 0;
    uint32_t count = 0;
    bool has_spread = false;
    bool has_computed = false;
    const char **keys = NULL;
    uint64_t hash = XR_FNV64_OFFSET_BASIS;
    static const char struct_tag[] = "StructObjectShape";
    static const char json_tag[] = "JsonObjectShape";
    static const char open_tag[] = "OpenKeys";
    if (!out_keys || !out_count || !out_has_spread || !out_has_computed || !out_hash)
        return false;
    *out_keys = NULL;
    *out_count = 0;
    *out_has_spread = false;
    *out_has_computed = false;
    *out_hash = 0;
    if (!object_shape_count_candidate_keys(bc, obj, &capacity, allow_computed, 0))
        return false;
    if (capacity > 0) {
        keys = (const char **) xr_calloc((size_t) capacity, sizeof(*keys));
        if (!keys)
            return false;
    }
    if (!object_shape_collect_keys(bc, obj, keys, capacity, &count, &has_spread, allow_computed,
                                   &has_computed, 0)) {
        xr_free(keys);
        return false;
    }
    if (domain == XG_OBJECT_DOMAIN_STRUCT)
        object_shape_canonicalize_keys(keys, count);
    if (domain == XG_OBJECT_DOMAIN_STRUCT)
        hash = fold_bytes(hash, struct_tag, sizeof(struct_tag) - 1);
    else
        hash = fold_bytes(hash, json_tag, sizeof(json_tag) - 1);
    /* An open literal never shares an identity with a closed one that lists the
     * same static keys: the open one carries fields the key list cannot name. */
    if (has_computed)
        hash = fold_bytes(hash, open_tag, sizeof(open_tag) - 1);
    for (uint32_t i = 0; i < count; i++) {
        const char *key = keys[i] ? keys[i] : "";
        hash = fold_bytes(hash, key, strlen(key));
    }
    *out_keys = keys;
    *out_count = count;
    *out_has_spread = has_spread;
    *out_has_computed = has_computed;
    *out_hash = hash;
    return true;
}

static bool object_patch_collect_literal_keys(const ObjectLiteralNode *obj, const char ***out_keys,
                                              uint32_t *out_count, uint64_t *out_hash) {
    uint32_t capacity = 0;
    uint32_t count = 0;
    const char **keys = NULL;
    uint64_t hash = XR_FNV64_OFFSET_BASIS;
    static const char tag[] = "StructObjectPatchShape";
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
        if (!key || !object_shape_add_key(keys, capacity, &count, key)) {
            xr_free(keys);
            return false;
        }
    }
    object_shape_canonicalize_keys(keys, count);
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

static uint16_t object_patch_overwrite_count(const ObjectLiteralNode *source_literal,
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

static void body_add_object_fields_for_keys(XgBodyCollect *bc, XgObjectShapeId shape_id,
                                            const char **keys, uint32_t key_count,
                                            uint32_t base_flags, const ObjectLiteralNode *literal) {
    if (!bc || !bc->evidence || shape_id == XG_NO_ID || (!keys && key_count > 0))
        return;
    for (uint32_t i = 0; i < key_count; i++) {
        XgObjectFieldSummary field;
        if (!keys[i])
            continue;
        memset(&field, 0, sizeof(field));
        field.field_id = (XgObjectFieldId) (bc->evidence->nobject_fields + 1);
        field.shape_id = shape_id;
        field.field_ordinal = (uint16_t) (i < UINT16_MAX ? i : UINT16_MAX);
        field.name_id = hash_name32(keys[i]);
        if (literal && literal->values) {
            int literal_index = body_object_literal_static_field_index(literal, keys[i]);
            if (literal_index >= 0)
                field.type_key = body_expr_type_key(bc, literal->values[literal_index]);
            if (literal_index >= 0)
                field.stable_type_key =
                    body_expr_stable_type_key(bc, literal->values[literal_index]);
        }
        field.flags = base_flags | XG_OBJECT_FIELD_STATIC_KEY;
        field.stable_name_key = xg_object_stable_name_key(keys[i]);
        (void) xg_global_evidence_add_object_field(bc->evidence, &field);
    }
}

static void body_add_object_fields_for_type_alias(XgProducer *producer, XgGlobalEvidence *evidence,
                                                  XgObjectShapeId shape_id,
                                                  const TypeAliasNode *alias) {
    typedef struct ObjectAliasFieldInput {
        const char *name;
        const XrTypeRef *type;
        uint64_t stable_name_key;
        uint32_t name_id;
        uint32_t flags;
    } ObjectAliasFieldInput;
    int field_count = body_type_alias_object_field_count(alias);
    ObjectAliasFieldInput *inputs;
    (void) producer;
    if (!evidence || shape_id == XG_NO_ID || !alias)
        return;
    inputs = field_count > 0
                 ? (ObjectAliasFieldInput *) xr_calloc((size_t) field_count, sizeof(*inputs))
                 : NULL;
    if (field_count > 0 && !inputs)
        return;
    for (int i = 0; i < field_count; i++) {
        inputs[i].name = body_type_alias_object_field_name(alias, i);
        inputs[i].type = body_type_alias_object_field_type(alias, i);
        inputs[i].stable_name_key = xg_object_stable_name_key(inputs[i].name ? inputs[i].name : "");
        inputs[i].name_id = hash_name32(inputs[i].name ? inputs[i].name : "");
        inputs[i].flags = XG_OBJECT_FIELD_STATIC_KEY;
        if (body_type_alias_object_field_optional(alias, i))
            inputs[i].flags |= XG_OBJECT_FIELD_OPTIONAL;
        else
            inputs[i].flags |= XG_OBJECT_FIELD_REQUIRED;
        if (body_type_alias_object_field_readonly(alias, i))
            inputs[i].flags |= XG_OBJECT_FIELD_READONLY;
    }
    for (int i = 1; i < field_count; i++) {
        ObjectAliasFieldInput current = inputs[i];
        int j = i;
        while (j > 0 && (inputs[j - 1].stable_name_key > current.stable_name_key ||
                         (inputs[j - 1].stable_name_key == current.stable_name_key &&
                          inputs[j - 1].name_id > current.name_id))) {
            inputs[j] = inputs[j - 1];
            j--;
        }
        inputs[j] = current;
    }
    for (int i = 0; i < field_count; i++) {
        const char *name = inputs[i].name;
        XgObjectFieldSummary field;
        if (!name)
            continue;
        memset(&field, 0, sizeof(field));
        field.field_id = (XgObjectFieldId) (evidence->nobject_fields + 1);
        field.shape_id = shape_id;
        field.field_ordinal = (uint16_t) (i < UINT16_MAX ? i : UINT16_MAX);
        field.name_id = inputs[i].name_id;
        field.type_key = inputs[i].type ? hash_tref32(inputs[i].type) : 0;
        field.stable_type_key = stable_tref_key(inputs[i].type);
        field.stable_name_key = inputs[i].stable_name_key;
        field.flags = inputs[i].flags;
        (void) xg_global_evidence_add_object_field(evidence, &field);
    }
    xr_free(inputs);
}

static XgObjectShapeSummary *body_find_mutable_object_shape(XgGlobalEvidence *evidence,
                                                            XgObjectShapeId shape_id) {
    if (!evidence || shape_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->nobject_shapes; i++) {
        if (evidence->object_shapes[i].object_shape_id == shape_id)
            return &evidence->object_shapes[i];
    }
    return NULL;
}

static bool body_finalize_object_shape_identity(XgGlobalEvidence *evidence,
                                                XgObjectShapeId shape_id) {
    XgObjectShapeSummary *shape = body_find_mutable_object_shape(evidence, shape_id);
    XgObjectFieldSummary *fields = NULL;
    uint64_t stable_shape_key = 0;
    bool ok = false;

    if (!shape)
        return false;
    if (shape->field_count != 0) {
        fields = (XgObjectFieldSummary *) xr_calloc(shape->field_count, sizeof(*fields));
        if (!fields)
            return false;
        for (uint32_t i = 0; i < evidence->nobject_fields; i++) {
            const XgObjectFieldSummary *field = &evidence->object_fields[i];
            if (field->shape_id != shape_id || field->field_ordinal >= shape->field_count)
                continue;
            fields[field->field_ordinal] = *field;
        }
        for (uint16_t ordinal = 0; ordinal < shape->field_count; ordinal++) {
            if (fields[ordinal].shape_id != shape_id || fields[ordinal].stable_name_key == 0)
                goto done;
        }
    }
    if (!xg_object_shape_stable_key(shape->domain, fields, shape->field_count, &stable_shape_key))
        goto done;
    shape->stable_type_key = shape->type_key ? (uint64_t) shape->type_key : stable_shape_key;
    shape->stable_shape_key = stable_shape_key;
    ok = true;

done:
    xr_free(fields);
    return ok;
}

static XgObjectShapeId body_add_object_patch_shape(XgBodyCollect *bc, uint32_t source_span_id,
                                                   const char **patch_keys,
                                                   uint32_t patch_key_count, uint64_t patch_hash) {
    XgObjectShapeSummary row;
    if (!bc || !bc->evidence)
        return XG_NO_ID;
    memset(&row, 0, sizeof(row));
    row.object_shape_id = (XgObjectShapeId) (bc->evidence->nobject_shapes + 1);
    row.module_id = bc->module_id;
    row.owner_func_id = bc->owner_func_id;
    row.source_span_id = source_span_id;
    row.type_key = hash_folded32(fold_u64(patch_hash, patch_key_count));
    row.field_name_start = (uint32_t) (patch_hash & UINT32_MAX);
    row.field_count = (uint16_t) (patch_key_count < UINT16_MAX ? patch_key_count : UINT16_MAX);
    row.shape_kind = XG_OBJECT_SHAPE_PATCH;
    row.domain = XG_OBJECT_DOMAIN_STRUCT;
    row.provenance = XG_OBJECT_SHAPE_PATCH;
    row.concrete_exact = 1;
    row.fresh = 1;
    row.flags =
        XG_OBJECT_SHAPE_SEALED | XG_OBJECT_SHAPE_STATIC_KEYS | XG_OBJECT_SHAPE_JSON_BRIDGEABLE;
    row.shape_hash = patch_hash;
    row.stable_type_key = row.type_key;
    row.stable_shape_key = patch_hash;
    if (!xg_global_evidence_add_object_shape(bc->evidence, &row))
        return XG_NO_ID;
    body_add_object_fields_for_keys(bc, row.object_shape_id, patch_keys, patch_key_count, 0, NULL);
    if (!body_finalize_object_shape_identity(bc->evidence, row.object_shape_id))
        return XG_NO_ID;
    return row.object_shape_id;
}

static void body_add_object_merge_rows_for_literal(XgBodyCollect *bc, const ObjectLiteralNode *obj,
                                                   XgObjectShapeId result_shape_id,
                                                   uint32_t result_field_count,
                                                   uint64_t result_shape_hash,
                                                   uint32_t source_span_id) {
    const char **patch_keys = NULL;
    uint32_t patch_key_count = 0;
    uint64_t patch_hash = 0;
    XgObjectShapeId patch_shape_id = XG_NO_ID;
    const XgObjectShapeSummary *patch_shape = NULL;
    const XgObjectShapeSummary *result_shape = NULL;
    if (!bc || !bc->evidence || !obj || result_shape_id == XG_NO_ID)
        return;
    if (!object_patch_collect_literal_keys(obj, &patch_keys, &patch_key_count, &patch_hash))
        return;
    result_shape = xg_global_evidence_find_object_shape(bc->evidence, result_shape_id);
    if (!result_shape)
        goto done;
    for (int i = 0; i < obj->count; i++) {
        AstNode *spread;
        const ObjectLiteralNode *source_literal = NULL;
        XgObjectShapeId base_shape_id;
        const XgObjectShapeSummary *base_shape;
        XgObjectMergeSummary row;
        uint16_t overwrites;
        uint64_t merge_hash;
        static const char tag[] = "StructObjectMerge";
        if (!body_object_literal_entry_is_spread(obj, i))
            continue;
        spread = obj->values[i];
        if (!spread)
            continue;
        base_shape_id =
            body_lookup_or_add_object_shape(bc, spread->as.spread_expr.expr, &source_literal);
        if (base_shape_id == XG_NO_ID || !source_literal)
            continue;
        if (patch_shape_id == XG_NO_ID) {
            patch_shape_id = body_add_object_patch_shape(bc, source_span_id, patch_keys,
                                                         patch_key_count, patch_hash);
            patch_shape = xg_global_evidence_find_object_shape(bc->evidence, patch_shape_id);
            if (!patch_shape)
                break;
        }
        base_shape = xg_global_evidence_find_object_shape(bc->evidence, base_shape_id);
        if (!base_shape)
            continue;
        overwrites = object_patch_overwrite_count(source_literal, patch_keys, patch_key_count);
        merge_hash = XR_FNV64_OFFSET_BASIS;
        merge_hash = fold_bytes(merge_hash, tag, sizeof(tag) - 1);
        merge_hash = fold_u64(merge_hash, base_shape->shape_hash);
        merge_hash = fold_u64(merge_hash, patch_hash);
        merge_hash = fold_u64(merge_hash, result_shape_hash);
        merge_hash = fold_u64(merge_hash, overwrites);
        memset(&row, 0, sizeof(row));
        row.merge_id = (XgObjectMergeId) (bc->evidence->nobject_merges + 1);
        row.module_id = bc->module_id;
        row.owner_func_id = bc->owner_func_id;
        row.source_node_id = producer_source_node_id(bc->module_id, spread->as.spread_expr.expr);
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
        row.flags = XG_OBJECT_MERGE_BASE_SHAPE_PROVEN | XG_OBJECT_MERGE_PATCH_SHAPE_PROVEN |
                    XG_OBJECT_MERGE_RESULT_SHAPE_PROVEN;
        if (overwrites != 0)
            row.flags |= XG_OBJECT_MERGE_OVERWRITES;
        row.merge_hash = merge_hash;
        (void) xg_global_evidence_add_object_merge(bc->evidence, &row);
    }

done:
    xr_free(patch_keys);
}

static XgObjectShapeId body_add_object_shape_for_literal_domain(XgBodyCollect *bc,
                                                                const ObjectLiteralNode *obj,
                                                                uint32_t source_span_id,
                                                                uint32_t type_key, uint8_t domain) {
    XgObjectShapeSummary row;
    const char **shape_keys = NULL;
    uint32_t shape_key_count = 0;
    bool has_spread = false;
    bool has_computed = false;
    uint64_t shape_hash = 0;
    uint32_t resolved_type_key;
    uint8_t shape_kind;
    if (!bc || !bc->evidence || !obj ||
        (domain != XG_OBJECT_DOMAIN_STRUCT && domain != XG_OBJECT_DOMAIN_JSON))
        return XG_NO_ID;
    /* A Json spread stays all or nothing. Its entry carries a null key, so it
     * used to be turned away by the same test that turned away computed keys;
     * keep that, because a merge whose source is itself open would hand the
     * object-merge plan a key list that cannot account for every field it
     * copies. */
    if (domain == XG_OBJECT_DOMAIN_JSON) {
        for (int i = 0; i < obj->count; i++) {
            if (body_object_literal_entry_is_spread(obj, i))
                return XG_NO_ID;
        }
    }
    /* A Json literal may otherwise mix decided keys with computed ones. Its
     * static portion is still worth describing: each static key holds the slot
     * its position gives it, which is what lets a field read compare one name
     * at run time instead of searching for it. A struct literal stays all or
     * nothing -- its shape is nominal, so a key it cannot name is a key that
     * disqualifies it. */
    if (!object_shape_collect_literal_keys(bc, obj, domain, domain == XG_OBJECT_DOMAIN_JSON,
                                           &shape_keys, &shape_key_count, &has_spread,
                                           &has_computed, &shape_hash))
        return XG_NO_ID;
    /* Nothing static survived, so there is no slot to guard against and no
     * description to give -- the receiver stays shapeless. */
    if (has_computed && shape_key_count == 0) {
        xr_free(shape_keys);
        return XG_NO_ID;
    }
    resolved_type_key =
        type_key ? type_key
                 : (domain == XG_OBJECT_DOMAIN_JSON ? hash_named_type_key32("Json", NULL, 0)
                                                    : body_struct_object_type_key(obj));
    shape_kind = has_spread ? XG_OBJECT_SHAPE_SPREAD : XG_OBJECT_SHAPE_LITERAL;
    for (uint32_t i = 0; i < bc->evidence->nobject_shapes; i++) {
        const XgObjectShapeSummary *existing = &bc->evidence->object_shapes[i];
        if (existing->module_id == bc->module_id && existing->owner_func_id == bc->owner_func_id &&
            existing->source_span_id == source_span_id && existing->type_key == resolved_type_key &&
            existing->domain == domain && existing->shape_kind == shape_kind &&
            existing->field_count ==
                (uint16_t) (shape_key_count < UINT16_MAX ? shape_key_count : UINT16_MAX) &&
            existing->shape_hash == shape_hash) {
            XgObjectShapeId existing_id = existing->object_shape_id;
            xr_free(shape_keys);
            return existing_id;
        }
    }
    memset(&row, 0, sizeof(row));
    row.object_shape_id = (XgObjectShapeId) (bc->evidence->nobject_shapes + 1);
    row.module_id = bc->module_id;
    row.owner_func_id = bc->owner_func_id;
    row.source_span_id = source_span_id;
    row.type_key = resolved_type_key;
    row.field_name_start = (uint32_t) (shape_hash & UINT32_MAX);
    row.field_count = (uint16_t) (shape_key_count < UINT16_MAX ? shape_key_count : UINT16_MAX);
    row.shape_kind = shape_kind;
    row.domain = domain;
    row.provenance = shape_kind;
    row.concrete_exact = 1;
    row.fresh = 1;
    row.flags = XG_OBJECT_SHAPE_SEALED | XG_OBJECT_SHAPE_STATIC_KEYS;
    if (domain == XG_OBJECT_DOMAIN_STRUCT)
        row.flags |= XG_OBJECT_SHAPE_JSON_BRIDGEABLE;
    else
        row.flags |= XG_OBJECT_SHAPE_JSON_DOMAIN | XG_OBJECT_SHAPE_FRESH | XG_OBJECT_SHAPE_MUTABLE;
    if (has_spread)
        row.flags |= XG_OBJECT_SHAPE_HAS_SPREAD;
    /* The listed keys are a floor, not the whole object: the computed entries
     * add fields whose names are unknown here. Say so, so that a reader takes
     * the guarded path rather than trusting an ordinal outright. OPEN_ROW is
     * not the word for it -- that one marks an open row-typed constraint, and
     * this row describes an allocation. */
    if (has_computed) {
        row.concrete_exact = 0;
        row.flags &= ~(uint32_t) (XG_OBJECT_SHAPE_SEALED | XG_OBJECT_SHAPE_STATIC_KEYS);
        row.flags |= XG_OBJECT_SHAPE_HAS_COMPUTED_KEYS;
    }
    row.shape_hash = shape_hash;
    row.stable_type_key = row.type_key;
    row.stable_shape_key = shape_hash;
    if (!xg_global_evidence_add_object_shape(bc->evidence, &row)) {
        xr_free(shape_keys);
        return XG_NO_ID;
    }
    if (has_spread)
        body_add_object_merge_rows_for_literal(bc, obj, row.object_shape_id, shape_key_count,
                                               shape_hash, source_span_id);
    body_add_object_fields_for_keys(bc, row.object_shape_id, shape_keys, shape_key_count,
                                    XG_OBJECT_FIELD_REQUIRED, obj);
    if (!body_finalize_object_shape_identity(bc->evidence, row.object_shape_id)) {
        xr_free(shape_keys);
        return XG_NO_ID;
    }
    xr_free(shape_keys);
    return row.object_shape_id;
}

static XgObjectShapeId body_add_object_shape_for_literal(XgBodyCollect *bc,
                                                         const ObjectLiteralNode *obj,
                                                         uint32_t source_span_id,
                                                         uint32_t type_key) {
    return body_add_object_shape_for_literal_domain(bc, obj, source_span_id, type_key,
                                                    XG_OBJECT_DOMAIN_STRUCT);
}

static XgObjectShapeId body_add_object_shape_for_type_alias(XgProducer *p, XgModuleId module_id,
                                                            const TypeAliasNode *alias,
                                                            uint32_t source_span_id) {
    XgGlobalEvidence *evidence = p ? p->evidence : NULL;
    XgObjectShapeSummary row;
    const XrTypeRef *object_type;
    uint32_t type_key;
    int field_count = body_type_alias_object_field_count(alias);
    if (!evidence || !alias || !alias->name || field_count <= 0)
        return XG_NO_ID;
    object_type = body_type_alias_object_type_ref(alias);
    type_key = body_type_alias_object_type_key(alias);
    memset(&row, 0, sizeof(row));
    row.object_shape_id = (XgObjectShapeId) (evidence->nobject_shapes + 1);
    row.module_id = module_id;
    row.owner_func_id = XG_NO_ID;
    row.source_span_id = source_span_id;
    row.type_key = type_key;
    row.field_name_start = body_type_alias_field_name_start(alias);
    row.field_count = (uint16_t) (field_count < UINT16_MAX ? field_count : UINT16_MAX);
    row.shape_kind = XG_OBJECT_SHAPE_STATIC;
    row.domain = XG_OBJECT_DOMAIN_STRUCT;
    row.provenance = XG_OBJECT_SHAPE_STATIC;
    row.concrete_exact = object_type && object_type->object_row_mode == XR_OBJECT_ROW_OPEN ? 0 : 1;
    row.flags =
        XG_OBJECT_SHAPE_SEALED | XG_OBJECT_SHAPE_STATIC_KEYS | XG_OBJECT_SHAPE_JSON_BRIDGEABLE;
    if (!row.concrete_exact) {
        row.flags &= ~(uint32_t) XG_OBJECT_SHAPE_SEALED;
        row.flags |= XG_OBJECT_SHAPE_OPEN_ROW;
    }
    row.shape_hash = body_type_alias_object_shape_hash(alias);
    row.stable_type_key = row.type_key;
    row.stable_shape_key = row.shape_hash;
    if (!xg_global_evidence_add_object_shape(evidence, &row))
        return XG_NO_ID;
    body_add_object_fields_for_type_alias(p, evidence, row.object_shape_id, alias);
    if (!body_finalize_object_shape_identity(evidence, row.object_shape_id))
        return XG_NO_ID;
    return row.object_shape_id;
}

static const ObjectLiteralNode *
body_unique_pending_object_return_literal(const XgPendingBody *body) {
    const ObjectLiteralNode *literal = NULL;
    bool seen = false;
    if (!body || body_type_ref_is_json(body_pending_return_type_ref(body)))
        return NULL;
    if (!body_find_unique_return_object_literal(body->body, &literal, &seen))
        return NULL;
    return seen ? literal : NULL;
}

/* A native primitive can hand back an exact structural object even though it
 * has no .xr body to scan: net.__copyBidirectional yields the two-field
 * __CopyBidirectionalResult through the i64_pair_result adapter. Such a shape
 * never reaches the evidence table through the object-literal or type-alias
 * routes, so a field read on the call result would have no proven receiver
 * shape and the AOT backend would reject the whole module. Register the shape
 * here from the analyzed return type, in the canonical field order the
 * structural field-table verifier and the backend shape interner require. The
 * i64_pair_result materializer stores each pair half into the matching
 * canonical slot, so a field read resolves to the slot its value was written
 * to, and this row's stable shape key matches the one the backend interns for
 * the materialized object. The row is keyed by the type's stable identity so
 * repeated calls share one shape. */
static XgObjectShapeId body_add_native_return_object_shape(XgBodyCollect *bc, const XrType *type,
                                                           uint32_t source_span_id) {
    XgObjectShapeSummary row;
    const XgObjectShapeSummary *existing;
    uint32_t type_key;
    int field_count;
    if (!bc || !bc->evidence || !type || type->kind != XR_KIND_STRUCT_OBJECT ||
        type->object.row_mode != XR_OBJECT_ROW_EXACT || type->object.field_count <= 0 ||
        type->object.field_count > UINT16_MAX || !type->object.field_names ||
        !type->object.field_types)
        return XG_NO_ID;
    field_count = type->object.field_count;
    type_key = hash_folded32(xr_type_stable_key(type));
    if (type_key == 0)
        return XG_NO_ID;
    existing = body_find_object_shape_for_type_key(bc, type_key, XG_OBJECT_SHAPE_STATIC);
    if (existing)
        return existing->object_shape_id;
    {
        /* Canonical field order (stable name key, then name id) is what the AOT
         * structural field-table verifier and the backend shape interner both
         * demand, and it is the order the i64_pair_result materializer writes
         * each value into, so a later field read resolves to the slot its value
         * was stored in. */
        typedef struct {
            const XrType *field_type;
            uint64_t stable_name_key;
            uint32_t name_id;
            bool readonly;
        } NativeObjectFieldInput;
        NativeObjectFieldInput *inputs =
            (NativeObjectFieldInput *) xr_calloc((size_t) field_count, sizeof(*inputs));
        if (!inputs)
            return XG_NO_ID;
        for (int i = 0; i < field_count; i++) {
            const char *name = type->object.field_names[i];
            if (!name) {
                xr_free(inputs);
                return XG_NO_ID;
            }
            inputs[i].field_type = type->object.field_types[i];
            inputs[i].stable_name_key = xg_object_stable_name_key(name);
            inputs[i].name_id = hash_name32(name);
            inputs[i].readonly = type->object.field_readonly && type->object.field_readonly[i];
        }
        for (int i = 1; i < field_count; i++) {
            NativeObjectFieldInput current = inputs[i];
            int j = i;
            while (j > 0 && (inputs[j - 1].stable_name_key > current.stable_name_key ||
                             (inputs[j - 1].stable_name_key == current.stable_name_key &&
                              inputs[j - 1].name_id > current.name_id))) {
                inputs[j] = inputs[j - 1];
                j--;
            }
            inputs[j] = current;
        }
        memset(&row, 0, sizeof(row));
        row.object_shape_id = (XgObjectShapeId) (bc->evidence->nobject_shapes + 1);
        row.module_id = bc->module_id;
        row.owner_func_id = XG_NO_ID;
        row.source_span_id = source_span_id;
        row.type_key = type_key;
        row.field_count = (uint16_t) field_count;
        row.shape_kind = XG_OBJECT_SHAPE_STATIC;
        row.domain = XG_OBJECT_DOMAIN_STRUCT;
        row.provenance = XG_OBJECT_SHAPE_STATIC;
        row.concrete_exact = 1;
        row.flags =
            XG_OBJECT_SHAPE_SEALED | XG_OBJECT_SHAPE_STATIC_KEYS | XG_OBJECT_SHAPE_JSON_BRIDGEABLE;
        row.stable_type_key = type_key;
        if (!xg_global_evidence_add_object_shape(bc->evidence, &row)) {
            xr_free(inputs);
            return XG_NO_ID;
        }
        for (int i = 0; i < field_count; i++) {
            XgObjectFieldSummary field;
            memset(&field, 0, sizeof(field));
            field.field_id = (XgObjectFieldId) (bc->evidence->nobject_fields + 1);
            field.shape_id = row.object_shape_id;
            field.field_ordinal = (uint16_t) i;
            field.name_id = inputs[i].name_id;
            field.stable_type_key =
                inputs[i].field_type ? xr_type_stable_key(inputs[i].field_type) : 0;
            field.stable_name_key = inputs[i].stable_name_key;
            field.flags = XG_OBJECT_FIELD_STATIC_KEY | XG_OBJECT_FIELD_REQUIRED;
            if (inputs[i].readonly)
                field.flags |= XG_OBJECT_FIELD_READONLY;
            (void) xg_global_evidence_add_object_field(bc->evidence, &field);
        }
        xr_free(inputs);
    }
    if (!body_finalize_object_shape_identity(bc->evidence, row.object_shape_id))
        return XG_NO_ID;
    return row.object_shape_id;
}

static XgObjectShapeId body_lookup_call_object_return_shape(XgBodyCollect *bc, const AstNode *expr,
                                                            const ObjectLiteralNode **out_literal) {
    const XgPendingBody *body;
    const ObjectLiteralNode *literal;
    const XrTypeRef *return_type;
    uint32_t type_key = 0;
    if (out_literal)
        *out_literal = NULL;
    if (!bc || !expr)
        return XG_NO_ID;
    switch (expr->type) {
        case AST_GROUPING:
            return body_lookup_call_object_return_shape(bc, expr->as.grouping, out_literal);
        case AST_MOVE_EXPR:
            return body_lookup_call_object_return_shape(bc, expr->as.move_expr.expr, out_literal);
        case AST_UNSAFE_EXPR:
            return body_lookup_call_object_return_shape(bc, expr->as.unsafe_expr.operand,
                                                        out_literal);
        case AST_FORCE_UNWRAP:
            return body_lookup_call_object_return_shape(bc, expr->as.unary.operand, out_literal);
        case AST_AS_EXPR:
            return body_lookup_call_object_return_shape(bc, expr->as.as_expr.expr, out_literal);
        case AST_COMPTIME_EXPR:
            return body_lookup_call_object_return_shape(bc, expr->as.comptime_expr.expr,
                                                        out_literal);
        default:
            break;
    }
    if (expr->type != AST_CALL_EXPR)
        return XG_NO_ID;
    body = body_find_call_body(bc, &expr->as.call_expr);
    if (!body) {
        /* No .xr body means a native primitive; take the result shape from the
         * analyzed return type so an exact structural result stays accessible. */
        const XrType *native_type = bc->producer && bc->producer->analyzer
                                        ? xa_analyzer_get_node_type(bc->producer->analyzer, expr)
                                        : NULL;
        return body_add_native_return_object_shape(bc, native_type,
                                                   expr->line > 0 ? (uint32_t) expr->line : 0);
    }
    return_type = body_pending_return_type_ref(body);
    literal = body_unique_pending_object_return_literal(body);
    if (out_literal && literal && !body_object_literal_has_spread(literal))
        *out_literal = literal;
    if (return_type)
        type_key = hash_tref32(return_type);
    if (type_key != 0) {
        const XgObjectShapeSummary *shape =
            body_find_object_shape_for_type_key(bc, type_key, XG_OBJECT_SHAPE_STATIC);
        if (shape)
            return shape->object_shape_id;
    }
    if (!literal)
        return XG_NO_ID;
    return body_add_object_shape_for_literal(bc, literal, (uint32_t) expr->line, type_key);
}

static void body_bind_object_shape_local(XgBodyCollect *bc, const char *name,
                                         XgObjectShapeId shape_id,
                                         const ObjectLiteralNode *literal) {
    XgLocalType *row;
    if (!bc || !name || shape_id == XG_NO_ID)
        return;
    row = body_find_local(bc, name);
    if (!row)
        return;
    row->object_shape_id = shape_id;
    row->object_shape_literal = literal;
}

static void body_bind_static_object_shape_for_type_key(XgBodyCollect *bc, const char *name,
                                                       uint32_t type_key) {
    const XgObjectShapeSummary *object_shape;
    if (!bc || !name || type_key == 0)
        return;
    object_shape = body_find_object_shape_for_type_key(bc, type_key, XG_OBJECT_SHAPE_STATIC);
    /* A structural literal registers its shape as a literal row; a binding
     * keyed by that literal's type key is the same shape. */
    if (!object_shape)
        object_shape = body_find_object_shape_for_type_key(bc, type_key, XG_OBJECT_SHAPE_LITERAL);
    if (!object_shape || object_shape->type_key != type_key ||
        (object_shape->flags & XG_OBJECT_SHAPE_JSON_BRIDGEABLE) == 0)
        return;
    body_bind_object_shape_local(bc, name, object_shape->object_shape_id, NULL);
}

static void body_clear_object_shape_local(XgBodyCollect *bc, const char *name) {
    XgLocalType *row;
    if (!bc || !name)
        return;
    row = body_find_local(bc, name);
    if (!row)
        return;
    row->object_shape_id = XG_NO_ID;
    row->object_shape_literal = NULL;
}

static XgObjectShapeId body_lookup_local_object_shape(XgBodyCollect *bc, const AstNode *expr,
                                                      const ObjectLiteralNode **out_literal) {
    XgLocalType *row;
    if (out_literal)
        *out_literal = NULL;
    if (!bc || !expr)
        return XG_NO_ID;
    switch (expr->type) {
        case AST_GROUPING:
            return body_lookup_local_object_shape(bc, expr->as.grouping, out_literal);
        case AST_MOVE_EXPR:
            return body_lookup_local_object_shape(bc, expr->as.move_expr.expr, out_literal);
        case AST_UNSAFE_EXPR:
            return body_lookup_local_object_shape(bc, expr->as.unsafe_expr.operand, out_literal);
        case AST_FORCE_UNWRAP:
            return body_lookup_local_object_shape(bc, expr->as.unary.operand, out_literal);
        default:
            break;
    }
    if (expr->type != AST_VARIABLE || !expr->as.variable.name)
        return XG_NO_ID;
    row = body_find_local(bc, expr->as.variable.name);
    if (!row || row->object_shape_id == XG_NO_ID)
        return XG_NO_ID;
    if (out_literal)
        *out_literal = row->object_shape_literal;
    return row->object_shape_id;
}

static const AstNode *body_object_receiver_unwrap(const AstNode *expr) {
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

static XgObjectShapeId body_lookup_static_object_shape_for_type_key(XgBodyCollect *bc,
                                                                    uint32_t type_key) {
    const XgObjectShapeSummary *shape =
        body_find_object_shape_for_type_key(bc, type_key, XG_OBJECT_SHAPE_STATIC);
    return shape ? shape->object_shape_id : XG_NO_ID;
}

static XgObjectShapeId body_lookup_sequence_object_shape(XgBodyCollect *bc, const AstNode *expr) {
    XgLocalType *local;
    const AstNode *receiver = body_object_receiver_unwrap(expr);
    if (!bc || !receiver || receiver->type != AST_INDEX_GET)
        return XG_NO_ID;
    local = body_lookup_local_sequence(bc, receiver->as.index_get.array);
    if (!local || local->sequence_elem_type_key == 0)
        return XG_NO_ID;
    return body_lookup_static_object_shape_for_type_key(bc, local->sequence_elem_type_key);
}

static XgObjectShapeId body_lookup_class_field_object_shape(XgBodyCollect *bc,
                                                            const AstNode *expr) {
    const AstNode *receiver = body_object_receiver_unwrap(expr);
    XgClassId receiver_class;
    const XgClassFieldSummary *field;
    if (!bc || !receiver || receiver->type != AST_MEMBER_ACCESS || !receiver->as.member_access.name)
        return XG_NO_ID;
    receiver_class = body_resolve_expr_class(bc, receiver->as.member_access.object);
    field = body_find_class_field_in_hierarchy(bc, receiver_class,
                                               hash_name32(receiver->as.member_access.name));
    if (!field || field->type_key == 0)
        return XG_NO_ID;
    return body_lookup_static_object_shape_for_type_key(bc, field->type_key);
}

static XgObjectShapeId body_lookup_selected_object_field_shape(XgBodyCollect *bc,
                                                               const AstNode *expr) {
    const AstNode *receiver = body_object_receiver_unwrap(expr);
    const char *field_name = NULL;
    XgObjectShapeId receiver_shape_id;
    uint32_t field_name_id;
    if (!bc || !receiver)
        return XG_NO_ID;
    if (receiver->type == AST_MEMBER_ACCESS) {
        field_name = receiver->as.member_access.name;
        receiver = receiver->as.member_access.object;
    } else if (receiver->type == AST_INDEX_GET) {
        field_name = body_static_string_key(receiver->as.index_get.index);
        receiver = receiver->as.index_get.array;
    } else {
        return XG_NO_ID;
    }
    if (!field_name)
        return XG_NO_ID;
    receiver_shape_id = body_lookup_object_shape(bc, receiver, NULL);
    if (receiver_shape_id == XG_NO_ID)
        return XG_NO_ID;
    field_name_id = hash_name32(field_name);
    for (uint32_t i = 0; i < bc->evidence->nobject_fields; i++) {
        const XgObjectFieldSummary *field = &bc->evidence->object_fields[i];
        if (field->shape_id == receiver_shape_id && field->name_id == field_name_id)
            return body_lookup_static_object_shape_for_type_key(bc, field->type_key);
    }
    return XG_NO_ID;
}

static XgObjectShapeId body_lookup_analyzed_object_shape(XgBodyCollect *bc, const AstNode *expr) {
    XrType *type;
    if (!bc || !bc->producer || !bc->producer->analyzer || !bc->evidence || !expr)
        return XG_NO_ID;
    type = xa_analyzer_get_node_type(bc->producer->analyzer, expr);
    if (!XR_TYPE_HAS_OBJECT_SHAPE(type) || type->object.field_count <= 0 ||
        !type->object.field_names || !type->object.field_types)
        return XG_NO_ID;

    for (uint32_t si = 0; si < bc->evidence->nobject_shapes; si++) {
        const XgObjectShapeSummary *shape = &bc->evidence->object_shapes[si];
        bool matches = true;
        if (shape->shape_kind != XG_OBJECT_SHAPE_STATIC ||
            shape->domain != XG_OBJECT_DOMAIN_STRUCT ||
            shape->field_count != (uint16_t) type->object.field_count ||
            shape->concrete_exact != (uint8_t) (type->object.row_mode == XR_OBJECT_ROW_EXACT))
            continue;
        for (int ti = 0; ti < type->object.field_count && matches; ti++) {
            uint32_t name_id = hash_name32(type->object.field_names[ti]);
            uint64_t stable_type_key = xr_type_stable_key(type->object.field_types[ti]);
            bool found = false;
            for (uint32_t fi = 0; fi < bc->evidence->nobject_fields; fi++) {
                const XgObjectFieldSummary *field = &bc->evidence->object_fields[fi];
                if (field->shape_id != shape->object_shape_id || field->name_id != name_id)
                    continue;
                found = field->stable_type_key == stable_type_key;
                if (type->object.field_readonly) {
                    bool readonly = (field->flags & XG_OBJECT_FIELD_READONLY) != 0;
                    found = found && readonly == type->object.field_readonly[ti];
                }
                break;
            }
            matches = found;
        }
        if (matches)
            return shape->object_shape_id;
    }
    return XG_NO_ID;
}

static XgObjectShapeId body_lookup_object_shape(XgBodyCollect *bc, const AstNode *expr,
                                                const ObjectLiteralNode **out_literal) {
    XgObjectShapeId shape_id = body_lookup_local_object_shape(bc, expr, out_literal);
    if (shape_id != XG_NO_ID)
        return shape_id;
    shape_id = body_lookup_call_object_return_shape(bc, expr, out_literal);
    if (shape_id != XG_NO_ID)
        return shape_id;
    shape_id = body_lookup_sequence_object_shape(bc, expr);
    if (shape_id != XG_NO_ID)
        return shape_id;
    shape_id = body_lookup_class_field_object_shape(bc, expr);
    if (shape_id != XG_NO_ID)
        return shape_id;
    shape_id = body_lookup_selected_object_field_shape(bc, expr);
    if (shape_id != XG_NO_ID)
        return shape_id;
    shape_id = body_lookup_analyzed_object_shape(bc, expr);
    if (shape_id != XG_NO_ID)
        return shape_id;

    /* An analyzed expression of an exact structural type carries a stronger
     * identity than syntax-local literal/call provenance. This is required
     * for values constructed by compiler intrinsics such as Json.parse<T>,
     * and for recursively selected object fields (profile.address.geo): the
     * target descriptor is already frozen in the type-alias shape table, so
     * re-derive the shape from the canonical expression type key. */
    return body_lookup_static_object_shape_for_type_key(bc, body_expr_type_key(bc, expr));
}

static XgObjectShapeId body_lookup_or_add_object_shape(XgBodyCollect *bc, const AstNode *expr,
                                                       const ObjectLiteralNode **out_literal) {
    XgObjectShapeId shape_id = body_lookup_object_shape(bc, expr, out_literal);
    if (shape_id != XG_NO_ID)
        return shape_id;

    const ObjectLiteralNode *literal = body_static_object_literal(expr);
    if (!literal)
        return XG_NO_ID;
    shape_id = body_add_object_shape_for_literal(bc, literal,
                                                 expr && expr->line > 0 ? (uint32_t) expr->line : 0,
                                                 body_expr_type_key(bc, expr));
    if (shape_id != XG_NO_ID && out_literal)
        *out_literal = literal;
    return shape_id;
}

static int body_object_shape_static_field_index(XgBodyCollect *bc, XgObjectShapeId shape_id,
                                                const ObjectLiteralNode *literal,
                                                const char *name) {
    uint32_t name_id;
    if (!bc || !bc->evidence || shape_id == XG_NO_ID || !name)
        return -1;
    (void) literal;
    name_id = hash_name32(name);
    if (name_id == 0)
        return -1;
    for (uint32_t i = 0; i < bc->evidence->nobject_fields; i++) {
        const XgObjectFieldSummary *field = &bc->evidence->object_fields[i];
        if (field->shape_id == shape_id && field->name_id == name_id)
            return field->field_ordinal;
    }
    return -1;
}

static void body_add_object_field_access(XgBodyCollect *bc, const AstNode *node, bool mutating) {
    const ObjectLiteralNode *literal = NULL;
    const AstNode *receiver = NULL;
    XgObjectShapeId shape_id;
    const char *name;
    int field_index;
    if (!bc || !node)
        return;
    if (node->type == AST_MEMBER_ACCESS) {
        name = node->as.member_access.name;
        receiver = node->as.member_access.object;
        shape_id = body_lookup_object_shape(bc, receiver, &literal);
    } else if (node->type == AST_MEMBER_SET) {
        name = node->as.member_set.member;
        receiver = node->as.member_set.object;
        shape_id = body_lookup_object_shape(bc, receiver, &literal);
    } else if (node->type == AST_INDEX_GET) {
        name = body_static_string_key(node->as.index_get.index);
        receiver = node->as.index_get.array;
        shape_id = body_lookup_object_shape(bc, receiver, &literal);
    } else if (node->type == AST_INDEX_SET) {
        name = body_static_string_key(node->as.index_set.index);
        receiver = node->as.index_set.array;
        shape_id = body_lookup_object_shape(bc, receiver, &literal);
    } else {
        return;
    }
    if (shape_id == XG_NO_ID || !name)
        return;
    field_index = body_object_shape_static_field_index(bc, shape_id, literal, name);
    if (field_index < 0)
        return;
    receiver = body_object_receiver_unwrap(receiver);
    if (receiver && receiver->type == AST_VARIABLE && receiver->as.variable.name) {
        XgLocalType *local = body_find_local(bc, receiver->as.variable.name);
        if (local && local->param_ordinal != UINT16_MAX &&
            local->object_row_mode == XR_OBJECT_ROW_OPEN) {
            (void) body_add_open_row_object_field_access(
                bc, node, mutating, shape_id, name, (uint16_t) field_index, local->param_ordinal);
            return;
        }
    }
    (void) body_add_resolved_object_field_access(bc, node, mutating, shape_id, name,
                                                 (uint16_t) field_index);
}

static const XgObjectFieldSummary *body_find_object_shape_field(const XgBodyCollect *bc,
                                                                XgObjectShapeId shape_id,
                                                                uint16_t field_ordinal) {
    if (!bc || !bc->evidence || shape_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < bc->evidence->nobject_fields; i++) {
        const XgObjectFieldSummary *field = &bc->evidence->object_fields[i];
        if (field->shape_id == shape_id && field->field_ordinal == field_ordinal)
            return field;
    }
    return NULL;
}

static void body_push_destructure_pattern_locals(XgBodyCollect *bc,
                                                 const XrDestructurePattern *pattern,
                                                 uint32_t type_key) {
    if (!bc || !pattern)
        return;
    switch (pattern->type) {
        case PATTERN_IDENTIFIER:
            (void) body_push_local(bc, pattern->as.identifier.name,
                                   pattern->as.identifier.symbol_id, XG_NO_ID, XG_NO_ID, type_key,
                                   NULL, type_key != 0);
            /* A binding whose field is itself an object shape must carry that
             * shape, exactly as a var declaration does, or member accesses on
             * the bound name lose their verified access rows. */
            body_bind_static_object_shape_for_type_key(bc, pattern->as.identifier.name, type_key);
            break;
        case PATTERN_ARRAY:
        case PATTERN_TUPLE:
            for (int i = 0; i < pattern->as.array.element_count; i++)
                body_push_destructure_pattern_locals(bc, pattern->as.array.elements[i], 0);
            break;
        case PATTERN_OBJECT:
            for (int i = 0; i < pattern->as.object.field_count; i++)
                body_push_destructure_pattern_locals(bc, pattern->as.object.patterns[i], 0);
            break;
        case PATTERN_SKIP:
            break;
        default:
            break;
    }
}

static void body_add_object_destructure_accesses(XgBodyCollect *bc,
                                                 const XrDestructurePattern *pattern,
                                                 const AstNode *source, uint32_t source_span_id,
                                                 bool declare_locals) {
    const ObjectLiteralNode *literal = NULL;
    XgObjectShapeId shape_id;
    if (!bc || !pattern || !source)
        return;
    if (pattern->type != PATTERN_OBJECT) {
        if (declare_locals)
            body_push_destructure_pattern_locals(bc, pattern, 0);
        return;
    }

    shape_id = body_lookup_object_shape(bc, source, &literal);
    if (shape_id == XG_NO_ID) {
        literal = body_static_object_literal(source);
        if (literal)
            shape_id = body_add_object_shape_for_literal(bc, literal, source_span_id,
                                                         body_expr_type_key(bc, source));
    }
    if (shape_id == XG_NO_ID) {
        if (declare_locals)
            body_push_destructure_pattern_locals(bc, pattern, 0);
        return;
    }

    for (int i = 0; i < pattern->as.object.field_count; i++) {
        const char *name = pattern->as.object.field_names[i];
        const XrDestructurePattern *binding = pattern->as.object.patterns[i];
        int field_index = body_object_shape_static_field_index(bc, shape_id, literal, name);
        uint32_t result_type_key = 0;
        if (!name || field_index < 0 || field_index > UINT16_MAX) {
            if (declare_locals)
                body_push_destructure_pattern_locals(bc, binding, 0);
            continue;
        }
        const XgObjectFieldSummary *field =
            body_find_object_shape_field(bc, shape_id, (uint16_t) field_index);
        if (field)
            result_type_key = field->type_key;

        XgObjectAccessSummary row;
        memset(&row, 0, sizeof(row));
        row.object_access_id = (XgObjectAccessId) (bc->evidence->nobject_accesses + 1);
        row.module_id = bc->module_id;
        row.owner_func_id = bc->owner_func_id;
        row.receiver_shape_id = shape_id;
        row.source_span_id = source_span_id;
        row.field_name_id = hash_name32(name);
        row.result_type_key = result_type_key;
        row.field_ordinal = (uint16_t) field_index;
        row.access_kind = XG_OBJECT_ACCESS_DESTRUCTURE;
        const XgObjectShapeSummary *receiver_shape =
            xg_global_evidence_find_object_shape(bc->evidence, shape_id);
        row.domain = receiver_shape ? receiver_shape->domain : 0;
        row.syntax = XG_OBJECT_ACCESS_SYNTAX_DESTRUCTURE;
        row.flags = XG_OBJECT_ACCESS_STATIC_FIELD | XG_OBJECT_ACCESS_RECEIVER_SHAPE_PROVEN;
        if (receiver_shape)
            (void) body_add_object_access_with_case(bc, &row, receiver_shape);

        if (declare_locals)
            body_push_destructure_pattern_locals(bc, binding, result_type_key);
    }
}

/* Match object patterns read subject fields exactly like a destructure
 * statement, so they need the same verified access rows. Rows are keyed by
 * the pattern's span; sibling arms that test the same field of the same
 * receiver shape share one row instead of tripping the binder's ambiguity
 * guard. Nested sub-patterns have no receiver expression of their own and
 * keep the dynamic path. */
static void body_add_match_object_pattern_accesses(XgBodyCollect *bc, const AstNode *pattern,
                                                   const AstNode *subject) {
    if (!bc || !pattern || !subject || pattern->type != AST_PATTERN_OBJECT)
        return;
    const PatternObjectNode *op = &pattern->as.pattern_object;
    const ObjectLiteralNode *literal = NULL;
    XgObjectShapeId shape_id = body_lookup_object_shape(bc, subject, &literal);
    if (shape_id == XG_NO_ID) {
        literal = body_static_object_literal(subject);
        if (literal)
            shape_id = body_add_object_shape_for_literal(bc, literal, (uint32_t) pattern->line,
                                                         body_expr_type_key(bc, subject));
    }
    if (shape_id == XG_NO_ID)
        return;
    for (int i = 0; i < op->count; i++) {
        const char *name = op->field_names ? op->field_names[i] : NULL;
        int field_index = body_object_shape_static_field_index(bc, shape_id, literal, name);
        if (!name || field_index < 0 || field_index > UINT16_MAX)
            continue;
        uint32_t field_name_id = hash_name32(name);
        bool already_recorded = false;
        for (uint32_t r = 0; r < bc->evidence->nobject_accesses; r++) {
            const XgObjectAccessSummary *existing = &bc->evidence->object_accesses[r];
            if (existing->owner_func_id == bc->owner_func_id &&
                existing->module_id == bc->module_id &&
                existing->source_span_id == (uint32_t) pattern->line &&
                existing->field_name_id == field_name_id &&
                existing->access_kind == XG_OBJECT_ACCESS_DESTRUCTURE &&
                existing->receiver_shape_id == shape_id) {
                already_recorded = true;
                break;
            }
        }
        if (already_recorded)
            continue;
        uint32_t result_type_key = 0;
        const XgObjectFieldSummary *field =
            body_find_object_shape_field(bc, shape_id, (uint16_t) field_index);
        if (field)
            result_type_key = field->type_key;
        XgObjectAccessSummary row;
        memset(&row, 0, sizeof(row));
        row.object_access_id = (XgObjectAccessId) (bc->evidence->nobject_accesses + 1);
        row.module_id = bc->module_id;
        row.owner_func_id = bc->owner_func_id;
        row.receiver_shape_id = shape_id;
        row.source_span_id = (uint32_t) pattern->line;
        row.field_name_id = field_name_id;
        row.result_type_key = result_type_key;
        row.field_ordinal = (uint16_t) field_index;
        row.access_kind = XG_OBJECT_ACCESS_DESTRUCTURE;
        const XgObjectShapeSummary *receiver_shape =
            xg_global_evidence_find_object_shape(bc->evidence, shape_id);
        row.domain = receiver_shape ? receiver_shape->domain : 0;
        row.syntax = XG_OBJECT_ACCESS_SYNTAX_DESTRUCTURE;
        row.flags = XG_OBJECT_ACCESS_STATIC_FIELD | XG_OBJECT_ACCESS_RECEIVER_SHAPE_PROVEN;
        if (receiver_shape)
            (void) body_add_object_access_with_case(bc, &row, receiver_shape);
    }
}

static uint32_t body_const_expr_id(XgBodyCollect *bc, const AstNode *expr) {
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
        case AST_LITERAL_RUNE:
            h = fold_u64(h, expr->as.literal.raw_value.rune_val);
            return hash_folded32(h);
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
            h = fold_u64(h, expr->as.literal.raw_value.bool_val ? 1 : 0);
            return hash_folded32(h);
        case AST_LITERAL_NULL:
            h = fold_u64(h, 0);
            return hash_folded32(h);
        case AST_ENUM_ACCESS:
        case AST_MEMBER_ACCESS: {
            const char *enum_name = NULL;
            const char *member_name = NULL;
            if (!body_expr_enum_access_parts(bc, expr, &enum_name, &member_name))
                return 0;
            h = fold_bytes(h, enum_name, strlen(enum_name));
            h = fold_bytes(h, member_name, strlen(member_name));
            return hash_folded32(h);
        }
        case AST_GROUPING:
            return body_const_expr_id(bc, expr->as.grouping);
        case AST_COMPTIME_EXPR:
            return body_const_expr_id(bc, expr->as.comptime_expr.expr);
        case AST_MOVE_EXPR:
            return body_const_expr_id(bc, expr->as.move_expr.expr);
        case AST_UNSAFE_EXPR:
            return body_const_expr_id(bc, expr->as.unsafe_expr.operand);
        case AST_FORCE_UNWRAP:
            return body_const_expr_id(bc, expr->as.unary.operand);
        default:
            return 0;
    }
}

static uint32_t body_map_runtime_hash_f64(double value) {
    return (uint32_t) xr_hash_core_mix_u64(xr_hash_core_f64_key_bits(value));
}

static bool body_map_key_type_is_float(uint32_t key_type_key) {
    static const uint8_t float_widths[] = {XR_NATIVE_F32, XR_NATIVE_F64};
    if (key_type_key == hash_synthetic_tref32(XR_TREF_FLOAT, NULL, NULL, 0))
        return true;
    for (uint32_t i = 0; i < sizeof(float_widths) / sizeof(float_widths[0]); i++) {
        if (key_type_key == hash_synthetic_width_tref32(XR_TREF_FLOAT_WIDTH, float_widths[i]))
            return true;
    }
    return false;
}

/* The precomputed hash must be the hash of the key the program actually stores.
 * The container's key type decides that: an integer literal keying a float map
 * is the float it converts to, so hashing its integer form would place the
 * entry in a slot no lookup of that map ever probes. */
static uint64_t body_map_const_prehash(const AstNode *expr, uint32_t key_type_key) {
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
            if (body_map_key_type_is_float(key_type_key))
                return body_map_runtime_hash_f64((double) (int64_t) expr->as.literal.int_bits);
            return (uint32_t) xr_hash_core_mix_u64((uint64_t) expr->as.literal.int_bits);
        case AST_LITERAL_FLOAT:
            return body_map_runtime_hash_f64(expr->as.literal.raw_value.float_val);
        case AST_LITERAL_RUNE:
            return (uint32_t) xr_hash_core_mix_u64((uint64_t) expr->as.literal.raw_value.rune_val);
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
        XR_NATIVE_I64, XR_NATIVE_I8,  XR_NATIVE_I16, XR_NATIVE_I32,   XR_NATIVE_U8,
        XR_NATIVE_U16, XR_NATIVE_U32, XR_NATIVE_U64, XR_NATIVE_ISIZE, XR_NATIVE_USIZE,
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

static const XgEnumNameRow *body_enum_row_for_name(XgBodyCollect *bc, const char *name) {
    if (!bc || !bc->producer || !name || !name[0])
        return NULL;
    return producer_lookup_enum_row_scoped(bc->producer, bc->module_id, hash_name32(name), true);
}

static bool body_enum_decl_has_ordinal_domain(const EnumDeclNode *decl) {
    if (!decl || decl->type_param_count != 0 || decl->member_count <= 0 ||
        decl->member_count > XG_DENSE_ENUM_MEMBER_MAX)
        return false;
    for (int i = 0; i < decl->member_count; i++) {
        const AstNode *member = decl->members ? decl->members[i] : NULL;
        const EnumMemberNode *variant;
        if (!member || member->type != AST_ENUM_MEMBER)
            return false;
        variant = &member->as.enum_member;
        if (!variant->name || variant->payload_count != 0)
            return false;
        for (int j = i + 1; j < decl->member_count; j++) {
            const AstNode *other = decl->members ? decl->members[j] : NULL;
            const EnumMemberNode *other_variant;
            if (!other || other->type != AST_ENUM_MEMBER)
                return false;
            other_variant = &other->as.enum_member;
            if (other_variant->name && strcmp(variant->name, other_variant->name) == 0)
                return false;
        }
    }
    return true;
}

static bool body_expr_enum_access_parts(XgBodyCollect *bc, const AstNode *expr,
                                        const char **out_enum_name, const char **out_member_name) {
    const char *enum_name = NULL;
    const char *member_name = NULL;
    if (out_enum_name)
        *out_enum_name = NULL;
    if (out_member_name)
        *out_member_name = NULL;
    if (!expr)
        return false;
    switch (expr->type) {
        case AST_ENUM_ACCESS:
            enum_name = expr->as.enum_access.enum_name;
            member_name = expr->as.enum_access.member_name;
            break;
        case AST_MEMBER_ACCESS:
            if (expr->as.member_access.object &&
                expr->as.member_access.object->type == AST_VARIABLE) {
                enum_name = expr->as.member_access.object->as.variable.name;
                member_name = expr->as.member_access.name;
            }
            break;
        case AST_GROUPING:
            return body_expr_enum_access_parts(bc, expr->as.grouping, out_enum_name,
                                               out_member_name);
        case AST_COMPTIME_EXPR:
            return body_expr_enum_access_parts(bc, expr->as.comptime_expr.expr, out_enum_name,
                                               out_member_name);
        case AST_MOVE_EXPR:
            return body_expr_enum_access_parts(bc, expr->as.move_expr.expr, out_enum_name,
                                               out_member_name);
        case AST_UNSAFE_EXPR:
            return body_expr_enum_access_parts(bc, expr->as.unsafe_expr.operand, out_enum_name,
                                               out_member_name);
        case AST_FORCE_UNWRAP:
            return body_expr_enum_access_parts(bc, expr->as.unary.operand, out_enum_name,
                                               out_member_name);
        default:
            return false;
    }
    if (!enum_name || !member_name || !body_enum_row_for_name(bc, enum_name))
        return false;
    if (out_enum_name)
        *out_enum_name = enum_name;
    if (out_member_name)
        *out_member_name = member_name;
    return true;
}

static bool body_enum_member_ordinal(XgBodyCollect *bc, const char *enum_name,
                                     const char *member_name, uint32_t *out_ordinal) {
    const XgEnumNameRow *row = body_enum_row_for_name(bc, enum_name);
    const EnumDeclNode *decl = row ? row->decl : NULL;
    if (out_ordinal)
        *out_ordinal = 0;
    if (!body_enum_decl_has_ordinal_domain(decl) || !member_name)
        return false;
    for (int i = 0; i < decl->member_count; i++) {
        const AstNode *member = decl->members ? decl->members[i] : NULL;
        const EnumMemberNode *variant;
        if (!member || member->type != AST_ENUM_MEMBER)
            return false;
        variant = &member->as.enum_member;
        if (variant->name && strcmp(variant->name, member_name) == 0) {
            if (out_ordinal)
                *out_ordinal = (uint32_t) i;
            return true;
        }
    }
    return false;
}

static bool body_map_literal_has_dense_enum_domain(XgBodyCollect *bc, AstNode **keys, int count,
                                                   uint32_t key_type_key) {
    const char *enum_name = NULL;
    const XgEnumNameRow *row = NULL;
    const EnumDeclNode *decl = NULL;
    if (!bc || !keys || count <= 0 || count > XG_DENSE_ENUM_MEMBER_MAX || key_type_key == 0)
        return false;
    for (int i = 0; i < count; i++) {
        const char *key_enum = NULL;
        const char *member_name = NULL;
        uint32_t ordinal = 0;
        if (!body_expr_enum_access_parts(bc, keys[i], &key_enum, &member_name))
            return false;
        if (!enum_name) {
            enum_name = key_enum;
            row = body_enum_row_for_name(bc, enum_name);
            decl = row ? row->decl : NULL;
            if (!row || !body_enum_decl_has_ordinal_domain(decl) || key_type_key != row->type_key ||
                count != decl->member_count)
                return false;
        } else if (!key_enum || strcmp(enum_name, key_enum) != 0) {
            return false;
        }
        if (!body_enum_member_ordinal(bc, key_enum, member_name, &ordinal) ||
            ordinal != (uint32_t) i)
            return false;
    }
    return true;
}

static bool body_map_value_type_supports_bool_direct(uint32_t value_type_key) {
    return value_type_key == hash_synthetic_tref32(XR_TREF_INT, NULL, NULL, 0) ||
           value_type_key == hash_synthetic_width_tref32(XR_TREF_INT_WIDTH, XR_NATIVE_I64) ||
           value_type_key == hash_synthetic_width_tref32(XR_TREF_FLOAT_WIDTH, XR_NATIVE_F32);
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
        XR_TREF_INT, XR_TREF_FLOAT, XR_TREF_STRING, XR_TREF_RUNE, XR_TREF_BOOL,
    };
    static const uint8_t int_widths[] = {
        XR_NATIVE_I64, XR_NATIVE_I8,  XR_NATIVE_I16, XR_NATIVE_I32,   XR_NATIVE_U8,
        XR_NATIVE_U16, XR_NATIVE_U32, XR_NATIVE_U64, XR_NATIVE_ISIZE, XR_NATIVE_USIZE,
    };
    static const uint8_t float_widths[] = {
        XR_NATIVE_F64,
        XR_NATIVE_F32,
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

static void body_ensure_enum_hash_eq(XgBodyCollect *bc, uint32_t key_type_key) {
    XgHashEqSummary row;
    if (!bc || !bc->evidence || key_type_key == 0)
        return;
    if (xg_global_evidence_find_hash_eq(bc->evidence, key_type_key))
        return;
    for (uint32_t i = 0; i < bc->evidence->ndecls; i++) {
        const XgDeclSummary *decl = &bc->evidence->decls[i];
        if (decl->kind == XG_DECL_ENUM && decl->type_key == key_type_key) {
            memset(&row, 0, sizeof(row));
            row.hash_eq_id = (XgHashEqId) (bc->evidence->nhash_eqs + 1);
            row.type_key = key_type_key;
            row.kind = XG_HASH_EQ_ENUM_ORDINAL;
            row.flags =
                XG_HASH_EQ_NO_ALLOC | XG_HASH_EQ_NO_THROW | XG_HASH_EQ_PURE | XG_HASH_EQ_FINAL;
            (void) xg_global_evidence_add_hash_eq(bc->evidence, &row);
            return;
        }
    }
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
           body_type_ref_is_named(
               method->params && method->params[0] ? method->params[0]->type : NULL, type_name) &&
           body_type_ref_is_bool(method->return_type);
}

static const XgDeriveSummary *body_find_derive_by_type_kind(XgBodyCollect *bc, uint32_t type_key,
                                                            XgDeclId owner_decl_id, uint8_t kind) {
    if (!bc || !bc->evidence || type_key == 0)
        return NULL;
    for (uint32_t i = 0; i < bc->evidence->nderives; i++) {
        const XgDeriveSummary *derive = &bc->evidence->derives[i];
        if (derive->type_key == type_key && derive->owner_decl_id == owner_decl_id &&
            derive->derive_kind == kind)
            return derive;
    }
    return NULL;
}

static bool body_derive_fields_match(XgBodyCollect *bc, const XgDeriveSummary *eq,
                                     const XgDeriveSummary *hash) {
    if (!bc || !bc->evidence || !eq || !hash || eq->field_count != hash->field_count)
        return false;
    if (eq->field_count == 0)
        return eq->field_start == 0 && hash->field_start == 0;
    if (eq->field_start == 0 || hash->field_start == 0)
        return false;
    uint32_t eq_end = eq->field_start + (uint32_t) eq->field_count - 1;
    uint32_t hash_end = hash->field_start + (uint32_t) hash->field_count - 1;
    if (eq_end < eq->field_start || hash_end < hash->field_start ||
        eq_end > bc->evidence->nderived_fields || hash_end > bc->evidence->nderived_fields)
        return false;
    for (uint32_t i = 0; i < eq->field_count; i++) {
        const XgDerivedFieldSummary *eq_field =
            &bc->evidence->derived_fields[eq->field_start - 1 + i];
        const XgDerivedFieldSummary *hash_field =
            &bc->evidence->derived_fields[hash->field_start - 1 + i];
        if (eq_field->field_ordinal != hash_field->field_ordinal ||
            eq_field->name_id != hash_field->name_id ||
            eq_field->type_key != hash_field->type_key ||
            eq_field->source_field_id != hash_field->source_field_id ||
            eq_field->flags != hash_field->flags)
            return false;
    }
    return true;
}

static void body_ensure_hash_eq_depth(XgBodyCollect *bc, uint32_t key_type_key, uint32_t depth);

static bool body_hash_eq_row_consumable(const XgHashEqSummary *hash_eq) {
    if (!hash_eq)
        return false;
    switch ((XgHashEqKind) hash_eq->kind) {
        case XG_HASH_EQ_BUILTIN:
        case XG_HASH_EQ_ENUM_ORDINAL:
        case XG_HASH_EQ_DERIVE:
        case XG_HASH_EQ_USER_METHOD:
            return true;
        default:
            return false;
    }
}

static bool body_derived_hash_eq_fields_supported(XgBodyCollect *bc, uint32_t owner_type_key,
                                                  const XgDeriveSummary *eq,
                                                  const XgDeriveSummary *hash, uint32_t depth) {
    if (!bc || !bc->evidence || !body_derive_fields_match(bc, eq, hash))
        return false;
    if (!eq || eq->field_count == 0)
        return true;
    if (depth >= 8)
        return false;
    for (uint32_t i = 0; i < eq->field_count; i++) {
        const XgDerivedFieldSummary *field = &bc->evidence->derived_fields[eq->field_start - 1 + i];
        if (field->type_key == 0 || field->type_key == owner_type_key)
            return false;
        body_ensure_hash_eq_depth(bc, field->type_key, depth + 1);
        if (!body_hash_eq_row_consumable(
                xg_global_evidence_find_hash_eq(bc->evidence, field->type_key)))
            return false;
    }
    return true;
}

static void body_ensure_derived_hash_eq_depth(XgBodyCollect *bc, uint32_t key_type_key,
                                              uint32_t depth) {
    const char *type_name = NULL;
    const XgClassSummary *cls = body_find_class_by_type_key(bc, key_type_key, &type_name);
    XgHashEqSummary row;
    if (!bc || !bc->evidence || !cls || !type_name)
        return;
    if (depth >= 8)
        return;
    if (xg_global_evidence_find_hash_eq(bc->evidence, key_type_key))
        return;
    const XgDeriveSummary *eq =
        body_find_derive_by_type_kind(bc, key_type_key, cls->decl_id, XG_DERIVE_EQ);
    const XgDeriveSummary *hash =
        body_find_derive_by_type_kind(bc, key_type_key, cls->decl_id, XG_DERIVE_HASH);
    if (!eq || !hash || eq->type_key != hash->type_key || !body_derive_fields_match(bc, eq, hash))
        return;
    if (!body_derived_hash_eq_fields_supported(bc, key_type_key, eq, hash, depth))
        return;
    memset(&row, 0, sizeof(row));
    row.hash_eq_id = (XgHashEqId) (bc->evidence->nhash_eqs + 1);
    row.type_key = key_type_key;
    row.kind = XG_HASH_EQ_DERIVE;
    row.eq_derive_id = eq->derive_id;
    row.hash_derive_id = hash->derive_id;
    row.flags = XG_HASH_EQ_NO_ALLOC | XG_HASH_EQ_NO_THROW | XG_HASH_EQ_PURE;
    if ((cls->flags & (XG_CLASS_EXPLICIT_FINAL | XG_CLASS_INFERRED_FINAL)) != 0)
        row.flags |= XG_HASH_EQ_FINAL;
    (void) xg_global_evidence_add_hash_eq(bc->evidence, &row);
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

static void body_ensure_hash_eq_depth(XgBodyCollect *bc, uint32_t key_type_key, uint32_t depth) {
    body_ensure_builtin_hash_eq(bc, key_type_key);
    body_ensure_enum_hash_eq(bc, key_type_key);
    body_ensure_derived_hash_eq_depth(bc, key_type_key, depth);
    body_ensure_user_hash_eq(bc, key_type_key);
}

static void body_ensure_hash_eq(XgBodyCollect *bc, uint32_t key_type_key) {
    body_ensure_hash_eq_depth(bc, key_type_key, 0);
}

static bool body_owner_is_module_init(const XgBodyCollect *bc) {
    return bc && bc->body_kind == XG_BODY_MODULE_INIT;
}

static XgMapShapeId body_add_map_shape_for_literal(
    XgBodyCollect *bc, const AstNode *node, const XrTypeRef *type_annotation,
    uint32_t source_span_id, bool readonly_static_candidate, uint32_t *out_receiver_type_key,
    uint32_t *out_key_type_key, uint32_t *out_value_type_key, uint8_t *out_container_kind) {
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
    bool dense_enum_domain = false;
    bool all_entries_const = true;
    bool all_keys_have_static_prehash = true;
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
        const_ids[i] = body_const_expr_id(bc, keys ? keys[i] : NULL);
        if (const_ids[i] == 0 || (values && body_const_expr_id(bc, values[i]) == 0))
            all_entries_const = false;
        if (body_map_const_prehash(keys ? keys[i] : NULL, key_type_key) == 0)
            all_keys_have_static_prehash = false;
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
    if (readonly_static_candidate && count > 0 && all_entries_const &&
        all_keys_have_static_prehash) {
        shape.source = XG_MAP_SHAPE_SRC_STATIC;
    } else {
        shape.source = XG_MAP_SHAPE_SRC_LITERAL;
    }
    shape.key_type_key = key_type_key;
    shape.value_type_key = container_kind == XG_MAP_CONTAINER_SET ? 0 : value_type_key;
    shape.entry_start = count > 0 ? (uint32_t) (bc->evidence->nmap_entries + 1) : 0;
    shape.entry_count = (uint16_t) (count < UINT16_MAX ? count : UINT16_MAX);
    shape.literal_count = (uint32_t) (count > 0 ? count : 0);
    shape.flags = XG_MAP_SHAPE_LITERAL;
    if (shape.source == XG_MAP_SHAPE_SRC_STATIC)
        shape.flags |= XG_MAP_SHAPE_STATIC | XG_MAP_SHAPE_READONLY;
    if (count > 0 && count <= XG_SMALL_MAP_LITERAL_MAX)
        shape.flags |= XG_MAP_SHAPE_SMALL;
    if (container_kind == XG_MAP_CONTAINER_MAP &&
        body_map_value_type_supports_bool_direct(value_type_key) &&
        body_map_literal_has_bool_domain(keys, count, key_type_key))
        shape.flags |= XG_MAP_SHAPE_BOOL_DIRECT;
    dense_enum_domain = body_map_literal_has_dense_enum_domain(bc, keys, count, key_type_key);
    if (dense_enum_domain)
        shape.flags |= XG_MAP_SHAPE_DENSE_ENUM;
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
        uint32_t value_const_id = values ? body_const_expr_id(bc, values[i]) : 0;
        memset(&entry, 0, sizeof(entry));
        entry.entry_id = (XgMapEntryId) (bc->evidence->nmap_entries + 1);
        entry.shape_id = shape.shape_id;
        entry.entry_ordinal = (uint32_t) i;
        entry.key_const_id = key_const_id;
        entry.value_const_id = value_const_id;
        entry.prehash = body_map_const_prehash(keys[i], key_type_key);
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
        if (dense_enum_domain) {
            const char *enum_name = NULL;
            const char *member_name = NULL;
            uint32_t ordinal = 0;
            if (body_expr_enum_access_parts(bc, keys[i], &enum_name, &member_name) &&
                body_enum_member_ordinal(bc, enum_name, member_name, &ordinal)) {
                entry.key_i64 = (int64_t) ordinal;
                entry.flags |= XG_MAP_ENTRY_ENUM_KEY;
            }
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
    if (dense_enum_domain)
        body_ensure_enum_hash_eq(bc, key_type_key);
    else
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
    row->sequence_elem_interface_id =
        producer_lookup_interface_from_tref(bc->producer, elem_type_ref);
    row->sequence_elem_class_id = producer_lookup_class_from_tref(bc->producer, elem_type_ref);
    row->sequence_storage_id = 0;
    row->sequence_elem_managed_ref = body_type_ref_is_managed_storage_ref(elem_type_ref);
    row->sequence_elem_object_shape_id = XG_NO_ID;
    row->sequence_elem_object_shape_literal = NULL;
    row->sequence_elem_map_container_kind = 0;
    row->sequence_elem_map_key_type_key = 0;
    row->sequence_elem_map_value_type_key = 0;
    row->sequence_fresh_empty = false;
    if (body_type_ref_map_parts(elem_type_ref, &row->sequence_elem_map_container_kind,
                                &row->sequence_elem_map_key_type_key,
                                &row->sequence_elem_map_value_type_key) &&
        row->sequence_elem_map_container_kind == XG_MAP_CONTAINER_SET)
        row->sequence_elem_map_value_type_key = 0;
}

static void body_bind_sequence_json_shape_local(XgBodyCollect *bc, const char *name,
                                                XgObjectShapeId shape_id,
                                                const ObjectLiteralNode *literal) {
    XgLocalType *row;
    if (!bc || !name || shape_id == XG_NO_ID)
        return;
    row = body_find_local(bc, name);
    if (!row || row->sequence_kind == 0)
        return;
    row->sequence_elem_object_shape_id = shape_id;
    row->sequence_elem_object_shape_literal = literal;
}

static void body_bind_sequence_local_from_source(XgBodyCollect *bc, const char *name,
                                                 const XgLocalType *source) {
    XgLocalType *row;
    if (!bc || !name || !source || source->sequence_kind == 0)
        return;
    row = body_find_local(bc, name);
    if (!row)
        return;
    row->sequence_kind = source->sequence_kind;
    row->sequence_elem_type_key = source->sequence_elem_type_key;
    row->sequence_elem_interface_id = source->sequence_elem_interface_id;
    row->sequence_elem_class_id = source->sequence_elem_class_id;
    row->sequence_storage_id = source->sequence_storage_id;
    row->sequence_elem_managed_ref = source->sequence_elem_managed_ref;
    row->sequence_elem_object_shape_id = source->sequence_elem_object_shape_id;
    row->sequence_elem_object_shape_literal = source->sequence_elem_object_shape_literal;
    row->sequence_elem_map_container_kind = source->sequence_elem_map_container_kind;
    row->sequence_elem_map_key_type_key = source->sequence_elem_map_key_type_key;
    row->sequence_elem_map_value_type_key = source->sequence_elem_map_value_type_key;
    row->sequence_fresh_empty = source->sequence_fresh_empty;
}

static void body_inherit_sequence_source_metadata(XgBodyCollect *bc, const char *name,
                                                  const XgLocalType *source) {
    XgLocalType *row;
    if (!bc || !name || !source || source->sequence_kind == 0)
        return;
    row = body_find_local(bc, name);
    if (!row || row->sequence_kind == 0)
        return;
    if (row->sequence_elem_type_key == 0)
        row->sequence_elem_type_key = source->sequence_elem_type_key;
    if (row->sequence_elem_interface_id == XG_NO_ID)
        row->sequence_elem_interface_id = source->sequence_elem_interface_id;
    if (row->sequence_elem_class_id == XG_NO_ID)
        row->sequence_elem_class_id = source->sequence_elem_class_id;
    row->sequence_storage_id = source->sequence_storage_id;
    row->sequence_elem_managed_ref = source->sequence_elem_managed_ref;
    row->sequence_elem_object_shape_id = source->sequence_elem_object_shape_id;
    row->sequence_elem_object_shape_literal = source->sequence_elem_object_shape_literal;
    row->sequence_elem_map_container_kind = source->sequence_elem_map_container_kind;
    row->sequence_elem_map_key_type_key = source->sequence_elem_map_key_type_key;
    row->sequence_elem_map_value_type_key = source->sequence_elem_map_value_type_key;
    row->sequence_fresh_empty = source->sequence_fresh_empty;
}

static void body_clear_sequence_json_shape(XgLocalType *row) {
    if (!row)
        return;
    row->sequence_elem_object_shape_id = XG_NO_ID;
    row->sequence_elem_object_shape_literal = NULL;
}

static bool body_sequence_local_elem_is_json(const XgLocalType *row) {
    return row && row->sequence_kind != 0 &&
           row->sequence_elem_type_key == hash_named_type_key32("Json", NULL, 0);
}

static bool body_object_shape_id_same_shape(XgBodyCollect *bc, XgObjectShapeId left_id,
                                            XgObjectShapeId right_id) {
    const XgObjectShapeSummary *left;
    const XgObjectShapeSummary *right;
    if (!bc || left_id == XG_NO_ID || right_id == XG_NO_ID)
        return false;
    if (left_id == right_id)
        return true;
    left = xg_global_evidence_find_object_shape(bc->evidence, left_id);
    right = xg_global_evidence_find_object_shape(bc->evidence, right_id);
    return left && right && left->domain == right->domain &&
           left->field_count == right->field_count &&
           left->stable_shape_key == right->stable_shape_key;
}

static void body_update_sequence_json_shape_from_value(XgBodyCollect *bc, XgLocalType *row,
                                                       const AstNode *value) {
    const ObjectLiteralNode *literal = NULL;
    XgObjectShapeId shape_id;
    if (!bc || !body_sequence_local_elem_is_json(row))
        return;
    shape_id = body_lookup_json_shape(bc, value, &literal);
    if (shape_id == XG_NO_ID)
        literal = body_static_object_literal(value);
    if (shape_id == XG_NO_ID && literal)
        shape_id = body_add_json_shape_for_literal(
            bc, literal, value && value->line > 0 ? (uint32_t) value->line : 0,
            hash_named_type_key32("Json", NULL, 0));
    if (shape_id == XG_NO_ID) {
        body_clear_sequence_json_shape(row);
        return;
    }
    if (row->sequence_elem_object_shape_id != XG_NO_ID) {
        if (!body_object_shape_id_same_shape(bc, row->sequence_elem_object_shape_id, shape_id)) {
            body_clear_sequence_json_shape(row);
            return;
        }
        if (!row->sequence_elem_object_shape_literal)
            row->sequence_elem_object_shape_literal = literal;
        return;
    }
    row->sequence_elem_object_shape_id = shape_id;
    row->sequence_elem_object_shape_literal = literal;
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
    row->sequence_elem_interface_id = XG_NO_ID;
    row->sequence_elem_class_id = XG_NO_ID;
    row->sequence_storage_id = 0;
    row->sequence_elem_managed_ref = false;
    row->sequence_elem_object_shape_id = XG_NO_ID;
    row->sequence_elem_object_shape_literal = NULL;
    row->sequence_elem_map_container_kind = 0;
    row->sequence_elem_map_key_type_key = 0;
    row->sequence_elem_map_value_type_key = 0;
    row->sequence_fresh_empty = false;
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
        case AST_SLICE_EXPR:
            return body_lookup_local_sequence(bc, expr->as.slice_expr.source);
        default:
            break;
    }
    if (expr->type != AST_VARIABLE || !expr->as.variable.name)
        return NULL;
    row = body_find_local(bc, expr->as.variable.name);
    return row && row->sequence_kind != 0 ? row : NULL;
}

static bool body_bulk_overlap_possible(XgBodyCollect *bc, const XgLocalType *dst_local,
                                       const AstNode *src_expr) {
    XgLocalType *src_local = body_lookup_local_sequence(bc, src_expr);
    if (!dst_local || !src_local || dst_local->sequence_storage_id == 0 ||
        src_local->sequence_storage_id == 0)
        return true;
    return dst_local->sequence_storage_id == src_local->sequence_storage_id;
}

static uint32_t body_sequence_storage_alias_count(const XgBodyCollect *bc,
                                                  const XgLocalType *local) {
    uint32_t count = 0;
    if (!bc || !local || local->sequence_storage_id == 0)
        return 0;
    for (uint32_t i = 0; i < bc->nlocals; i++) {
        const XgLocalType *row = &bc->locals[i];
        if (row->sequence_kind != 0 && row->sequence_storage_id == local->sequence_storage_id)
            count++;
    }
    return count;
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
    key_const_id = body_const_expr_id(bc, key);
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
    row.key_prehash = body_map_const_prehash(key, row.key_type_key);
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
        } else if (strcmp(member->name, "containsKey") == 0 && call->arg_count == 1) {
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
        if (strcmp(member->name, "contains") == 0 && call->arg_count == 1) {
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
            return hash_named_type_key32("Array", NULL, 0) ^ body_uint8_type_key();
        case XG_SEQ_STRING:
            return hash_synthetic_tref32(XR_TREF_STRING, NULL, NULL, 0);
        case XG_SEQ_SLICE:
            return hash_named_type_key32("Slice", NULL, 0) ^ elem_type_key;
        case XG_SEQ_BYTE_SLICE:
            return hash_named_type_key32("Slice", NULL, 0) ^ body_uint8_type_key();
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
    row.index_expr_id = body_const_expr_id(bc, index);
    row.length_expr_id = body_const_expr_id(bc, length);
    if (mutating)
        row.flags |= XG_SEQ_ACCESS_MUTATING;
    if (row.index_expr_id != 0)
        row.flags |= XG_SEQ_ACCESS_CONST_INDEX;
    if (access_kind == XG_SEQ_ACCESS_SLICE)
        row.flags |= XG_SEQ_ACCESS_SLICE_NORMALIZED;
    if (local->sequence_kind == XG_SEQ_SLICE || local->sequence_kind == XG_SEQ_BYTE_SLICE)
        row.flags |= XG_SEQ_ACCESS_FROM_SLICE;
    (void) xg_global_evidence_add_sequence_access(bc->evidence, &row);
}

static void body_add_sequence_index_access(XgBodyCollect *bc, const AstNode *node, bool mutating) {
    const AstNode *receiver;
    const AstNode *index;
    const AstNode *value = NULL;
    XgLocalType *local;
    if (!bc || !node)
        return;
    if (node->type == AST_INDEX_GET) {
        receiver = node->as.index_get.array;
        index = node->as.index_get.index;
    } else if (node->type == AST_INDEX_SET) {
        receiver = node->as.index_set.array;
        index = node->as.index_set.index;
        value = node->as.index_set.value;
    } else {
        return;
    }
    local = body_lookup_local_sequence(bc, receiver);
    if (!local)
        return;
    if (mutating)
        body_update_sequence_json_shape_from_value(bc, local, value);
    body_add_sequence_access_row(bc, node, local,
                                 mutating ? XG_SEQ_ACCESS_INDEX_SET : XG_SEQ_ACCESS_INDEX_GET,
                                 index, NULL, mutating);
    if (mutating)
        local->sequence_fresh_empty = false;
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

static bool body_add_sequence_len_call(XgBodyCollect *bc, const AstNode *node) {
    XgLocalType *local;
    XgLocalType field_local;
    if (!bc || !node || node->type != AST_CALL_EXPR)
        return false;
    const CallExprNode *call = &node->as.call_expr;
    if (!call->callee || call->callee->type != AST_VARIABLE || call->arg_count != 1 ||
        !call->callee->as.variable.name || strcmp(call->callee->as.variable.name, "len") != 0)
        return false;
    local = body_lookup_local_sequence(bc, call->arguments[0]);
    if (!local && call->arguments[0] && call->arguments[0]->type == AST_MEMBER_ACCESS) {
        const MemberAccessNode *member = &call->arguments[0]->as.member_access;
        if (member->object && member->object->type == AST_THIS_EXPR && member->name) {
            const XgClassFieldSummary *field = body_find_class_field_in_hierarchy(
                bc, bc->current_class_id, hash_name32(member->name));
            if (field && (field->semantic_kind == XG_CLASS_FIELD_TYPE_ARRAY ||
                          field->semantic_kind == XG_CLASS_FIELD_TYPE_STRING)) {
                memset(&field_local, 0, sizeof(field_local));
                field_local.type_key = field->type_key;
                field_local.sequence_kind = field->semantic_kind == XG_CLASS_FIELD_TYPE_STRING
                                                ? XG_SEQ_STRING
                                                : XG_SEQ_ARRAY;
                field_local.sequence_elem_type_key = field->element_type_key;
                local = &field_local;
            }
        }
    }
    if (!local)
        return true;
    body_add_sequence_access_row(bc, node, local, XG_SEQ_ACCESS_LENGTH, NULL, NULL, false);
    return true;
}

static const AstNode *body_single_expr_statement(const AstNode *body) {
    if (!body)
        return NULL;
    if (body->type == AST_EXPR_STMT)
        return body->as.expr_stmt;
    if (body->type != AST_BLOCK || body->as.block.count != 1 || !body->as.block.statements ||
        !body->as.block.statements[0] || body->as.block.statements[0]->type != AST_EXPR_STMT)
        return NULL;
    return body->as.block.statements[0]->as.expr_stmt;
}

static bool body_counted_loop_push_count_is_proven(const XgBodyCollect *bc, const AstNode *call,
                                                   const XgLocalType *local,
                                                   const AstNode *receiver, const AstNode *value) {
    if (!bc || !call || !local || !local->sequence_fresh_empty || !receiver || !value ||
        receiver->type != AST_VARIABLE ||
        body_single_expr_statement(bc->counted_loop_body) != call || !bc->counted_loop_count_expr ||
        bc->counted_loop_count_expr->node_id == 0 || bc->counted_loop_id == 0)
        return false;
    if (!receiver->as.variable.name || !local->name ||
        strcmp(receiver->as.variable.name, local->name) != 0)
        return false;
    switch (value->type) {
        case AST_VARIABLE:
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_STRING:
        case AST_LITERAL_RUNE:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_LITERAL_NULL:
            return true;
        default:
            return false;
    }
}

static bool body_counted_loop_push_no_clobber_is_proven(const XgBodyCollect *bc,
                                                        const XgLocalType *local) {
    return body_sequence_storage_alias_count(bc, local) == 1;
}

static bool body_rune_scalar_is_utf8_encodable(uint32_t cp) {
    return cp <= 0x10FFFFu && !(cp >= 0xD800u && cp <= 0xDFFFu);
}

/* A StringBuilder.append argument has a compile-time-exact byte length when it
 * is a string literal or a scalar literal whose formatted UTF-8 length is fixed
 * (rune / bool / null). Integer and float literals are intentionally excluded
 * because their formatted width is decided by runtime numeric formatting. Kept
 * in sync with xi_cgen's xicgen_stringbuilder_exact_append_len. */
static bool body_string_builder_append_has_exact_count(const XgLocalType *local,
                                                       const AstNode *value) {
    if (!local || local->sequence_kind != XG_SEQ_STRING_BUILDER || !value || value->node_id == 0)
        return false;
    switch ((AstNodeType) value->type) {
        case AST_LITERAL_STRING:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_LITERAL_NULL:
            return true;
        case AST_LITERAL_RUNE:
            return body_rune_scalar_is_utf8_encodable(value->as.literal.raw_value.rune_val);
        default:
            return false;
    }
}

static bool body_expr_is_zero_fill_literal(const AstNode *value) {
    if (!value)
        return false;
    switch ((AstNodeType) value->type) {
        case AST_LITERAL_INT:
            return !value->as.literal.int_overflows_i64 && value->as.literal.raw_value.int_val == 0;
        case AST_LITERAL_FLOAT:
            return value->as.literal.raw_value.float_val == 0.0;
        case AST_LITERAL_RUNE:
            return value->as.literal.raw_value.rune_val == 0;
        case AST_LITERAL_FALSE:
            return true;
        default:
            return false;
    }
}

static void body_add_capacity_op(XgBodyCollect *bc, const AstNode *node, const XgLocalType *local,
                                 uint8_t op_kind, const AstNode *count_expr, uint32_t loop_id,
                                 uint32_t proof_flags, bool may_grow) {
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
    row.count_expr_id = count_expr ? count_expr->node_id : 0;
    row.loop_id = loop_id;
    row.flags = proof_flags;
    if (may_grow)
        row.flags |= XG_CAPACITY_MAY_GROW;
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
    row.length_expr_id = length_expr ? length_expr->node_id : 0;
    if (body_type_key_is_pod_array_lane(dst_local->sequence_elem_type_key))
        row.flags |= XG_BULK_POD;
    if (overlap_possible)
        row.flags |= XG_BULK_OVERLAP_POSSIBLE;
    if (op_kind == XG_BULK_FILL && body_expr_is_zero_fill_literal(src_expr))
        row.flags |= XG_BULK_ZERO_FILL;
    if (op_kind != XG_BULK_COMPARE && dst_local->sequence_kind == XG_SEQ_ARRAY &&
        dst_local->sequence_elem_managed_ref)
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

static bool body_sequence_registry_receiver_matches(const XgLocalType *local,
                                                    XaBuiltinReceiverKind receiver) {
    if (!local || local->sequence_kind == 0)
        return false;
    switch (receiver) {
        case XA_BUILTIN_RECEIVER_EXACT_INTEGER:
        case XA_BUILTIN_RECEIVER_EXACT_UNSIGNED_INTEGER:
            return false;
        case XA_BUILTIN_RECEIVER_U8_ARRAY:
            return local->sequence_kind == XG_SEQ_BYTES;
        case XA_BUILTIN_RECEIVER_ARRAY:
            return local->sequence_kind == XG_SEQ_ARRAY || local->sequence_kind == XG_SEQ_BYTES;
        case XA_BUILTIN_RECEIVER_U8_SLICE:
            return local->sequence_kind == XG_SEQ_BYTE_SLICE;
        case XA_BUILTIN_RECEIVER_POD_SLICE:
            return (local->sequence_kind == XG_SEQ_SLICE ||
                    local->sequence_kind == XG_SEQ_BYTE_SLICE) &&
                   body_type_key_is_pod_array_lane(local->sequence_elem_type_key);
    }
    return false;
}

static bool body_sequence_registry_arg_count_matches(const XaBuiltinReceiverMethodSpec *spec,
                                                     uint32_t arg_count) {
    if (!spec)
        return false;
    if (arg_count < (uint32_t) spec->min_params)
        return false;
    if (spec->is_variadic)
        return true;
    return arg_count <= (uint32_t) spec->param_count;
}

static bool body_sequence_registry_method_matches(const XgLocalType *local, const char *method_name,
                                                  uint32_t arg_count,
                                                  XaBuiltinReceiverMethodId method_id) {
    const XaBuiltinReceiverMethodSpec *spec = xa_builtin_receiver_method_by_id(method_id);
    return spec && method_name && strcmp(method_name, spec->source_name) == 0 &&
           body_sequence_registry_receiver_matches(local, spec->receiver) &&
           body_sequence_registry_arg_count_matches(spec, arg_count);
}

static bool body_sequence_registry_method_matches_either(const XgLocalType *local,
                                                         const char *method_name,
                                                         uint32_t arg_count,
                                                         XaBuiltinReceiverMethodId left,
                                                         XaBuiltinReceiverMethodId right) {
    return body_sequence_registry_method_matches(local, method_name, arg_count, left) ||
           body_sequence_registry_method_matches(local, method_name, arg_count, right);
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
        if (body_sequence_registry_method_matches(local, member->name, call->arg_count,
                                                  XA_BUILTIN_RECEIVER_METHOD_ARRAY_PUSH)) {
            bool loop_push_count =
                body_counted_loop_push_count_is_proven(bc, node, local, member->object, arg0);
            uint32_t proof_flags = 0;
            if (loop_push_count) {
                proof_flags |= XG_CAPACITY_EXACT_COUNT | XG_CAPACITY_LOOP_APPEND;
                if (body_counted_loop_push_no_clobber_is_proven(bc, local))
                    proof_flags |= XG_CAPACITY_NO_CLOBBER;
            }
            body_add_capacity_op(bc, node, local, XG_CAPACITY_PUSH,
                                 loop_push_count ? bc->counted_loop_count_expr : NULL,
                                 loop_push_count ? bc->counted_loop_id : 0, proof_flags, true);
            local->sequence_fresh_empty = false;
            body_update_sequence_json_shape_from_value(bc, local, arg0);
            return;
        }
        if ((strcmp(member->name, "append") == 0 || strcmp(member->name, "extend") == 0) &&
            call->arg_count >= 1) {
            bool exact_count = strcmp(member->name, "append") == 0 &&
                               body_string_builder_append_has_exact_count(local, arg0);
            body_add_capacity_op(
                bc, node, local,
                strcmp(member->name, "extend") == 0 ? XG_CAPACITY_EXTEND : XG_CAPACITY_APPEND,
                exact_count ? arg0 : NULL, 0, exact_count ? XG_CAPACITY_EXACT_COUNT : 0, true);
            local->sequence_fresh_empty = false;
            if (body_sequence_local_elem_is_json(local))
                body_clear_sequence_json_shape(local);
            return;
        }
        if (body_sequence_registry_method_matches(local, member->name, call->arg_count,
                                                  XA_BUILTIN_RECEIVER_METHOD_ARRAY_RESERVE)) {
            body_add_capacity_op(bc, node, local, XG_CAPACITY_RESERVE, arg0, 0,
                                 XG_CAPACITY_EXACT_COUNT, true);
            return;
        }
        if (body_sequence_registry_method_matches(local, member->name, call->arg_count,
                                                  XA_BUILTIN_RECEIVER_METHOD_ARRAY_CLEAR) ||
            (local->sequence_kind == XG_SEQ_STRING_BUILDER && strcmp(member->name, "clear") == 0 &&
             call->arg_count == 0)) {
            body_add_capacity_op(bc, node, local, XG_CAPACITY_CLEAR, NULL, 0, 0, false);
            local->sequence_fresh_empty = false;
            body_clear_sequence_json_shape(local);
            return;
        }
        if (body_sequence_registry_method_matches(local, member->name, call->arg_count,
                                                  XA_BUILTIN_RECEIVER_METHOD_ARRAY_TO_STRING) ||
            (local->sequence_kind == XG_SEQ_STRING_BUILDER &&
             strcmp(member->name, "toString") == 0 && call->arg_count == 0)) {
            body_add_capacity_op(bc, node, local, XG_CAPACITY_TO_STRING, NULL, 0, 0, false);
            if (local->sequence_kind == XG_SEQ_STRING_BUILDER)
                body_add_encoding_op(bc, node, XG_ENCODING_BYTES_TO_STRING, member->object,
                                     hash_synthetic_tref32(XR_TREF_STRING, NULL, NULL, 0),
                                     XG_ENCODING_VALIDATED_ONCE | XG_ENCODING_SCALAR_BOUNDARY);
            return;
        }
        if (body_sequence_registry_method_matches(
                local, member->name, call->arg_count,
                XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_APPEND_FROM)) {
            body_add_capacity_op(bc, node, local, XG_CAPACITY_APPEND, arg0, 0,
                                 XG_CAPACITY_EXACT_COUNT, true);
            body_add_bulk_op(bc, node, XG_BULK_COPY, local, arg0, arg0,
                             body_bulk_overlap_possible(bc, local, arg0));
            local->sequence_fresh_empty = false;
            if (body_sequence_local_elem_is_json(local))
                body_clear_sequence_json_shape(local);
            return;
        }
        if (body_sequence_registry_method_matches_either(
                local, member->name, call->arg_count, XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_COPY_FROM,
                XA_BUILTIN_RECEIVER_METHOD_POD_SLICE_COPY_FROM)) {
            body_add_bulk_op(bc, node, XG_BULK_COPY, local, arg0, arg0,
                             body_bulk_overlap_possible(bc, local, arg0));
            local->sequence_fresh_empty = false;
            if (body_sequence_local_elem_is_json(local))
                body_clear_sequence_json_shape(local);
            return;
        }
        if (body_sequence_registry_method_matches(local, member->name, call->arg_count,
                                                  XA_BUILTIN_RECEIVER_METHOD_ARRAY_FILL) ||
            body_sequence_registry_method_matches_either(
                local, member->name, call->arg_count, XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_FILL,
                XA_BUILTIN_RECEIVER_METHOD_POD_SLICE_FILL)) {
            body_add_bulk_op(bc, node, XG_BULK_FILL, local, arg0, member->object, false);
            local->sequence_fresh_empty = false;
            body_update_sequence_json_shape_from_value(bc, local, arg0);
            return;
        }
        if (body_sequence_registry_method_matches_either(
                local, member->name, call->arg_count, XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_COMPARE,
                XA_BUILTIN_RECEIVER_METHOD_POD_SLICE_COMPARE)) {
            body_add_bulk_op(bc, node, XG_BULK_COMPARE, local, arg0, arg0, false);
            return;
        }
        if (body_sequence_registry_method_matches_either(
                local, member->name, call->arg_count,
                XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_REPEAT_FROM,
                XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_REPEAT_FROM)) {
            const AstNode *count_expr =
                call->arguments ? call->arguments[call->arg_count - 1] : NULL;
            body_add_bulk_op(bc, node, XG_BULK_REPEAT, local, member->object, count_expr, true);
            local->sequence_fresh_empty = false;
            return;
        }
        local->sequence_fresh_empty = false;
    }

    if (strcmp(member->name, "copyBytes") == 0 && call->arg_count == 0 &&
        body_expr_type_key(bc, member->object) ==
            hash_synthetic_tref32(XR_TREF_STRING, NULL, NULL, 0)) {
        uint32_t output_type_key = hash_named_type_key32("Array", NULL, 0) ^ body_uint8_type_key();
        body_add_encoding_op(bc, node, XG_ENCODING_STRING_TO_BYTES, member->object, output_type_key,
                             XG_ENCODING_KNOWN_UTF8 | XG_ENCODING_SCALAR_BOUNDARY);
        return;
    }
    if ((strcmp(member->name, "fromUtf8") == 0 || strcmp(member->name, "fromUtf8Lossy") == 0) &&
        call->arg_count == 1 && member->object && member->object->type == AST_VARIABLE &&
        member->object->as.variable.name &&
        strcmp(member->object->as.variable.name, "string") == 0) {
        uint32_t output_type_key = hash_synthetic_tref32(XR_TREF_STRING, NULL, NULL, 0);
        body_add_encoding_op(bc, node, XG_ENCODING_BYTES_TO_STRING, arg0, output_type_key,
                             XG_ENCODING_SCALAR_BOUNDARY);
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

static XgFuncId body_call_object_shape_flow_target(const XgBodyCollect *bc,
                                                   const XgCallsiteSummary *callsite) {
    const XgPendingBody *method_body;
    if (!bc || !bc->producer || !callsite)
        return XG_NO_ID;
    if ((callsite->kind == XG_CALL_DIRECT_FUNC || callsite->kind == XG_CALL_CLOSURE) &&
        callsite->static_target_func_id != XG_NO_ID)
        return callsite->static_target_func_id;
    if (callsite->kind != XG_CALL_METHOD || callsite->method_id == XG_NO_ID)
        return XG_NO_ID;
    method_body = producer_find_method_body(bc->producer, callsite->method_id);
    return method_body ? method_body->func_id : XG_NO_ID;
}

static const XgLocalType *body_forwarded_open_object_param(XgBodyCollect *bc,
                                                           const AstNode *argument) {
    while (argument && argument->type == AST_GROUPING)
        argument = argument->as.grouping;
    if (!bc || !argument || argument->type != AST_VARIABLE)
        return NULL;
    XgLocalType *local = body_find_local(bc, argument->as.variable.name);
    return local && local->param_ordinal != UINT16_MAX &&
                   local->object_row_mode == XR_OBJECT_ROW_OPEN
               ? local
               : NULL;
}

static void body_record_call_object_shape_flows(XgBodyCollect *bc, const AstNode *call,
                                                const XgCallsiteSummary *callsite) {
    XgFuncId target_func_id = body_call_object_shape_flow_target(bc, callsite);
    if (!bc || !bc->producer || !bc->evidence || !call || !callsite || target_func_id == XG_NO_ID)
        return;
    for (int i = 0; i < call->as.call_expr.arg_count && i < UINT16_MAX; i++) {
        const ObjectLiteralNode *literal = NULL;
        AstNode *argument = call->as.call_expr.arguments ? call->as.call_expr.arguments[i] : NULL;
        XgObjectShapeId shape_id = body_lookup_or_add_object_shape(bc, argument, &literal);
        const XgObjectShapeSummary *shape;
        const XgLocalType *source_param;
        XgObjectShapeFlowSummary flow;
        memset(&flow, 0, sizeof(flow));
        flow.flow_id = (XgObjectShapeFlowId) (bc->evidence->nobject_shape_flows + 1);
        flow.callsite_id = callsite->callsite_id;
        flow.source_func_id = bc->owner_func_id;
        flow.target_func_id = target_func_id;
        flow.source_param_ordinal = UINT16_MAX;
        flow.target_param_ordinal = (uint16_t) i;
        shape = shape_id != XG_NO_ID ? xg_global_evidence_find_object_shape(bc->evidence, shape_id)
                                     : NULL;
        if (shape && shape->concrete_exact && (shape->flags & XG_OBJECT_SHAPE_OPEN_ROW) == 0 &&
            shape->domain == XG_OBJECT_DOMAIN_STRUCT) {
            flow.concrete_shape_id = shape_id;
            flow.flags = XG_OBJECT_SHAPE_FLOW_CONCRETE;
        } else if ((source_param = body_forwarded_open_object_param(bc, argument)) != NULL) {
            flow.source_param_ordinal = source_param->param_ordinal;
            flow.flags = XG_OBJECT_SHAPE_FLOW_FORWARDED;
        } else {
            continue;
        }
        if (!xg_global_evidence_add_object_shape_flow(bc->evidence, &flow))
            return;
    }
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
        const XgPendingBody *child_target =
            !target ? producer_find_child_function_body(bc->producer, bc->owner_func_id,
                                                        hash_name32(callee_name))
                    : NULL;
        XgClassNameRow *class_row = producer_lookup_class_row(bc->producer, callee_name);
        XgClassSummary *class_summary =
            class_row && class_row->summary_index < bc->evidence->nclasses
                ? &bc->evidence->classes[class_row->summary_index]
                : NULL;
        XgMethodSummary *constructor =
            class_summary ? producer_find_class_method_by_name(bc->evidence, class_summary,
                                                               hash_name32("constructor"), true)
                          : NULL;
        uint32_t callee_name_id = hash_name32(callee_name);
        generic_name = callee_name;
        if (target)
            generic_origin_decl_id = target->decl_id;
        if (class_row)
            bc->capability_bits |= XG_CAP_OBJECTS;
        bc->capability_bits |= body_capabilities_for_builtin_constructor(callee_name);
        if (target && (target->decl_flags & XG_DECL_EXTERN)) {
            if (target->extern_dylib && target->extern_dylib[0])
                (void) producer_add_link_dependency(bc->producer, target->module_id,
                                                    target->decl_id, (uint32_t) call->line,
                                                    XG_LINK_DEP_EXTERN_DYLIB, target->extern_dylib);
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
        } else if (child_target) {
            row.kind = XG_CALL_CLOSURE;
            row.static_target_func_id = child_target->func_id;
        } else if (constructor) {
            row.kind = XG_CALL_METHOD;
            row.receiver_static_class_id = class_row->class_id;
            row.method_id = constructor->method_id;
            row.method_name_id = constructor->name_id;
            row.method_signature_key = constructor->signature_key;
            generic_kind = XG_GENERIC_INST_CLASS;
            generic_origin_class_id = class_row->class_id;
            generic_origin_method_id = constructor->method_id;
        } else if (body_global_builtin_call_is_leaf_intrinsic(callee_name,
                                                              call->as.call_expr.arg_count)) {
            row.kind = XG_CALL_NATIVE;
            row.method_id = (XgMethodId) callee_name_id;
            row.method_name_id = callee_name_id;
            if (strcmp(callee_name, "typeName") == 0)
                bc->metadata_use_bits |= XG_METADATA_TYPENAME;
        } else if (body_variable_is_stdlib_native_function(bc, &callee->as.variable)) {
            bc->effect_bits |= XG_BODY_MAY_CALL_NATIVE;
            bc->escape_bits |= XG_BODY_ESCAPE_NATIVE;
            bc->capability_bits |= XG_CAP_NATIVE;
            row.kind = XG_CALL_NATIVE;
            row.method_id = (XgMethodId) callee_name_id;
            row.method_name_id = callee_name_id;
        }
    } else if (callee && callee->type == AST_MEMBER_ACCESS) {
        const char *stdlib_module =
            body_stdlib_module_for_expr(bc, callee->as.member_access.object);
        const XaBuiltinReceiverMethodSpec *builtin_receiver_method =
            body_builtin_receiver_method_spec(bc, &callee->as.member_access,
                                              call->as.call_expr.arg_count);
        XgInterfaceId receiver_interface =
            body_resolve_expr_interface(bc, callee->as.member_access.object);
        uint32_t method_name_id = hash_name32(callee->as.member_access.name);
        generic_name = callee->as.member_access.name;
        generic_kind = XG_GENERIC_INST_METHOD;
        if (stdlib_module && producer_stdlib_module_known(stdlib_module))
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
            /* A function-typed field called directly (obj.field(args)) is an
             * indirect closure call, not a method dispatch: the field holds a
             * function value, so lowering emits XI_CALL. Recording it as a
             * method would make closed-world reachability search for a method
             * that does not exist and reject the build; the concrete target set
             * is proven by the callable analysis instead. The analyzer records
             * XA_SEL_FIELD on the callee for exactly this case. */
            const XaSelection *field_sel =
                bc->producer->analyzer ? xa_analyzer_get_selection(bc->producer->analyzer, callee)
                                       : NULL;
            XrType *field_result =
                field_sel && field_sel->kind == XA_SEL_FIELD && field_sel->target_symbol &&
                        field_sel->result_type
                    ? xr_type_non_nullable(bc->producer->analyzer->isolate, field_sel->result_type)
                    : NULL;
            if (field_result && field_result->kind == XR_KIND_FUNCTION) {
                row.kind = XG_CALL_CLOSURE;
            } else if (receiver_class != XG_NO_ID) {
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
            } else if (builtin_receiver_method) {
                row.kind = XG_CALL_NATIVE;
                row.method_id = (XgMethodId) method_name_id;
                row.method_name_id = method_name_id;
                if (builtin_receiver_method->effect == XA_BUILTIN_EFFECT_MUTATES_RECEIVER)
                    bc->effect_bits |= XG_BODY_MAY_MUTATE;
                if (builtin_receiver_method->allocation == XA_BUILTIN_ALLOCATION_MAY_HEAP)
                    bc->effect_bits |= XG_BODY_MAY_ALLOC;
            } else {
                bc->effect_bits |= XG_BODY_MAY_CALL_NATIVE;
                bc->escape_bits |= XG_BODY_ESCAPE_NATIVE;
                if (body_is_compiler_owned_native_member(
                        body_resolve_expr_nominal_name_id(bc, callee->as.member_access.object),
                        callee->as.member_access.name, false))
                    bc->capability_bits |= XG_CAP_NATIVE;
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
        body_record_call_object_shape_flows(bc, call, &row);
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
        case AST_LITERAL_RUNE:
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
        bc->inherited_name_local_count = pending->captured_name_local_count;
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
    name = fn->name && fn->name[0] ? fn->name : "<anonymous>";
    signature_key = hash_function_signature(fn);
    source_node_id = producer_unique_body_source_node_id(
        bc->producer, bc->module_id, producer_source_node_id(bc->module_id, node), child_func_id,
        hash_name32(name), signature_key);
    if (!producer_enqueue_body(bc->producer, child_func_id, bc->module_id, XG_NO_ID,
                               bc->current_class_id, XG_NO_ID, hash_name32(name), signature_key,
                               source_node_id, (uint32_t) node->line, XG_BODY_FUNCTION, fn->body,
                               NULL, fn, NULL))
        return;
    pending = &bc->producer->bodies[bc->producer->nbodies - 1];
    pending->lexical_parent_func_id = bc->owner_func_id;
    if (!producer_snapshot_body_captures(pending, bc)) {
        xr_free(pending->captured_locals);
        xr_free(pending->captured_name_locals);
        memset(pending, 0, sizeof(*pending));
        bc->producer->nbodies--;
    }
}

static bool body_builtin_method_call_may_suspend(XgBodyCollect *bc, const AstNode *call) {
    const AstNode *callee;
    uint32_t class_name;
    uint32_t method_name;
    int argc;
    if (!bc || !call || call->type != AST_CALL_EXPR || !(callee = call->as.call_expr.callee) ||
        callee->type != AST_MEMBER_ACCESS)
        return false;
    class_name = body_resolve_expr_nominal_name_id(bc, callee->as.member_access.object);
    if (class_name == 0)
        return false;
    if (!callee->as.member_access.name)
        return false;
    method_name = hash_name32(callee->as.member_access.name);
    argc = call->as.call_expr.arg_count;
    if (class_name == hash_name32("Channel"))
        return (method_name == hash_name32("send") && argc == 1) ||
               (method_name == hash_name32("sendTimeout") && argc == 2) ||
               (method_name == hash_name32("recv") && argc == 0) ||
               (method_name == hash_name32("recvOr") && argc == 1) ||
               (method_name == hash_name32("recvTimeout") && argc == 1);
    if (class_name == hash_name32("Task"))
        return (method_name == hash_name32("awaitResult") && argc == 0) ||
               (method_name == hash_name32("awaitTimeout") && argc == 1);
    if (class_name == hash_name32("WorkQueue"))
        return method_name == hash_name32("pop") && (argc == 0 || argc == 1);
    if (class_name == hash_name32("ResultGroup"))
        return method_name == hash_name32("recv") && argc == 0;
    if (class_name == hash_name32("CountdownLatch"))
        return method_name == hash_name32("wait") && argc == 0;
    if (class_name == hash_name32("Semaphore"))
        return method_name == hash_name32("acquire") && argc == 0;
    if (class_name == hash_name32("EventCount"))
        return method_name == hash_name32("wait") && (argc == 1 || argc == 2);
    return false;
}

static bool body_stdlib_call_may_suspend(XgBodyCollect *bc, const AstNode *call) {
    const AstNode *callee;
    const char *module;
    const char *name;
    if (!bc || !call || call->type != AST_CALL_EXPR || !(callee = call->as.call_expr.callee) ||
        (callee->type != AST_MEMBER_ACCESS && callee->type != AST_VARIABLE))
        return false;
    if (callee->type == AST_MEMBER_ACCESS) {
        name = callee->as.member_access.name;
        module = body_stdlib_module_for_expr(bc, callee->as.member_access.object);
    } else {
        const XgStdlibImportRow *row =
            producer_lookup_stdlib_import(bc->producer, bc->module_id, callee->as.variable.name);
        name = row ? row->member_name : NULL;
        module = row ? row->module_name : NULL;
    }
    if (!name)
        return false;
    if (!module)
        return false;
    if (strcmp(module, "time") == 0)
        return strcmp(name, "sleep") == 0;
    if (strcmp(module, "test_yield") != 0)
        return false;
    return strcmp(name, "simple") == 0 || strcmp(name, "add") == 0 ||
           strcmp(name, "multi_yield") == 0 || strcmp(name, "chain") == 0 ||
           strcmp(name, "error_test") == 0 || strcmp(name, "cancel_test") == 0 ||
           strcmp(name, "counter_inc") == 0 || strcmp(name, "nested") == 0 ||
           strcmp(name, "long_task") == 0;
}

static bool body_stdlib_call_observes_coro_heap(XgBodyCollect *bc, const AstNode *call) {
    const AstNode *callee;
    const char *module;
    if (!bc || !call || call->type != AST_CALL_EXPR || !(callee = call->as.call_expr.callee) ||
        (callee->type != AST_MEMBER_ACCESS && callee->type != AST_VARIABLE))
        return false;
    if (callee->type == AST_MEMBER_ACCESS) {
        if (!callee->as.member_access.name)
            return false;
        module = body_stdlib_module_for_expr(bc, callee->as.member_access.object);
    } else {
        const XgStdlibImportRow *row =
            producer_lookup_stdlib_import(bc->producer, bc->module_id, callee->as.variable.name);
        module = row && row->member_name ? row->module_name : NULL;
    }
    return module && (strcmp(module, "runtime") == 0 || strcmp(module, "test_yield") == 0);
}

static uint32_t body_enum_metadata_bit(uint32_t field) {
    switch ((XaEnumMetaField) field) {
        case XA_ENUM_META_VARIANTS:
        case XA_ENUM_META_LENGTH:
            return XG_METADATA_ENUM_COUNT;
        case XA_ENUM_META_ORDINAL:
            return XG_METADATA_ENUM_ORDINAL;
        case XA_ENUM_META_NAME:
            return XG_METADATA_ENUM_VARIANT_NAME;
        case XA_ENUM_META_PAYLOAD_COUNT:
        case XA_ENUM_META_IS_UNIT:
        case XA_ENUM_META_PAYLOADS:
            return XG_METADATA_ENUM_PAYLOAD_COUNT;
        case XA_ENUM_META_PAYLOAD_INDEX:
            return XG_METADATA_ENUM_PAYLOAD_INDEX;
        case XA_ENUM_META_PAYLOAD_NAME:
            return XG_METADATA_ENUM_PAYLOAD_NAME;
        case XA_ENUM_META_PAYLOAD_TYPE:
            return XG_METADATA_ENUM_PAYLOAD_TYPE;
        default:
            return 0;
    }
}

static void body_add_enum_metadata_use(XgBodyCollect *bc, const AstNode *node) {
    const XaSelection *selection;
    XaSelectionTable *table;
    if (!bc || !bc->producer || !bc->producer->analyzer || !node ||
        !bc->producer->analyzer->selection_table)
        return;
    table = (XaSelectionTable *) bc->producer->analyzer->selection_table;
    selection = xa_selection_table_get(table, node);
    if (!selection) {
        if (node->type == AST_MEMBER_ACCESS && node->as.member_access.object &&
            node->as.member_access.name) {
            XrType *receiver =
                xa_analyzer_get_node_type(bc->producer->analyzer, node->as.member_access.object);
            if (receiver && receiver->kind == XR_KIND_ENUM) {
                if (strcmp(node->as.member_access.name, "name") == 0)
                    bc->metadata_use_bits |= XG_METADATA_ENUM_VARIANT_NAME;
                else if (strcmp(node->as.member_access.name, "ordinal") == 0)
                    bc->metadata_use_bits |= XG_METADATA_ENUM_ORDINAL;
            }
        }
        return;
    }
    if (selection->kind == XA_SEL_ENUM_VARIANTS)
        bc->metadata_use_bits |= XG_METADATA_ENUM_COUNT;
    else if (selection->kind == XA_SEL_ENUM_META) {
        uint32_t field = selection->field_index;
        if (field == XA_ENUM_META_LENGTH &&
            xr_type_is_enum_metadata_named(selection->receiver_type, XR_ENUM_PAYLOADS_TYPE_NAME))
            field = XA_ENUM_META_PAYLOAD_COUNT;
        bc->metadata_use_bits |= body_enum_metadata_bit(field);
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
        case AST_CALL_EXPR: {
            bool intrinsic_sequence_len = body_add_sequence_len_call(bc, node);
            bool intrinsic_array_data_ptr = body_call_is_array_data_ptr_leaf(bc, node);
            if (body_call_uses_coro_runtime(bc, &node->as.call_expr))
                bc->capability_bits |= XG_CAP_COROUTINE;
            if (body_call_is_coro_local_set(bc, &node->as.call_expr)) {
                bc->effect_bits |= XG_BODY_MAY_ALLOC;
                bc->capability_bits |= XG_CAP_OBJECTS;
            }
            if (body_call_is_coro_pool_submit(bc, &node->as.call_expr)) {
                bc->effect_bits |= XG_BODY_MAY_SPAWN | XG_BODY_MAY_ALLOC;
                bc->escape_bits |= XG_BODY_ESCAPE_CORO;
                bc->capability_bits |= XG_CAP_COROUTINE | XG_CAP_TASK | XG_CAP_OBJECTS;
            }
            if (body_stdlib_call_observes_coro_heap(bc, node)) {
                bc->effect_bits |= XG_BODY_OBSERVES_TASK_ID;
                bc->capability_bits |= XG_CAP_COROUTINE;
            }
            if (body_builtin_method_call_may_suspend(bc, node) ||
                body_stdlib_call_may_suspend(bc, node)) {
                bc->effect_bits |= XG_BODY_MAY_SUSPEND;
                bc->capability_bits |= XG_CAP_COROUTINE;
            }
            body_add_json_codec_call(bc, node);
            body_add_map_method_key_access(bc, node);
            body_add_sequence_method_evidence(bc, node);
            if (!intrinsic_sequence_len && !intrinsic_array_data_ptr)
                collect_callsite(bc, node);
            walk_body_for_calls(bc, node->as.call_expr.callee);
            for (int i = 0; i < node->as.call_expr.arg_count; i++)
                walk_body_for_calls(bc, node->as.call_expr.arguments[i]);
            break;
        }
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
        case AST_CONST_DECL: {
            uint32_t type_key = node->as.var_decl.type_annotation
                                    ? hash_tref32(node->as.var_decl.type_annotation)
                                    : 0;
            const ObjectLiteralNode *json_literal =
                body_type_ref_is_json(node->as.var_decl.type_annotation)
                    ? body_static_object_literal(node->as.var_decl.initializer)
                    : NULL;
            const ObjectLiteralNode *object_literal =
                !json_literal ? body_static_object_literal(node->as.var_decl.initializer) : NULL;
            XgObjectShapeId json_shape_id = XG_NO_ID;
            XgObjectShapeId source_json_shape_id = XG_NO_ID;
            const ObjectLiteralNode *source_json_literal = NULL;
            XgObjectShapeId object_shape_id = XG_NO_ID;
            XgObjectShapeId source_object_shape_id = XG_NO_ID;
            const ObjectLiteralNode *source_object_literal = NULL;
            XgMapShapeId map_shape_id = XG_NO_ID;
            uint8_t map_container_kind = 0;
            uint32_t map_receiver_type_key = 0;
            uint32_t map_key_type_key = 0;
            uint32_t map_value_type_key = 0;
            XgLocalType *source_map_local = NULL;
            XgLocalType source_map_snapshot;
            uint8_t sequence_kind = 0;
            uint32_t sequence_elem_type_key = 0;
            const XrTypeRef *sequence_elem_type_ref = NULL;
            const ObjectLiteralNode *sequence_json_literal = NULL;
            XgObjectShapeId sequence_object_shape_id = XG_NO_ID;
            XgLocalType *source_sequence_local = NULL;
            XgLocalType source_sequence_snapshot;
            uint32_t source_sequence_storage_id = 0;
            XgClassId class_id =
                producer_lookup_class_from_tref(bc->producer, node->as.var_decl.type_annotation);
            XgInterfaceId interface_id = producer_lookup_interface_from_tref(
                bc->producer, node->as.var_decl.type_annotation);
            bool inferred = false;
            (void) body_type_ref_sequence_parts(node->as.var_decl.type_annotation, &sequence_kind,
                                                &sequence_elem_type_key);
            sequence_elem_type_ref =
                body_type_ref_sequence_elem_type_ref(node->as.var_decl.type_annotation);
            if (sequence_kind == 0 && node->as.var_decl.initializer) {
                const XrTypeRef *inferred_sequence_type_ref =
                    body_expr_type_ref(bc, node->as.var_decl.initializer);
                if (body_type_ref_sequence_parts(inferred_sequence_type_ref, &sequence_kind,
                                                 &sequence_elem_type_key)) {
                    sequence_elem_type_ref =
                        body_type_ref_sequence_elem_type_ref(inferred_sequence_type_ref);
                } else if (body_expr_is_string_builder_constructor(node->as.var_decl.initializer)) {
                    sequence_kind = XG_SEQ_STRING_BUILDER;
                    sequence_elem_type_key = body_rune_type_key();
                }
            }
            if (sequence_kind != 0 && body_type_ref_is_json(sequence_elem_type_ref))
                sequence_json_literal =
                    body_static_json_array_element_literal(node->as.var_decl.initializer);
            (void) body_add_interface_object_uses_for_type_ref(
                bc, node->as.var_decl.type_annotation, 0, (uint32_t) node->line);
            bc->capability_bits |=
                body_capabilities_for_type_ref(node->as.var_decl.type_annotation);
            walk_body_for_calls(bc, node->as.var_decl.initializer);
            source_json_shape_id =
                body_lookup_json_shape(bc, node->as.var_decl.initializer, &source_json_literal);
            source_object_shape_id =
                body_lookup_object_shape(bc, node->as.var_decl.initializer, &source_object_literal);
            source_map_local = body_lookup_local_map_shape(bc, node->as.var_decl.initializer);
            source_sequence_local = body_lookup_local_sequence(bc, node->as.var_decl.initializer);
            /* body_push_local below may grow bc->locals. Preserve any source
             * row whose metadata must survive that insertion. */
            if (source_map_local) {
                source_map_snapshot = *source_map_local;
                source_map_local = &source_map_snapshot;
            }
            if (source_sequence_local) {
                source_sequence_snapshot = *source_sequence_local;
                source_sequence_local = &source_sequence_snapshot;
                source_sequence_storage_id = source_sequence_local->sequence_storage_id;
            }
            if (node->as.var_decl.initializer &&
                (node->as.var_decl.initializer->type == AST_MAP_LITERAL ||
                 node->as.var_decl.initializer->type == AST_SET_LITERAL)) {
                map_shape_id = body_add_map_shape_for_literal(
                    bc, node->as.var_decl.initializer, node->as.var_decl.type_annotation,
                    (uint32_t) node->line,
                    node->type == AST_CONST_DECL && body_owner_is_module_init(bc),
                    &map_receiver_type_key, &map_key_type_key, &map_value_type_key,
                    &map_container_kind);
                if (type_key == 0 && map_receiver_type_key != 0)
                    type_key = map_receiver_type_key;
            }
            if (json_literal && type_key == 0)
                type_key = hash_named_type_key32("Json", NULL, 0);
            if (object_literal && type_key == 0)
                type_key = body_struct_object_type_key(object_literal);
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
            if (object_literal && type_key == 0)
                type_key = body_struct_object_type_key(object_literal);
            const char *nominal_name =
                node->as.var_decl.type_annotation ? node->as.var_decl.type_annotation->name : NULL;
            if (!nominal_name && node->as.var_decl.initializer &&
                node->as.var_decl.initializer->type == AST_CALL_EXPR &&
                body_call_is_coro_local_new(bc, &node->as.var_decl.initializer->as.call_expr)) {
                nominal_name = "CoroLocal";
            }
            (void) body_push_local(bc, node->as.var_decl.name, node->as.var_decl.symbol_id,
                                   class_id, interface_id, type_key, nominal_name, inferred);
            body_bind_static_object_shape_for_type_key(bc, node->as.var_decl.name, type_key);
            if (json_literal) {
                json_shape_id = body_add_json_shape_for_literal(bc, json_literal,
                                                                (uint32_t) node->line, type_key);
                body_bind_object_shape_local(bc, node->as.var_decl.name, json_shape_id,
                                             json_literal);
            } else if (source_json_shape_id != XG_NO_ID &&
                       body_local_type_is_json(body_find_local(bc, node->as.var_decl.name))) {
                body_bind_object_shape_local(bc, node->as.var_decl.name, source_json_shape_id,
                                             source_json_literal);
            }
            if (object_literal) {
                object_shape_id = body_add_object_shape_for_literal(
                    bc, object_literal, (uint32_t) node->line, type_key);
                body_bind_object_shape_local(
                    bc, node->as.var_decl.name, object_shape_id,
                    body_object_literal_has_spread(object_literal) ? NULL : object_literal);
            } else if (source_object_shape_id != XG_NO_ID) {
                body_bind_object_shape_local(bc, node->as.var_decl.name, source_object_shape_id,
                                             source_object_literal);
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
                body_bind_sequence_local(bc, node->as.var_decl.name, sequence_kind,
                                         sequence_elem_type_key, sequence_elem_type_ref);
            if (source_sequence_local && !sequence_json_literal) {
                if (sequence_kind != 0)
                    body_inherit_sequence_source_metadata(bc, node->as.var_decl.name,
                                                          source_sequence_local);
                else
                    body_bind_sequence_local_from_source(bc, node->as.var_decl.name,
                                                         source_sequence_local);
            }
            if (sequence_kind != 0) {
                XgLocalType *sequence_local = body_find_local(bc, node->as.var_decl.name);
                const AstNode *initializer = node->as.var_decl.initializer;
                if (sequence_local) {
                    if (source_sequence_storage_id != 0) {
                        sequence_local->sequence_storage_id = source_sequence_storage_id;
                    } else if (sequence_kind == XG_SEQ_ARRAY || sequence_kind == XG_SEQ_BYTES ||
                               sequence_kind == XG_SEQ_STRING_BUILDER) {
                        sequence_local->sequence_storage_id = node->node_id;
                    }
                    if (initializer && initializer->type == AST_ARRAY_LITERAL &&
                        !initializer->as.array_literal.is_repeat &&
                        initializer->as.array_literal.count == 0)
                        sequence_local->sequence_fresh_empty = true;
                }
            }
            if (sequence_kind != 0 && body_type_ref_is_json(sequence_elem_type_ref) &&
                !sequence_json_literal) {
                sequence_object_shape_id = body_lookup_sequence_json_ternary_shape(
                    bc, node->as.var_decl.initializer, &sequence_json_literal);
                if (sequence_object_shape_id == XG_NO_ID)
                    sequence_object_shape_id = body_lookup_call_sequence_json_shape(
                        bc, node->as.var_decl.initializer, &sequence_json_literal);
            }
            if (sequence_object_shape_id == XG_NO_ID && sequence_json_literal) {
                sequence_object_shape_id = body_add_json_shape_for_literal(
                    bc, sequence_json_literal, (uint32_t) node->line,
                    sequence_elem_type_key ? sequence_elem_type_key
                                           : hash_named_type_key32("Json", NULL, 0));
            }
            if (sequence_object_shape_id != XG_NO_ID) {
                body_bind_sequence_json_shape_local(
                    bc, node->as.var_decl.name, sequence_object_shape_id, sequence_json_literal);
            }
            {
                const XgLocalType *container_local = body_find_local(bc, node->as.var_decl.name);
                body_add_generic_container_storage(
                    bc, node->as.var_decl.type_annotation, (uint32_t) node->line,
                    container_local ? container_local->map_shape_id : XG_NO_ID);
            }
            break;
        }
        case AST_DESTRUCTURE_DECL:
            walk_body_for_calls(bc, node->as.destructure_decl.initializer);
            body_add_object_destructure_accesses(bc, node->as.destructure_decl.pattern,
                                                 node->as.destructure_decl.initializer,
                                                 (uint32_t) node->line, true);
            bc->effect_bits |= XG_BODY_MAY_READ_MEM;
            break;
        case AST_DESTRUCTURE_ASSIGN:
            walk_body_for_calls(bc, node->as.destructure_assign.value);
            body_add_object_destructure_accesses(bc, node->as.destructure_assign.pattern,
                                                 node->as.destructure_assign.value,
                                                 (uint32_t) node->line, false);
            bc->effect_bits |= XG_BODY_MAY_READ_MEM | XG_BODY_MAY_MUTATE;
            break;
        case AST_ASSIGNMENT: {
            bool target_is_scalar_local = body_owned_local_rebind_is_scalar(
                bc, node->as.assignment.name, node->as.assignment.symbol_id);
            XgLocalType *target_row = body_find_local(bc, node->as.assignment.name);
            XgLocalType target_row_snapshot;
            bool target_is_json = body_local_type_is_json(target_row);
            const ObjectLiteralNode *json_literal =
                target_is_json ? body_static_object_literal(node->as.assignment.value) : NULL;
            const ObjectLiteralNode *source_json_literal = NULL;
            XgObjectShapeId source_json_shape_id =
                body_lookup_json_shape(bc, node->as.assignment.value, &source_json_literal);
            const ObjectLiteralNode *source_object_literal = NULL;
            XgObjectShapeId source_object_shape_id =
                body_lookup_object_shape(bc, node->as.assignment.value, &source_object_literal);
            XgLocalType *source_map_local =
                body_lookup_local_map_shape(bc, node->as.assignment.value);
            XgLocalType source_map_snapshot;
            XgLocalType *source_sequence_local =
                body_lookup_local_sequence(bc, node->as.assignment.value);
            XgLocalType source_sequence_snapshot;
            uint8_t target_sequence_kind = target_row ? target_row->sequence_kind : 0;
            uint32_t target_sequence_elem_type_key =
                target_row ? target_row->sequence_elem_type_key : 0;
            XgInterfaceId target_sequence_elem_interface_id =
                target_row ? target_row->sequence_elem_interface_id : XG_NO_ID;
            const XrTypeRef *source_sequence_type_ref =
                body_expr_type_ref(bc, node->as.assignment.value);
            uint8_t source_sequence_kind = 0;
            uint32_t source_sequence_elem_type_key = 0;
            const XrTypeRef *source_sequence_elem_type_ref = NULL;
            const ObjectLiteralNode *source_sequence_json_literal = NULL;
            XgObjectShapeId source_sequence_object_shape_id = XG_NO_ID;
            XgClassId class_id;
            XgInterfaceId interface_id;
            uint32_t type_key;
            /* Walking and rebinding the RHS may grow or replace bc->locals. */
            if (target_row) {
                target_row_snapshot = *target_row;
                target_row = &target_row_snapshot;
            }
            if (source_map_local) {
                source_map_snapshot = *source_map_local;
                source_map_local = &source_map_snapshot;
            }
            if (source_sequence_local) {
                source_sequence_snapshot = *source_sequence_local;
                source_sequence_local = &source_sequence_snapshot;
            }
            if (body_type_ref_sequence_parts(source_sequence_type_ref, &source_sequence_kind,
                                             &source_sequence_elem_type_key)) {
                source_sequence_elem_type_ref =
                    body_type_ref_sequence_elem_type_ref(source_sequence_type_ref);
                if (body_type_ref_is_json(source_sequence_elem_type_ref)) {
                    source_sequence_object_shape_id = body_lookup_sequence_json_ternary_shape(
                        bc, node->as.assignment.value, &source_sequence_json_literal);
                    if (source_sequence_object_shape_id == XG_NO_ID)
                        source_sequence_object_shape_id = body_lookup_call_sequence_json_shape(
                            bc, node->as.assignment.value, &source_sequence_json_literal);
                }
            }
            walk_body_for_calls(bc, node->as.assignment.value);
            class_id = body_resolve_expr_class(bc, node->as.assignment.value);
            interface_id = body_resolve_expr_interface(bc, node->as.assignment.value);
            type_key = body_expr_type_key(bc, node->as.assignment.value);
            body_clear_json_shape_local(bc, node->as.assignment.name);
            body_clear_object_shape_local(bc, node->as.assignment.name);
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
            body_bind_static_object_shape_for_type_key(
                bc, node->as.assignment.name,
                target_row && target_row->type_key != 0 ? target_row->type_key : type_key);
            if (json_literal) {
                XgObjectShapeId json_shape_id = body_add_json_shape_for_literal(
                    bc, json_literal, (uint32_t) node->line,
                    type_key ? type_key : hash_named_type_key32("Json", NULL, 0));
                body_bind_object_shape_local(bc, node->as.assignment.name, json_shape_id,
                                             json_literal);
            } else if (source_json_shape_id != XG_NO_ID &&
                       body_local_type_is_json(body_find_local(bc, node->as.assignment.name))) {
                body_bind_object_shape_local(bc, node->as.assignment.name, source_json_shape_id,
                                             source_json_literal);
            }
            if (source_object_shape_id != XG_NO_ID)
                body_bind_object_shape_local(bc, node->as.assignment.name, source_object_shape_id,
                                             source_object_literal);
            if (source_map_local)
                body_bind_map_shape_local_from_source(bc, node->as.assignment.name,
                                                      source_map_local);
            if (source_sequence_local && source_sequence_kind != 0) {
                body_bind_sequence_local(bc, node->as.assignment.name, source_sequence_kind,
                                         source_sequence_elem_type_key,
                                         source_sequence_elem_type_ref);
                body_inherit_sequence_source_metadata(bc, node->as.assignment.name,
                                                      source_sequence_local);
            } else if (source_sequence_local) {
                body_bind_sequence_local_from_source(bc, node->as.assignment.name,
                                                     source_sequence_local);
            } else if (source_sequence_kind != 0) {
                body_bind_sequence_local(bc, node->as.assignment.name, source_sequence_kind,
                                         source_sequence_elem_type_key,
                                         source_sequence_elem_type_ref);
            } else if (target_sequence_kind != 0) {
                body_bind_sequence_local(bc, node->as.assignment.name, target_sequence_kind,
                                         target_sequence_elem_type_key, NULL);
                target_row = body_find_local(bc, node->as.assignment.name);
                if (target_row)
                    target_row->sequence_elem_interface_id = target_sequence_elem_interface_id;
            }
            if (source_sequence_object_shape_id == XG_NO_ID && target_sequence_kind != 0 &&
                target_sequence_elem_type_key == hash_named_type_key32("Json", NULL, 0)) {
                source_sequence_object_shape_id = body_lookup_sequence_json_ternary_shape(
                    bc, node->as.assignment.value, &source_sequence_json_literal);
            }
            if (source_sequence_object_shape_id != XG_NO_ID)
                body_bind_sequence_json_shape_local(bc, node->as.assignment.name,
                                                    source_sequence_object_shape_id,
                                                    source_sequence_json_literal);
            if (!target_is_scalar_local)
                bc->effect_bits |= XG_BODY_MAY_MUTATE;
            break;
        }
        case AST_MEMBER_ACCESS: {
            const char *stdlib_module =
                body_stdlib_module_for_expr(bc, node->as.member_access.object);
            if (!body_member_access_is_scalar_builtin(bc, &node->as.member_access))
                bc->effect_bits |= XG_BODY_MAY_READ_MEM;
            if (stdlib_module &&
                producer_stdlib_member_is_constant(stdlib_module, node->as.member_access.name)) {
                (void) producer_add_stdlib_symbol_dependency(bc->producer, bc->module_id,
                                                             (uint32_t) node->line, stdlib_module,
                                                             node->as.member_access.name);
            }
            body_add_json_member_access(bc, node, false);
            body_add_object_field_access(bc, node, false);
            body_add_enum_metadata_use(bc, node);
            walk_body_for_calls(bc, node->as.member_access.object);
            break;
        }
        case AST_MEMBER_SET:
            body_add_json_member_access(bc, node, true);
            body_add_object_field_access(bc, node, true);
            walk_body_for_calls(bc, node->as.member_set.object);
            walk_body_for_calls(bc, node->as.member_set.value);
            bc->effect_bits |= XG_BODY_MAY_MUTATE;
            bc->escape_bits |= XG_BODY_ESCAPE_FIELD;
            break;
        case AST_INDEX_GET:
            bc->effect_bits |= XG_BODY_MAY_READ_MEM;
            body_add_json_index_access(bc, node, false);
            body_add_object_field_access(bc, node, false);
            body_add_map_index_key_access(bc, node, false);
            body_add_sequence_index_access(bc, node, false);
            walk_body_for_calls(bc, node->as.index_get.array);
            walk_body_for_calls(bc, node->as.index_get.index);
            break;
        case AST_INDEX_SET:
            body_add_json_index_access(bc, node, true);
            body_add_object_field_access(bc, node, true);
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
            body_add_json_codec_shape_test(bc, node, node->as.as_expr.type, node->as.as_expr.expr);
            walk_body_for_calls(bc, node->as.as_expr.expr);
            break;
        case AST_IS_EXPR:
            bc->capability_bits |= XG_CAP_INSTANCEOF;
            body_add_json_codec_shape_test(bc, node, node->as.is_expr.type, node->as.is_expr.expr);
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
            bc->effect_bits |= XG_BODY_MAY_ERROR;
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
            /* Spawn requires a task/scheduler, but does not suspend the
             * current body.  Keeping this distinct is what permits a
             * descriptor-only root instead of a resumable main frame. */
            bc->effect_bits |= XG_BODY_MAY_SPAWN | XG_BODY_MAY_ALLOC;
            bc->escape_bits |= XG_BODY_ESCAPE_CORO;
            bc->capability_bits |= XG_CAP_COROUTINE | XG_CAP_TASK | XG_CAP_OBJECTS;
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
            /* Scope exit joins linked children and is therefore a real
             * suspension point even when the body contains no explicit
             * await expression. */
            bc->effect_bits |= XG_BODY_MAY_SUSPEND;
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
        case AST_COMPOUND_ASSIGNMENT: {
            bool target_is_scalar_local =
                node->as.compound_assignment.object == NULL &&
                body_owned_local_rebind_is_scalar(bc, node->as.compound_assignment.name,
                                                  node->as.compound_assignment.symbol_id);
            walk_body_for_calls(bc, node->as.compound_assignment.object);
            walk_body_for_calls(bc, node->as.compound_assignment.value);
            if (!target_is_scalar_local)
                bc->effect_bits |= XG_BODY_MAY_MUTATE;
            break;
        }
        case AST_INC:
            if (!body_owned_local_rebind_is_scalar(bc, node->as.inc.name, node->as.inc.symbol_id))
                bc->effect_bits |= XG_BODY_MAY_MUTATE;
            break;
        case AST_DEC:
            if (!body_owned_local_rebind_is_scalar(bc, node->as.dec.name, node->as.dec.symbol_id))
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
            const AstNode *saved_loop_body = bc->counted_loop_body;
            const AstNode *saved_count_expr = bc->counted_loop_count_expr;
            uint32_t saved_loop_id = bc->counted_loop_id;
            const AstNode *collection = node->as.for_in_stmt.collection;
            XgClassId item_class =
                producer_lookup_class_from_tref(bc->producer, node->as.for_in_stmt.item_type);
            XgInterfaceId item_interface =
                producer_lookup_interface_from_tref(bc->producer, node->as.for_in_stmt.item_type);
            uint32_t item_type_key =
                node->as.for_in_stmt.item_type ? hash_tref32(node->as.for_in_stmt.item_type) : 0;
            /* Untyped for-in item over a tracked Array<C>: inherit the element
             * class so method calls on the item resolve their receiver and
             * get a dispatch plan (R2-3: without it the AOT backend silently
             * bound the base-class method for polymorphic loop receivers). */
            if (item_class == XG_NO_ID) {
                const XgLocalType *seq = body_lookup_local_sequence(bc, collection);
                if (seq) {
                    item_class = seq->sequence_elem_class_id;
                    if (item_interface == XG_NO_ID)
                        item_interface = seq->sequence_elem_interface_id;
                }
            }
            bc->capability_bits |= body_capabilities_for_type_ref(node->as.for_in_stmt.item_type);
            if (node->as.for_in_stmt.domain_kind == XR_FOR_IN_DOMAIN_UNIT_ENUM_VALUES ||
                node->as.for_in_stmt.domain_kind == XR_FOR_IN_DOMAIN_ENUM_VARIANTS)
                bc->metadata_use_bits |= XG_METADATA_ENUM_COUNT;
            bc->effect_bits |= XG_BODY_MAY_READ_MEM;
            walk_body_for_calls(bc, node->as.for_in_stmt.collection);
            (void) body_push_name_local(bc, node->as.for_in_stmt.value_name,
                                        node->as.for_in_stmt.value_symbol_id);
            (void) body_push_local(
                bc, node->as.for_in_stmt.item_name, node->as.for_in_stmt.item_symbol_id, item_class,
                item_interface, item_type_key,
                node->as.for_in_stmt.item_type ? node->as.for_in_stmt.item_type->name : NULL,
                false);
            if (collection && collection->type == AST_RANGE &&
                !collection->as.range.inclusive_end && collection->as.range.start &&
                collection->as.range.start->type == AST_LITERAL_INT &&
                collection->as.range.start->as.literal.int_bits == 0 && collection->as.range.end) {
                bc->counted_loop_body = node->as.for_in_stmt.body;
                bc->counted_loop_count_expr = collection->as.range.end;
                bc->counted_loop_id = node->node_id;
            }
            walk_body_for_calls(bc, node->as.for_in_stmt.body);
            bc->counted_loop_body = saved_loop_body;
            bc->counted_loop_count_expr = saved_count_expr;
            bc->counted_loop_id = saved_loop_id;
            bc->nlocals = base_locals;
            bc->nname_locals = base_name_locals;
            break;
        }
        case AST_TRY_CATCH:
            for (int i = 0; i < node->as.try_catch.catch_count; i++) {
                XrCatchClause *cc =
                    node->as.try_catch.catch_clauses ? node->as.try_catch.catch_clauses[i] : NULL;
                if (cc && cc->is_panic) {
                    bc->effect_bits |= XG_BODY_MAY_PANIC;
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
        case AST_MATCH_EXPR: {
            walk_body_for_calls(bc, node->as.match_expr.expr);
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                const AstNode *arm_node =
                    node->as.match_expr.arms ? node->as.match_expr.arms[i] : NULL;
                if (!arm_node || arm_node->type != AST_MATCH_ARM)
                    continue;
                const MatchArmNode *arm = &arm_node->as.match_arm;
                body_add_match_object_pattern_accesses(bc, arm->pattern, node->as.match_expr.expr);
                walk_body_for_calls(bc, arm->guard);
                walk_body_for_calls(bc, arm->body);
            }
            break;
        }
        default:
            break;
    }
}

static void body_add_method_params(XgBodyCollect *bc, const MethodDeclNode *method) {
    if (!bc || !method)
        return;
    for (int i = 0; i < method->param_count; i++) {
        XrParamNode *param = method->params ? method->params[i] : NULL;
        XrTypeRef *param_type = param ? param->type : NULL;
        const char *param_name = param ? param->name : NULL;
        XgClassId class_id = producer_lookup_class_from_tref(bc->producer, param_type);
        XgInterfaceId interface_id = producer_lookup_interface_from_tref(bc->producer, param_type);
        uint32_t type_key = param_type ? hash_tref32(param_type) : 0;
        uint8_t sequence_kind = 0;
        uint32_t sequence_elem_type_key = 0;
        bc->capability_bits |= body_capabilities_for_type_ref(param_type);
        (void) body_add_interface_object_uses_for_type_ref(bc, param_type,
                                                           XG_INTERFACE_OBJECT_USE_PARAM, 0);
        (void) body_push_local(bc, param_name, 0, class_id, interface_id, type_key,
                               param_type ? param_type->name : NULL, false);
        body_bind_static_object_shape_for_type_key(bc, param_name, type_key);
        {
            XgLocalType *local = body_find_local(bc, param_name);
            if (local) {
                local->param_ordinal = (uint16_t) (i < UINT16_MAX ? i : UINT16_MAX);
                if (param_type && param_type->kind == XR_TREF_OBJECT)
                    local->object_row_mode = (uint8_t) param_type->object_row_mode;
                else if (local->object_shape_id != XG_NO_ID) {
                    const XgObjectShapeSummary *shape =
                        xg_global_evidence_find_object_shape(bc->evidence, local->object_shape_id);
                    if (shape && (shape->flags & XG_OBJECT_SHAPE_OPEN_ROW) != 0)
                        local->object_row_mode = XR_OBJECT_ROW_OPEN;
                }
            }
        }
        body_bind_map_shape_local_for_type_ref(bc, param_name, param_type, 0);
        if (body_type_ref_sequence_parts(param_type, &sequence_kind, &sequence_elem_type_key))
            body_bind_sequence_local(bc, param_name, sequence_kind, sequence_elem_type_key,
                                     body_type_ref_sequence_elem_type_ref(param_type));
        {
            const XgLocalType *container_local = body_find_local(bc, param_name);
            body_add_generic_container_storage(
                bc, param_type, 0, container_local ? container_local->map_shape_id : XG_NO_ID);
        }
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
                               class_id, interface_id, type_key,
                               param && param->type ? param->type->name : NULL, false);
        body_bind_static_object_shape_for_type_key(bc, param ? param->name : NULL, type_key);
        {
            XgLocalType *local = body_find_local(bc, param ? param->name : NULL);
            if (local) {
                local->param_ordinal = (uint16_t) (i < UINT16_MAX ? i : UINT16_MAX);
                if (param && param->type && param->type->kind == XR_TREF_OBJECT)
                    local->object_row_mode = (uint8_t) param->type->object_row_mode;
                else if (local->object_shape_id != XG_NO_ID) {
                    const XgObjectShapeSummary *shape =
                        xg_global_evidence_find_object_shape(bc->evidence, local->object_shape_id);
                    if (shape && (shape->flags & XG_OBJECT_SHAPE_OPEN_ROW) != 0)
                        local->object_row_mode = XR_OBJECT_ROW_OPEN;
                }
            }
        }
        body_bind_map_shape_local_for_type_ref(
            bc, param ? param->name : NULL, param ? param->type : NULL,
            param && param->line > 0 ? (uint32_t) param->line : 0);
        if (body_type_ref_sequence_parts(param ? param->type : NULL, &sequence_kind,
                                         &sequence_elem_type_key))
            body_bind_sequence_local(
                bc, param ? param->name : NULL, sequence_kind, sequence_elem_type_key,
                body_type_ref_sequence_elem_type_ref(param ? param->type : NULL));
        {
            const XgLocalType *container_local = body_find_local(bc, param ? param->name : NULL);
            body_add_generic_container_storage(
                bc, param ? param->type : NULL,
                param && param->line > 0 ? (uint32_t) param->line : 0,
                container_local ? container_local->map_shape_id : XG_NO_ID);
        }
    }
}

static bool pending_body_is_generic_template(const XgProducer *producer,
                                             const XgPendingBody *pending) {
    if (!pending)
        return false;
    /* Function- and method-level generics retain a canonical erased body.
     * Only an open generic class makes the enclosing receiver/layout
     * non-executable before specialization. */
    if (!producer || !producer->evidence || pending->current_class_id == XG_NO_ID)
        return false;
    for (uint32_t i = 0; i < producer->evidence->nclasses; i++) {
        const XgClassSummary *cls = &producer->evidence->classes[i];
        if (cls->class_id == pending->current_class_id)
            return (cls->flags & XG_CLASS_GENERIC_SKELETON) != 0;
    }
    return false;
}

static bool add_body_summary(XgProducer *producer, const XgPendingBody *pending) {
    XgBodyCollect bc;
    XgBodySummary row;
    XgPendingBody pending_copy;
    uint32_t generic_storage_start;
    uint32_t sequence_access_start;
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
    bc.body_kind = pending->kind;
    bc.return_type = pending->method     ? pending->method->return_type
                     : pending->function ? pending->function->return_type
                                         : NULL;
    generic_storage_start = producer->evidence->ngeneric_storages;
    sequence_access_start = producer->evidence->nsequence_accesses;
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
    body_link_generic_array_storage_plans(&bc, generic_storage_start, sequence_access_start);

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
    if (pending_body_is_generic_template(producer, pending))
        row.flags |= XG_BODY_GENERIC_TEMPLATE;
    row.body_hash = hash_ast_shape(pending->body, XR_FNV64_OFFSET_BASIS);
    row.effect_bits = bc.effect_bits;
    if (pending->links) {
        const XaAllocationSummary *allocation =
            pending->links->alloc_effect_id != XA_ALLOC_EFFECT_NONE
                ? xa_allocation_db_get(producer->analyzer->allocation_db,
                                       pending->links->alloc_effect_id)
                : NULL;
        if (allocation) {
            row.allocation_state = (uint8_t) allocation->state;
            row.allocation_reason_bits = allocation->reason_bits;
            row.allocation_fingerprint = allocation->stable_fingerprint;
            row.allocation_complete = 1;
        } else if (pending->links->alloc_effect_complete) {
            row.allocation_state = (uint8_t) pending->links->alloc_state;
            row.allocation_reason_bits = pending->links->alloc_reason_bits;
            row.allocation_fingerprint = pending->links->alloc_fingerprint;
            row.allocation_complete = 1;
        }
    }
    row.escape_bits = bc.escape_bits;
    row.capability_bits = bc.capability_bits;
    row.param_storage_key = hash_param_storage_requirements32(pending->links);
    if (!add_param_storage_summaries(producer, pending->links, pending->func_id,
                                     &row.param_storage_start, &row.param_storage_count)) {
        xr_free(bc.locals);
        xr_free(bc.name_locals);
        return false;
    }
    row.callsite_start = bc.callsite_start;
    row.callsite_count = bc.callsite_count;
    row.metadata_use_bits = bc.metadata_use_bits;
    row.static_data_use_bits = bc.static_data_use_bits;
    row.return_ownership = producer_return_ownership(pending->links);
    xr_free(bc.locals);
    xr_free(bc.name_locals);
    return xg_global_evidence_add_body(producer->evidence, &row) != NULL;
}

static XgObjectAccessSummary *producer_find_mutable_object_access(XgGlobalEvidence *evidence,
                                                                  XgObjectAccessId access_id) {
    if (!evidence || access_id == XG_NO_ID)
        return NULL;
    for (uint32_t i = 0; i < evidence->nobject_accesses; i++) {
        if (evidence->object_accesses[i].object_access_id == access_id)
            return &evidence->object_accesses[i];
    }
    return NULL;
}

static const XgObjectFieldSummary *
producer_find_object_field_by_name(const XgGlobalEvidence *evidence, XgObjectShapeId shape_id,
                                   uint32_t name_id) {
    if (!evidence || shape_id == XG_NO_ID || name_id == 0)
        return NULL;
    for (uint32_t i = 0; i < evidence->nobject_fields; i++) {
        const XgObjectFieldSummary *field = &evidence->object_fields[i];
        if (field->shape_id == shape_id && field->name_id == name_id)
            return field;
    }
    return NULL;
}

typedef struct XgObjectFlowVisit {
    XgFuncId func_id;
    uint16_t param_ordinal;
} XgObjectFlowVisit;

static bool producer_collect_object_flow_shapes(const XgGlobalEvidence *evidence,
                                                XgFuncId target_func_id,
                                                uint16_t target_param_ordinal,
                                                XgObjectShapeId *shape_ids, uint32_t shape_capacity,
                                                uint32_t *shape_count, XgObjectFlowVisit *visits,
                                                uint32_t visit_capacity, uint32_t *visit_count) {
    if (!evidence || target_func_id == XG_NO_ID || !shape_count || !visit_count)
        return false;
    for (uint32_t i = 0; i < *visit_count; i++) {
        if (visits[i].func_id == target_func_id && visits[i].param_ordinal == target_param_ordinal)
            return true;
    }
    if (!visits || *visit_count >= visit_capacity)
        return false;
    visits[*visit_count].func_id = target_func_id;
    visits[*visit_count].param_ordinal = target_param_ordinal;
    (*visit_count)++;
    for (uint32_t i = 0; i < evidence->nobject_shape_flows; i++) {
        const XgObjectShapeFlowSummary *flow = &evidence->object_shape_flows[i];
        if (flow->target_func_id != target_func_id ||
            flow->target_param_ordinal != target_param_ordinal)
            continue;
        if ((flow->flags & XG_OBJECT_SHAPE_FLOW_CONCRETE) != 0) {
            bool duplicate = false;
            if (flow->concrete_shape_id == XG_NO_ID)
                return false;
            for (uint32_t j = 0; j < *shape_count; j++)
                duplicate = duplicate || shape_ids[j] == flow->concrete_shape_id;
            if (!duplicate) {
                if (!shape_ids || *shape_count >= shape_capacity)
                    return false;
                shape_ids[(*shape_count)++] = flow->concrete_shape_id;
            }
        } else if ((flow->flags & XG_OBJECT_SHAPE_FLOW_FORWARDED) != 0) {
            if (flow->source_func_id == XG_NO_ID || flow->source_param_ordinal == UINT16_MAX ||
                !producer_collect_object_flow_shapes(
                    evidence, flow->source_func_id, flow->source_param_ordinal, shape_ids,
                    shape_capacity, shape_count, visits, visit_capacity, visit_count))
                return false;
        } else {
            return false;
        }
    }
    return true;
}

static bool producer_finalize_open_object_accesses(XgProducer *producer) {
    XgGlobalEvidence *evidence;
    if (!producer || !producer->evidence)
        return false;
    evidence = producer->evidence;
    for (uint32_t i = 0; i < producer->nopen_object_accesses; i++) {
        const XgPendingOpenObjectAccess *pending = &producer->open_object_accesses[i];
        XgObjectAccessSummary *access =
            producer_find_mutable_object_access(evidence, pending->access_id);
        const XgObjectFieldSummary *constraint_field;
        XgObjectShapeId *shape_ids;
        XgObjectFlowVisit *visits;
        uint32_t shape_count = 0;
        uint32_t visit_count = 0;
        if (!access || access->owner_func_id != pending->owner_func_id ||
            access->receiver_param_ordinal != pending->param_ordinal ||
            access->constraint_shape_id != pending->constraint_shape_id)
            return false;
        constraint_field = producer_find_object_field_by_name(
            evidence, pending->constraint_shape_id, access->field_name_id);
        if (!constraint_field)
            return false;
        shape_ids =
            evidence->nobject_shape_flows > 0
                ? (XgObjectShapeId *) xr_calloc(evidence->nobject_shape_flows, sizeof(*shape_ids))
                : NULL;
        visits =
            (XgObjectFlowVisit *) xr_calloc(evidence->nobject_shape_flows + 1, sizeof(*visits));
        if ((evidence->nobject_shape_flows > 0 && !shape_ids) || !visits) {
            xr_free(shape_ids);
            return false;
        }
        if (!producer_collect_object_flow_shapes(
                evidence, pending->owner_func_id, pending->param_ordinal, shape_ids,
                evidence->nobject_shape_flows, &shape_count, visits,
                evidence->nobject_shape_flows + 1, &visit_count)) {
            xr_free(visits);
            xr_free(shape_ids);
            return false;
        }
        xr_free(visits);
        for (uint32_t j = 0; j < shape_count; j++) {
            const XgObjectFieldSummary *field;
            field =
                producer_find_object_field_by_name(evidence, shape_ids[j], access->field_name_id);
            if (!field || field->type_key != constraint_field->type_key) {
                xr_free(shape_ids);
                return false;
            }
        }
        if (shape_count == 0) {
            xr_free(shape_ids);
            return false;
        }
        for (uint32_t j = 1; j < shape_count; j++) {
            XgObjectShapeId current = shape_ids[j];
            const XgObjectShapeSummary *current_shape =
                xg_global_evidence_find_object_shape(evidence, current);
            uint32_t k = j;
            while (k > 0) {
                const XgObjectShapeSummary *previous =
                    xg_global_evidence_find_object_shape(evidence, shape_ids[k - 1]);
                if (!previous || !current_shape ||
                    previous->stable_shape_key < current_shape->stable_shape_key ||
                    (previous->stable_shape_key == current_shape->stable_shape_key &&
                     previous->object_shape_id <= current_shape->object_shape_id))
                    break;
                shape_ids[k] = shape_ids[k - 1];
                k--;
            }
            shape_ids[k] = current;
        }
        access->receiver_shape_count = (uint16_t) shape_count;
        access->receiver_shape_set_id = access->object_access_id;
        access->receiver_shape_id = shape_ids[0];
        access->result_type_key = constraint_field->type_key;
        for (uint32_t j = 0; j < shape_count; j++) {
            const XgObjectShapeSummary *shape =
                xg_global_evidence_find_object_shape(evidence, shape_ids[j]);
            const XgObjectFieldSummary *field =
                producer_find_object_field_by_name(evidence, shape_ids[j], access->field_name_id);
            XgObjectAccessCaseSummary access_case;
            if (!shape || !field) {
                xr_free(shape_ids);
                return false;
            }
            if (j == 0) {
                access->field_ordinal = field->field_ordinal;
                access->mutation_epoch = shape->mutation_epoch;
            }
            memset(&access_case, 0, sizeof(access_case));
            access_case.case_id = (XgObjectAccessCaseId) (evidence->nobject_access_cases + 1);
            access_case.object_access_id = access->object_access_id;
            access_case.receiver_shape_set_id = access->receiver_shape_set_id;
            access_case.receiver_shape_id = shape->object_shape_id;
            access_case.stable_shape_key = shape->stable_shape_key;
            access_case.mutation_epoch = shape->mutation_epoch;
            access_case.field_ordinal = field->field_ordinal;
            access_case.domain = shape->domain;
            if (!xg_global_evidence_add_object_access_case(evidence, &access_case)) {
                xr_free(shape_ids);
                return false;
            }
        }
        xr_free(shape_ids);
    }
    return true;
}

static bool producer_emit_body_summaries(XgProducer *producer) {
    if (!producer)
        return false;
    for (uint32_t i = 0; i < producer->nbodies; i++) {
        if (!add_body_summary(producer, &producer->bodies[i]))
            return false;
    }
    return producer_finalize_open_object_accesses(producer);
}

static bool add_function_decl(XgProducer *p, XgModuleId module_id, const AstNode *node) {
    const FunctionDeclNode *fn = &node->as.function_decl;
    XgDeclSummary decl;
    XgDeclId decl_id = (XgDeclId) (p->evidence->ndecls + 1);
    XgFuncId func_id = producer_next_func_id(p);
    memset(&decl, 0, sizeof(decl));
    decl.module_id = module_id;
    decl.decl_id = decl_id;
    decl.kind = XG_DECL_FUNC;
    decl.name_id = hash_name32(fn->name);
    decl.signature_key = hash_function_signature(fn);
    decl.source_node_id =
        producer_unique_decl_source_node_id(p, module_id, producer_source_node_id(module_id, node),
                                            decl.kind, decl.name_id, decl.signature_key);
    decl.source_span_id = (uint32_t) node->line;
    decl.storage_domain = XR_STORAGE_MODULE_STATIC;
    decl.storage_mutability = XR_STORAGE_READONLY;
    decl.address_identity = XR_ADDRESS_MODULE_STABLE;
    decl.materialization_kind = XR_MATERIALIZE_STATIC_DATA;
    if (fn->is_extern)
        decl.flags |= XG_DECL_EXTERN;
    if (fn->type_param_count > 0)
        decl.flags |= XG_DECL_GENERIC_TEMPLATE;
    if (!xg_global_evidence_add_decl(p->evidence, &decl))
        return false;
    if (!producer_register_func(p, module_id, fn->name, NULL, func_id, decl_id, decl.flags))
        return false;
    return producer_enqueue_body(p, func_id, module_id, decl_id, XG_NO_ID, XG_NO_ID,
                                 hash_name32(fn->name), decl.signature_key, decl.source_node_id,
                                 (uint32_t) node->line, XG_BODY_FUNCTION, fn->body, NULL, fn,
                                 producer_function_links(p, fn));
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
    XaSymbolLinks *class_links = producer_class_links(p, cls);
    XrClassInfo *class_info = class_links ? class_links->class_info : NULL;
    memset(&decl, 0, sizeof(decl));
    decl.module_id = module_id;
    decl.decl_id = decl_id;
    decl.kind = (uint8_t) kind;
    decl.name_id = hash_name32(cls->name);
    decl.source_node_id =
        producer_unique_decl_source_node_id(p, module_id, producer_source_node_id(module_id, node),
                                            decl.kind, decl.name_id, decl.signature_key);
    decl.source_span_id = (uint32_t) node->line;
    decl.storage_domain = XR_STORAGE_MODULE_STATIC;
    decl.storage_mutability = XR_STORAGE_READONLY;
    decl.address_identity = XR_ADDRESS_MODULE_STABLE;
    decl.materialization_kind = XR_MATERIALIZE_STATIC_DATA;
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
        method.name_id = hash_name32(m->name);
        method.signature_key = hash_method_signature(m);
        method.source_node_id = producer_unique_method_source_node_id(
            p, module_id, class_id, producer_source_node_id(module_id, method_node), method.name_id,
            method.signature_key);
        if (m->is_static)
            method.flags |= XG_METHOD_STATIC;
        if (m->is_constructor)
            method.flags |= XG_METHOD_CONSTRUCTOR;
        if (!m->body)
            method.flags |= XG_METHOD_NATIVE;
        if (!cls->is_monomorphized && (cls->type_param_count > 0 || cls->is_generic_skeleton))
            method.flags |= XG_METHOD_GENERIC_TEMPLATE;
        method.return_ownership =
            producer_return_ownership(producer_method_links(p, class_info, m));
        if (!xg_global_evidence_add_method(p->evidence, &method))
            return false;
        method_count++;
        if (!producer_enqueue_body(p, method_func_id, module_id, decl_id, class_id,
                                   method.method_id, hash_name32(m->name), method.signature_key,
                                   method.source_node_id, (uint32_t) method_node->line,
                                   XG_BODY_METHOD, m->body, m, NULL,
                                   producer_method_links(p, class_info, m)))
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
        summary.owner_class_id = class_id;
        summary.name_id = hash_name32(field->name);
        summary.type_key = hash_tref32(field->field_type);
        summary.decl_ordinal = (uint32_t) i;
        summary.source_node_id = producer_unique_class_field_source_node_id(
            p, module_id, producer_source_node_id(module_id, field_node), class_id, summary.name_id,
            summary.decl_ordinal);
        summary.instance_slot = field->is_static ? UINT32_MAX : instance_field_count++;
        if (!class_field_fill_type_facts(&summary, field->field_type))
            return false;
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
    if (!cls->is_monomorphized && (cls->type_param_count > 0 || cls->is_generic_skeleton))
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
    // Backfill the evidence class id onto the analyzer's class info so IR
    // lowering can resolve field accesses through the exact declaring class,
    // disambiguating same-named classes exported by different modules.
    if (class_info)
        class_info->xg_class_id = class_id;
    if (!producer_add_decl_derives(p, module_id, decl_id, (uint32_t) node->line, cls->name,
                                   derive_flags, cls, class_id))
        return false;
    if (!add_monomorphized_class_instantiation(p, module_id, node, cls, class_id))
        return false;
    return producer_register_class(p, module_id, cls->name, cls->super_name, node, class_id,
                                   p->evidence->nclasses - 1);
}

static bool add_interface_decl(XgProducer *p, XgModuleId module_id, const AstNode *node) {
    const InterfaceDeclNode *iface = &node->as.interface_decl;
    XgDeclSummary decl;
    XgInterfaceId interface_id = (XgInterfaceId) hash_name32(iface->name);
    memset(&decl, 0, sizeof(decl));
    decl.module_id = module_id;
    decl.decl_id = (XgDeclId) (p->evidence->ndecls + 1);
    decl.kind = XG_DECL_INTERFACE;
    decl.name_id = interface_id;
    decl.signature_key = (uint32_t) iface->method_count;
    decl.source_node_id =
        producer_unique_decl_source_node_id(p, module_id, producer_source_node_id(module_id, node),
                                            decl.kind, decl.name_id, decl.signature_key);
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
    uint32_t type_key = hash_named_type_key32(e->name, NULL, 0);
    memset(&decl, 0, sizeof(decl));
    decl.module_id = module_id;
    decl.decl_id = (XgDeclId) (p->evidence->ndecls + 1);
    decl.kind = XG_DECL_ENUM;
    decl.name_id = hash_name32(e->name);
    decl.type_key = type_key;
    decl.signature_key = (uint32_t) e->member_count;
    decl.source_node_id =
        producer_unique_decl_source_node_id(p, module_id, producer_source_node_id(module_id, node),
                                            decl.kind, decl.name_id, decl.signature_key);
    decl.source_span_id = (uint32_t) node->line;
    decl.storage_domain = XR_STORAGE_MODULE_STATIC;
    decl.storage_mutability = XR_STORAGE_READONLY;
    decl.address_identity = XR_ADDRESS_MODULE_STABLE;
    decl.materialization_kind = XR_MATERIALIZE_STATIC_DATA;
    if (derive_flags != 0)
        decl.flags |= XG_DECL_DERIVE;
    decl.derive_flags = derive_flags;
    if (!xg_global_evidence_add_decl(p->evidence, &decl))
        return false;
    if (!producer_register_enum(p, module_id, e->name, e, type_key))
        return false;
    return producer_add_decl_derives(p, module_id, decl.decl_id, (uint32_t) node->line, e->name,
                                     derive_flags, NULL, 0);
}

static bool add_import_link_dependencies(XgProducer *p, XgModuleId module_id, const AstNode *node) {
    const ImportStmtNode *import;
    bool link_known;
    if (!p || !node || node->type != AST_IMPORT_STMT)
        return true;
    import = &node->as.import_stmt;
    link_known = producer_stdlib_module_known(import->module_name);
    if (!link_known && !producer_vm_control_module_known(import->module_name))
        return true;
    if (link_known && !producer_add_link_dependency(p, module_id, XG_NO_ID, (uint32_t) node->line,
                                                    XG_LINK_DEP_STDLIB_MODULE, import->module_name))
        return false;
    if (import->member_count == 0) {
        const char *local_name = import->alias ? import->alias : import->module_name;
        return producer_register_stdlib_import(p, module_id, local_name, import->module_name, NULL);
    }
    for (int i = 0; i < import->member_count; i++) {
        const ImportMember *member = &import->members[i];
        const char *local_name = member->alias ? member->alias : member->name;
        if (link_known &&
            !producer_add_stdlib_symbol_dependency(p, module_id, (uint32_t) node->line,
                                                   import->module_name, member->name))
            return false;
        if (!producer_register_stdlib_import(p, module_id, local_name, import->module_name,
                                             member->name))
            return false;
    }
    return true;
}

static bool add_type_alias_object_shape(XgProducer *p, XgModuleId module_id, const AstNode *node) {
    const TypeAliasNode *alias;
    XgObjectShapeId object_shape_id;
    if (!p || !p->evidence || !node || node->type != AST_TYPE_ALIAS)
        return true;
    alias = &node->as.type_alias;
    if (!alias->name || alias->type_param_count > 0 ||
        body_type_alias_object_field_count(alias) <= 0)
        return true;
    object_shape_id = (XgObjectShapeId) (p->evidence->nobject_shapes + 1);
    if (body_add_object_shape_for_type_alias(p, module_id, alias, (uint32_t) node->line) !=
        object_shape_id)
        return false;
    return true;
}

static bool module_stmt_has_runtime_body(const AstNode *stmt) {
    if (!stmt)
        return false;
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

static bool add_module_storage_decl(XgProducer *p, XgModuleId module_id, const AstNode *stmt) {
    XgDeclSummary decl;
    const char *name = NULL;
    if (!stmt)
        return true;
    if (stmt->type == AST_IMPORT_STMT) {
        if (stmt->as.import_stmt.member_count == 0) {
            name = stmt->as.import_stmt.alias ? stmt->as.import_stmt.alias
                                              : stmt->as.import_stmt.module_name;
        } else {
            for (int i = 0; i < stmt->as.import_stmt.member_count; i++) {
                const ImportMember *member = &stmt->as.import_stmt.members[i];
                XgDeclSummary import_decl;
                memset(&import_decl, 0, sizeof(import_decl));
                import_decl.module_id = module_id;
                import_decl.source_node_id =
                    producer_source_node_id(module_id, stmt) + (uint32_t) i;
                import_decl.decl_id = (XgDeclId) (p->evidence->ndecls + 1);
                import_decl.kind = XG_DECL_GLOBAL;
                import_decl.name_id = hash_name32(member->alias ? member->alias : member->name);
                import_decl.source_span_id = (uint32_t) stmt->line;
                import_decl.storage_domain = XR_STORAGE_MODULE_STATIC;
                import_decl.storage_mutability = XR_STORAGE_READONLY;
                import_decl.address_identity = XR_ADDRESS_MODULE_STABLE;
                import_decl.materialization_kind = XR_MATERIALIZE_STATIC_DATA;
                if (!xg_global_evidence_add_decl(p->evidence, &import_decl))
                    return false;
            }
            return true;
        }
    } else if (stmt->type == AST_VAR_DECL || stmt->type == AST_CONST_DECL) {
        name = stmt->as.var_decl.name;
    } else {
        return true;
    }
    memset(&decl, 0, sizeof(decl));
    decl.module_id = module_id;
    decl.source_node_id = producer_source_node_id(module_id, stmt);
    decl.decl_id = (XgDeclId) (p->evidence->ndecls + 1);
    decl.kind = XG_DECL_GLOBAL;
    decl.name_id = hash_name32(name);
    decl.type_key =
        stmt->type == AST_IMPORT_STMT ? 0 : hash_tref32(stmt->as.var_decl.type_annotation);
    decl.source_span_id = (uint32_t) stmt->line;
    decl.storage_domain = XR_STORAGE_MODULE_STATIC;
    decl.storage_mutability =
        stmt->type == AST_CONST_DECL ? XR_STORAGE_READONLY : XR_STORAGE_MUTABLE;
    if (stmt->type == AST_IMPORT_STMT)
        decl.storage_mutability = XR_STORAGE_READONLY;
    decl.address_identity = XR_ADDRESS_MODULE_STABLE;
    decl.materialization_kind = XR_MATERIALIZE_STATIC_DATA;
    return xg_global_evidence_add_decl(p->evidence, &decl) != NULL;
}

static bool add_module_decl_stmt(XgProducer *p, XgModuleId module_id, const AstNode *stmt,
                                 bool *handled) {
    if (handled)
        *handled = true;
    if (!stmt)
        return true;
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
            return add_type_alias_object_shape(p, module_id, stmt);
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
        if (!add_module_storage_decl(p, module_id, stmt))
            return false;
        if (!add_module_decl_stmt(p, module_id, stmt, &handled))
            return false;
        if (!handled && module_stmt_has_runtime_body(stmt))
            has_module_body = true;
    }
    if (has_module_body) {
        XgFuncId module_func_id = producer_next_func_id(p);
        if (!producer_enqueue_body(p, module_func_id, module_id, XG_NO_ID, XG_NO_ID, XG_NO_ID,
                                   hash_name32("<module-init>"), 0, 0, 0, XG_BODY_MODULE_INIT, ast,
                                   NULL, NULL, NULL))
            return false;
    }
    return true;
}

static uint64_t module_source_hash(const XrModuleSpec *spec);

/*
 * Identity of the compiler that produced a cached evidence payload.
 *
 * The payload encodes what *this* compiler concluded about the source, so the
 * cache key has to name the compiler, not just its semantic version. Two
 * binaries built from the same version disagree the moment either the frontend
 * or the producer changes, and replaying the older one's evidence surfaces far
 * downstream as a decl that no longer exists — "module storage provenance is
 * missing" — rather than as a stale cache.
 *
 * The image's size and mtime change on every rebuild and cost one stat, taken
 * once per process. A compiler we cannot locate or stat falls back to the
 * version hash alone: degrading to today's behavior beats refusing to build.
 */
static uint64_t compiler_image_hash(void) {
    static uint64_t cached;
    static bool computed;
    char exe[4096];
    XrFsStat st;
    if (computed)
        return cached;
    computed = true;
    cached = XG_COMPILER_SEMVER_HASH;
    if (xr_proc_self_exe_path(exe, sizeof(exe)) != 0)
        return cached;
    if (xr_fs_stat(exe, &st) != 0 || st.kind != XR_FS_FILE)
        return cached;
    cached = fold_u64(cached, st.size);
    cached = fold_u64(cached, (uint64_t) st.mtime_ns);
    return cached;
}

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
    key.compiler_semver_hash = compiler_image_hash();
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
    key.compiler_semver_hash = compiler_image_hash();
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
    return xg_global_evidence_build_from_module_graph_with_imported_modules_and_analyzer(
        evidence, graph, profile, imported_summary_hash, imported_modules, imported_module_count,
        NULL);
}

XR_FUNC bool xg_global_evidence_build_from_module_graph_with_imported_modules_and_analyzer(
    XgGlobalEvidence *evidence, const XrModuleGraph *graph, uint32_t profile,
    uint64_t imported_summary_hash, const XgModuleSummary *imported_modules,
    uint32_t imported_module_count, XaAnalyzer *analyzer) {
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
    producer.analyzer = analyzer;

    for (int ti = 0; ti < graph->topo_count; ti++) {
        int idx = graph->topo_order[ti];
        const XrModuleSpec *spec = &graph->specs[idx];
        XgModuleId module_id = (XgModuleId) (ti + 1);
        XgModuleSummary module_summary;
        if (!xg_module_summary_from_module_spec(&module_summary, module_id, spec) ||
            !xg_global_evidence_add_module(evidence, &module_summary)) {
            xr_free(producer.classes);
            xr_free(producer.interfaces);
            xr_free(producer.enums);
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
            xr_free(producer.enums);
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
        xr_free(producer.enums);
        xr_free(producer.funcs);
        xr_free(producer.stdlib_imports);
        producer_free_bodies(&producer);
        xg_global_evidence_free(evidence);
        return false;
    }
    xr_free(producer.classes);
    xr_free(producer.interfaces);
    xr_free(producer.enums);
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

static bool evidence_sequence_access_requires_storage_clone(uint8_t access_kind) {
    switch ((XgSequenceAccessKind) access_kind) {
        case XG_SEQ_ACCESS_INDEX_GET:
        case XG_SEQ_ACCESS_INDEX_SET:
        case XG_SEQ_ACCESS_SLICE:
        case XG_SEQ_ACCESS_ITER:
            return true;
        case XG_SEQ_ACCESS_LENGTH:
        default:
            return false;
    }
}

static bool evidence_generic_storage_forces_body_clone(const XgGlobalEvidence *evidence,
                                                       XgFuncId specialized_body_func_id) {
    if (!evidence || specialized_body_func_id == XG_NO_ID)
        return false;
    for (uint32_t i = 0; i < evidence->ngeneric_storages; i++) {
        const XgGenericStorageSummary *storage = &evidence->generic_storages[i];
        uint32_t clone_relevant =
            storage->flags & (XG_GENERIC_STORAGE_TYPED_INLINE | XG_GENERIC_STORAGE_REF_LANE);
        if (clone_relevant == 0 || storage->container_plan_id == XG_NO_ID)
            continue;
        switch ((XgGenericStorageKind) storage->storage_kind) {
            case XG_GENERIC_STORAGE_ARRAY: {
                const XgSequenceAccessSummary *sequence = xg_global_evidence_find_sequence_access(
                    evidence, (XgSequenceAccessId) storage->container_plan_id);
                if (sequence && sequence->owner_func_id == specialized_body_func_id &&
                    sequence->receiver_type_key == storage->specialized_type_key &&
                    sequence->elem_type_key == storage->elem_type_key &&
                    evidence_sequence_access_requires_storage_clone(sequence->access_kind))
                    return true;
                break;
            }
            case XG_GENERIC_STORAGE_MAP:
            case XG_GENERIC_STORAGE_SET:
                for (uint32_t j = 0; j < evidence->nkey_accesses; j++) {
                    const XgKeyAccessSummary *access = &evidence->key_accesses[j];
                    if (access->owner_func_id == specialized_body_func_id &&
                        access->receiver_shape_id == storage->container_plan_id &&
                        access->key_type_key == storage->key_type_key &&
                        access->value_type_key == storage->value_type_key)
                        return true;
                }
                break;
            case XG_GENERIC_STORAGE_CLASS:
            case XG_GENERIC_STORAGE_STRUCT:
            default:
                break;
        }
    }
    return false;
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
    if (evidence_generic_storage_forces_body_clone(dst, specialized_body->func_id))
        code_size.flags = XG_GENERIC_CODESIZE_FORCE_CLONE;
    else if ((uint64_t) specialized_size * (uint64_t) code_size.instantiation_count <=
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
