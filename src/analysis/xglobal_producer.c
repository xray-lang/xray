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
#include <stdint.h>
#include <string.h>

enum {
    XG_BODY_MAY_THROW = 1u << 0,
    XG_BODY_MAY_SUSPEND = 1u << 1,
    XG_BODY_MAY_ALLOC = 1u << 2,
    XG_BODY_MAY_MUTATE = 1u << 3,
    XG_BODY_MAY_CALL_NATIVE = 1u << 4,
};

typedef struct XgClassNameRow {
    const char *name;
    const char *super_name;
    XgClassId class_id;
    uint32_t summary_index;
} XgClassNameRow;

typedef struct XgFuncNameRow {
    const char *name;
    XgFuncId func_id;
} XgFuncNameRow;

typedef struct XgInterfaceNameRow {
    const char *name;
    XgInterfaceId interface_id;
    const InterfaceDeclNode *decl;
} XgInterfaceNameRow;

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
    struct XgPendingBody *bodies;
    uint32_t nbodies;
    uint32_t body_cap;
    XgFuncId next_func_id;
    uint32_t field_cursor;
} XgProducer;

typedef struct XgPendingBody {
    XgFuncId func_id;
    XgClassId current_class_id;
    const AstNode *body;
    const MethodDeclNode *method;
    const FunctionDeclNode *function;
} XgPendingBody;

typedef struct XgLocalType {
    const char *name;
    XgClassId class_id;
    XgInterfaceId interface_id;
    bool inferred;
} XgLocalType;

typedef struct XgBodyCollect {
    XgProducer *producer;
    XgGlobalEvidence *evidence;
    XgFuncId owner_func_id;
    XgClassId current_class_id;
    XgLocalType *locals;
    uint32_t nlocals;
    uint32_t local_cap;
    uint32_t callsite_start;
    uint32_t callsite_count;
    uint32_t effect_bits;
    uint32_t capability_bits;
    uint32_t metadata_use_bits;
    uint32_t static_data_use_bits;
} XgBodyCollect;

static uint64_t fold_bytes(uint64_t h, const void *data, size_t len) {
    uint64_t part = xr_hash_bytes64(data, len);
    h ^= part + UINT64_C(0x9e3779b97f4a7c15) + (h << 6) + (h >> 2);
    return h ? h : 1;
}

static uint64_t fold_u64(uint64_t h, uint64_t value) {
    return fold_bytes(h, &value, sizeof(value));
}

static uint32_t hash_name32(const char *name) {
    if (!name || !name[0])
        return 0;
    uint32_t h = xr_hash_bytes(name, strlen(name));
    return h ? h : 1;
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
            h = fold_u64(h, (uint64_t) node->as.call_expr.arg_count);
            for (int i = 0; i < node->as.call_expr.arg_count; i++)
                h = hash_ast_shape(node->as.call_expr.arguments[i], h);
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

static bool producer_register_func(XgProducer *p, const char *name, XgFuncId func_id) {
    if (!name || func_id == XG_NO_ID)
        return true;
    if (!producer_reserve_funcs(p, p->nfuncs + 1))
        return false;
    p->funcs[p->nfuncs].name = name;
    p->funcs[p->nfuncs].func_id = func_id;
    p->nfuncs++;
    return true;
}

static XgFuncId producer_lookup_func(const XgProducer *p, const char *name) {
    if (!p || !name)
        return XG_NO_ID;
    for (uint32_t i = 0; i < p->nfuncs; i++) {
        if (p->funcs[i].name && strcmp(p->funcs[i].name, name) == 0)
            return p->funcs[i].func_id;
    }
    return XG_NO_ID;
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

static bool producer_register_interface(XgProducer *p, const char *name,
                                        const InterfaceDeclNode *decl) {
    XgInterfaceNameRow *existing;
    if (!name)
        return true;
    existing = producer_lookup_interface_row(p, name);
    if (existing) {
        existing->decl = decl;
        return true;
    }
    if (!producer_reserve_interfaces(p, p->ninterfaces + 1))
        return false;
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

static bool producer_enqueue_body(XgProducer *p, XgFuncId func_id, XgClassId current_class_id,
                                  const AstNode *body, const MethodDeclNode *method,
                                  const FunctionDeclNode *function) {
    XgPendingBody *row;
    if (!body)
        return true;
    if (!producer_reserve_bodies(p, p->nbodies + 1))
        return false;
    row = &p->bodies[p->nbodies++];
    memset(row, 0, sizeof(*row));
    row->func_id = func_id;
    row->current_class_id = current_class_id;
    row->body = body;
    row->method = method;
    row->function = function;
    return true;
}

static bool producer_register_class(XgProducer *p, const char *name, const char *super_name,
                                    XgClassId class_id, uint32_t summary_index) {
    if (!producer_reserve_classes(p, p->nclasses + 1))
        return false;
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
                                                           uint32_t name_id) {
    if (!ev || !cls || cls->method_start == 0 || name_id == 0)
        return NULL;
    for (uint32_t i = 0; i < cls->method_count; i++) {
        uint32_t method_index = cls->method_start - 1 + i;
        XgMethodSummary *method = method_index < ev->nmethods ? &ev->methods[method_index] : NULL;
        if (method && method->name_id == name_id && (method->flags & XG_METHOD_STATIC) == 0 &&
            (method->flags & XG_METHOD_CONSTRUCTOR) == 0)
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

static XgMethodSummary *producer_find_method_by_name_in_hierarchy(XgProducer *p, XgClassId class_id,
                                                                  uint32_t name_id) {
    XgClassNameRow *row = producer_lookup_class_row_by_id(p, class_id);
    uint32_t depth = 0;
    while (row && depth++ < 64) {
        XgClassSummary *summary;
        XgMethodSummary *method;
        if (row->summary_index >= p->evidence->nclasses)
            return NULL;
        summary = &p->evidence->classes[row->summary_index];
        method = producer_find_class_method_by_name(p->evidence, summary, name_id);
        if (method)
            return method;
        if (summary->parent_class_id == XG_NO_ID)
            break;
        row = producer_lookup_class_row_by_id(p, summary->parent_class_id);
    }
    return NULL;
}

static void producer_finalize_class_graph(XgProducer *p) {
    if (!p || !p->evidence)
        return;

    for (uint32_t i = 0; i < p->nclasses; i++) {
        XgClassNameRow *row = &p->classes[i];
        XgClassSummary *summary;
        if (row->summary_index >= p->evidence->nclasses)
            continue;
        summary = &p->evidence->classes[row->summary_index];
        summary->parent_class_id = producer_lookup_class(p, row->super_name);
        if (summary->parent_class_id != XG_NO_ID) {
            XgClassNameRow *parent_row =
                producer_lookup_class_row_by_id(p, summary->parent_class_id);
            if (parent_row && parent_row->summary_index < p->evidence->nclasses) {
                p->evidence->classes[parent_row->summary_index].flags |= XG_CLASS_HAS_SUBCLASS;
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
            parent_method =
                producer_find_parent_method(p, summary, method->name_id, method->signature_key);
            if (!parent_method)
                continue;
            method->override_of = parent_method->method_id;
            parent_method->flags |= XG_METHOD_OVERRIDDEN;
        }
    }
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

static bool body_push_local(XgBodyCollect *bc, const char *name, XgClassId class_id,
                            XgInterfaceId interface_id, bool inferred) {
    XgLocalType *row;
    if (!bc || !name || (class_id == XG_NO_ID && interface_id == XG_NO_ID))
        return true;
    if (!body_reserve_locals(bc, bc->nlocals + 1))
        return false;
    row = &bc->locals[bc->nlocals++];
    row->name = name;
    row->class_id = class_id;
    row->interface_id = interface_id;
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

static void body_assign_local(XgBodyCollect *bc, const char *name, XgClassId class_id,
                              XgInterfaceId interface_id) {
    XgLocalType *row = body_find_local(bc, name);
    if (class_id == XG_NO_ID && interface_id == XG_NO_ID)
        return;
    if (!row) {
        (void) body_push_local(bc, name, class_id, interface_id, true);
        return;
    }
    if (!row->inferred)
        return;
    row->class_id = class_id;
    row->interface_id = interface_id;
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

static bool body_member_receiver_is_module(const MemberAccessNode *member, const char *name) {
    if (!member || !member->object || member->object->type != AST_VARIABLE || !name)
        return false;
    return strcmp(member->object->as.variable.name, name) == 0;
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

static void collect_callsite(XgBodyCollect *bc, const AstNode *call) {
    XgCallsiteSummary row;
    const AstNode *callee;
    memset(&row, 0, sizeof(row));
    row.callsite_id = (XgCallsiteId) (bc->evidence->ncallsites + 1);
    row.owner_func_id = bc->owner_func_id;
    row.kind = XG_CALL_CLOSURE;
    row.arg_count =
        (uint16_t) (call->as.call_expr.arg_count < UINT16_MAX ? call->as.call_expr.arg_count
                                                              : UINT16_MAX);
    callee = call->as.call_expr.callee;
    if (callee && callee->type == AST_VARIABLE) {
        const char *callee_name = callee->as.variable.name;
        XgFuncId target = producer_lookup_func(bc->producer, callee->as.variable.name);
        if (producer_lookup_class(bc->producer, callee->as.variable.name) != XG_NO_ID)
            bc->capability_bits |= XG_CAP_OBJECTS;
        bc->capability_bits |= body_capabilities_for_builtin_constructor(callee_name);
        if (callee_name && strcmp(callee_name, "typename") == 0)
            bc->metadata_use_bits |= XG_METADATA_TYPENAME;
        row.kind = XG_CALL_DIRECT_FUNC;
        row.static_target_func_id =
            target != XG_NO_ID ? target : (XgFuncId) hash_name32(callee->as.variable.name);
    } else if (callee && callee->type == AST_MEMBER_ACCESS) {
        XgInterfaceId receiver_interface =
            body_resolve_expr_interface(bc, callee->as.member_access.object);
        uint32_t method_name_id = hash_name32(callee->as.member_access.name);
        bc->capability_bits |=
            body_capabilities_for_builtin_member_constructor(&callee->as.member_access);
        if (receiver_interface != XG_NO_ID) {
            row.kind = XG_CALL_INTERFACE;
            row.receiver_static_interface_id = receiver_interface;
            row.method_id = (XgMethodId) method_name_id;
            row.method_name_id = method_name_id;
            row.method_signature_key = producer_find_interface_method_signature(
                bc->producer, receiver_interface, method_name_id);
        } else {
            XgClassId receiver_class = body_resolve_expr_class(bc, callee->as.member_access.object);
            XgMethodSummary *method = producer_find_method_by_name_in_hierarchy(
                bc->producer, receiver_class, method_name_id);
            row.kind = XG_CALL_METHOD;
            row.receiver_static_class_id = receiver_class;
            row.method_id = method ? method->method_id : (XgMethodId) method_name_id;
            row.method_name_id = method ? method->name_id : method_name_id;
            row.method_signature_key = method ? method->signature_key : 0;
        }
    }
    if (bc->callsite_count == 0)
        bc->callsite_start = row.callsite_id;
    if (xg_global_evidence_add_callsite(bc->evidence, &row))
        bc->callsite_count++;
}

static void collect_super_callsite(XgBodyCollect *bc, const AstNode *call) {
    XgCallsiteSummary row;
    XgClassId parent_class;
    uint32_t method_name_id;
    XgMethodSummary *method;
    if (!bc || !call)
        return;
    parent_class = body_parent_class_id(bc);
    method_name_id = hash_name32(call->as.super_call.method_name);
    method = producer_find_method_by_name_in_hierarchy(bc->producer, parent_class, method_name_id);
    memset(&row, 0, sizeof(row));
    row.callsite_id = (XgCallsiteId) (bc->evidence->ncallsites + 1);
    row.owner_func_id = bc->owner_func_id;
    row.kind = XG_CALL_METHOD;
    row.receiver_static_class_id = parent_class;
    row.method_id = method ? method->method_id : (XgMethodId) method_name_id;
    row.method_name_id = method ? method->name_id : method_name_id;
    row.method_signature_key = method ? method->signature_key : 0;
    row.arg_count =
        (uint16_t) (call->as.super_call.arg_count < UINT16_MAX ? call->as.super_call.arg_count
                                                               : UINT16_MAX);
    if (bc->callsite_count == 0)
        bc->callsite_start = row.callsite_id;
    if (xg_global_evidence_add_callsite(bc->evidence, &row))
        bc->callsite_count++;
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
            collect_callsite(bc, node);
            walk_body_for_calls(bc, node->as.call_expr.callee);
            for (int i = 0; i < node->as.call_expr.arg_count; i++)
                walk_body_for_calls(bc, node->as.call_expr.arguments[i]);
            break;
        case AST_FUNCTION_EXPR:
            if (node->as.function_expr.is_generator)
                bc->capability_bits |= XG_CAP_GENERATOR | XG_CAP_COROUTINE;
            walk_body_for_calls(bc, node->as.function_expr.body);
            break;
        case AST_SUPER_CALL:
            collect_super_callsite(bc, node);
            for (int i = 0; i < node->as.super_call.arg_count; i++)
                walk_body_for_calls(bc, node->as.super_call.arguments[i]);
            break;
        case AST_BLOCK: {
            uint32_t base_locals = bc->nlocals;
            for (int i = 0; i < node->as.block.count; i++)
                walk_body_for_calls(bc, node->as.block.statements[i]);
            bc->nlocals = base_locals;
            break;
        }
        case AST_EXPR_STMT:
            walk_body_for_calls(bc, node->as.expr_stmt);
            break;
        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++)
                walk_body_for_calls(bc, node->as.print_stmt.exprs[i]);
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
        case AST_SHARED_DECL: {
            XgClassId class_id =
                producer_lookup_class_from_tref(bc->producer, node->as.var_decl.type_annotation);
            XgInterfaceId interface_id = producer_lookup_interface_from_tref(
                bc->producer, node->as.var_decl.type_annotation);
            bool inferred = false;
            bc->capability_bits |=
                body_capabilities_for_type_ref(node->as.var_decl.type_annotation);
            walk_body_for_calls(bc, node->as.var_decl.initializer);
            if (class_id == XG_NO_ID && interface_id == XG_NO_ID) {
                class_id = body_resolve_expr_class(bc, node->as.var_decl.initializer);
                interface_id = body_resolve_expr_interface(bc, node->as.var_decl.initializer);
                inferred = class_id != XG_NO_ID || interface_id != XG_NO_ID;
            }
            (void) body_push_local(bc, node->as.var_decl.name, class_id, interface_id, inferred);
            break;
        }
        case AST_ASSIGNMENT: {
            XgClassId class_id;
            XgInterfaceId interface_id;
            walk_body_for_calls(bc, node->as.assignment.value);
            class_id = body_resolve_expr_class(bc, node->as.assignment.value);
            interface_id = body_resolve_expr_interface(bc, node->as.assignment.value);
            body_assign_local(bc, node->as.assignment.name, class_id, interface_id);
            bc->effect_bits |= XG_BODY_MAY_MUTATE;
            break;
        }
        case AST_MEMBER_ACCESS:
            walk_body_for_calls(bc, node->as.member_access.object);
            break;
        case AST_MEMBER_SET:
            walk_body_for_calls(bc, node->as.member_set.object);
            walk_body_for_calls(bc, node->as.member_set.value);
            bc->effect_bits |= XG_BODY_MAY_MUTATE;
            break;
        case AST_INDEX_GET:
            walk_body_for_calls(bc, node->as.index_get.array);
            walk_body_for_calls(bc, node->as.index_get.index);
            break;
        case AST_INDEX_SET:
            walk_body_for_calls(bc, node->as.index_set.array);
            walk_body_for_calls(bc, node->as.index_set.index);
            walk_body_for_calls(bc, node->as.index_set.value);
            bc->effect_bits |= XG_BODY_MAY_MUTATE;
            break;
        case AST_SLICE_EXPR:
            walk_body_for_calls(bc, node->as.slice_expr.source);
            walk_body_for_calls(bc, node->as.slice_expr.start);
            walk_body_for_calls(bc, node->as.slice_expr.end);
            break;
        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++)
                walk_body_for_calls(bc, node->as.return_stmt.values[i]);
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
            bc->static_data_use_bits |= XG_STATIC_DATA_COMPTIME_VALUE;
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
            for (int i = 0; i < node->as.new_expr.arg_count; i++)
                walk_body_for_calls(bc, node->as.new_expr.arguments[i]);
            break;
        case AST_THROW_STMT:
            bc->effect_bits |= XG_BODY_MAY_THROW;
            bc->capability_bits |= XG_CAP_EXCEPTION;
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
        case AST_SELECT_CASE:
            if (node->as.select_case.is_timeout)
                bc->capability_bits |=
                    XG_CAP_TIMER | XG_CAP_CHANNEL | XG_CAP_COROUTINE | XG_CAP_OBJECTS;
            walk_body_for_calls(bc, node->as.select_case.channel);
            walk_body_for_calls(bc, node->as.select_case.value);
            walk_body_for_calls(bc, node->as.select_case.body);
            break;
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
        case AST_PARALLEL_FOR_STMT: {
            uint32_t base_locals = bc->nlocals;
            bc->effect_bits |= XG_BODY_MAY_SUSPEND | XG_BODY_MAY_THROW | XG_BODY_MAY_MUTATE;
            bc->capability_bits |= XG_CAP_COROUTINE;
            for (int i = 0; i < node->as.parallel_for_stmt.local_count; i++)
                walk_body_for_calls(bc, node->as.parallel_for_stmt.locals[i].source);
            walk_body_for_calls(bc, node->as.parallel_for_stmt.range);
            walk_body_for_calls(bc, node->as.parallel_for_stmt.worker_count);
            walk_body_for_calls(bc, node->as.parallel_for_stmt.final_body);
            walk_body_for_calls(bc, node->as.parallel_for_stmt.body);
            bc->nlocals = base_locals;
            break;
        }
        case AST_PARALLEL_REDUCE_EXPR: {
            uint32_t base_locals = bc->nlocals;
            bc->effect_bits |= XG_BODY_MAY_SUSPEND | XG_BODY_MAY_THROW | XG_BODY_MAY_MUTATE;
            bc->capability_bits |= XG_CAP_COROUTINE;
            for (int i = 0; i < node->as.parallel_reduce_expr.local_count; i++)
                walk_body_for_calls(bc, node->as.parallel_reduce_expr.locals[i].source);
            walk_body_for_calls(bc, node->as.parallel_reduce_expr.range);
            walk_body_for_calls(bc, node->as.parallel_reduce_expr.worker_count);
            walk_body_for_calls(bc, node->as.parallel_reduce_expr.initial);
            walk_body_for_calls(bc, node->as.parallel_reduce_expr.combine);
            walk_body_for_calls(bc, node->as.parallel_reduce_expr.body);
            bc->nlocals = base_locals;
            break;
        }
        case AST_PARALLEL_COLLECT_EXPR: {
            uint32_t base_locals = bc->nlocals;
            bc->effect_bits |= XG_BODY_MAY_SUSPEND | XG_BODY_MAY_THROW | XG_BODY_MAY_MUTATE;
            bc->capability_bits |= XG_CAP_COROUTINE;
            for (int i = 0; i < node->as.parallel_collect_expr.local_count; i++)
                walk_body_for_calls(bc, node->as.parallel_collect_expr.locals[i].source);
            walk_body_for_calls(bc, node->as.parallel_collect_expr.range);
            walk_body_for_calls(bc, node->as.parallel_collect_expr.worker_count);
            walk_body_for_calls(bc, node->as.parallel_collect_expr.into);
            walk_body_for_calls(bc, node->as.parallel_collect_expr.final_body);
            walk_body_for_calls(bc, node->as.parallel_collect_expr.body);
            bc->nlocals = base_locals;
            break;
        }
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
            walk_body_for_calls(bc, node->as.for_stmt.initializer);
            walk_body_for_calls(bc, node->as.for_stmt.condition);
            walk_body_for_calls(bc, node->as.for_stmt.increment);
            walk_body_for_calls(bc, node->as.for_stmt.body);
            bc->nlocals = base_locals;
            break;
        }
        case AST_FOR_IN_STMT: {
            uint32_t base_locals = bc->nlocals;
            XgClassId item_class =
                producer_lookup_class_from_tref(bc->producer, node->as.for_in_stmt.item_type);
            XgInterfaceId item_interface =
                producer_lookup_interface_from_tref(bc->producer, node->as.for_in_stmt.item_type);
            bc->capability_bits |= body_capabilities_for_type_ref(node->as.for_in_stmt.item_type);
            walk_body_for_calls(bc, node->as.for_in_stmt.collection);
            (void) body_push_local(bc, node->as.for_in_stmt.item_name, item_class, item_interface,
                                   false);
            walk_body_for_calls(bc, node->as.for_in_stmt.body);
            bc->nlocals = base_locals;
            break;
        }
        case AST_TRY_CATCH:
            bc->effect_bits |= XG_BODY_MAY_THROW;
            bc->capability_bits |= XG_CAP_EXCEPTION;
            walk_body_for_calls(bc, node->as.try_catch.try_body);
            for (int i = 0; i < node->as.try_catch.catch_count; i++)
                walk_body_for_calls(bc, node->as.try_catch.catch_clauses
                                            ? node->as.try_catch.catch_clauses[i]->body
                                            : NULL);
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
        bc->capability_bits |=
            body_capabilities_for_type_ref(method->param_types ? method->param_types[i] : NULL);
        (void) body_push_local(bc, method->parameters ? method->parameters[i] : NULL, class_id,
                               interface_id, false);
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
        bc->capability_bits |= body_capabilities_for_type_ref(param ? param->type : NULL);
        (void) body_push_local(bc, param ? param->name : NULL, class_id, interface_id, false);
    }
}

static bool add_body_summary(XgProducer *producer, const XgPendingBody *pending) {
    XgBodyCollect bc;
    XgBodySummary row;
    if (!producer || !pending || !pending->body)
        return true;
    memset(&bc, 0, sizeof(bc));
    bc.producer = producer;
    bc.evidence = producer->evidence;
    bc.owner_func_id = pending->func_id;
    bc.current_class_id = pending->current_class_id;
    body_add_method_params(&bc, pending->method);
    body_add_function_params(&bc, pending->function);
    if (pending->function && pending->function->is_generator)
        bc.capability_bits |= XG_CAP_GENERATOR | XG_CAP_COROUTINE;
    walk_body_for_calls(&bc, pending->body);

    memset(&row, 0, sizeof(row));
    row.func_id = pending->func_id;
    row.body_hash = hash_ast_shape(pending->body, XR_FNV64_OFFSET_BASIS);
    row.effect_bits = bc.effect_bits;
    row.capability_bits = bc.capability_bits;
    row.callsite_start = bc.callsite_start;
    row.callsite_count = bc.callsite_count;
    row.metadata_use_bits = bc.metadata_use_bits;
    row.static_data_use_bits = bc.static_data_use_bits;
    xr_free(bc.locals);
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
    memset(&decl, 0, sizeof(decl));
    decl.module_id = module_id;
    decl.decl_id = decl_id;
    decl.kind = XG_DECL_FUNC;
    decl.name_id = hash_name32(fn->name);
    decl.signature_key = hash_function_signature(fn);
    decl.source_span_id = (uint32_t) node->line;
    if (!xg_global_evidence_add_decl(p->evidence, &decl))
        return false;
    if (!producer_register_func(p, fn->name, func_id))
        return false;
    return producer_enqueue_body(p, func_id, XG_NO_ID, fn->body, NULL, fn);
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
    uint32_t method_start = p->evidence->nmethods + 1;
    uint32_t method_count = 0;
    uint32_t derive_flags = attrs_derive_flags(cls->attributes, cls->attr_count);
    memset(&decl, 0, sizeof(decl));
    decl.module_id = module_id;
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
        if (m->is_static)
            method.flags |= XG_METHOD_STATIC;
        if (m->is_constructor)
            method.flags |= XG_METHOD_CONSTRUCTOR;
        if (!xg_global_evidence_add_method(p->evidence, &method))
            return false;
        method_count++;
        if (!producer_enqueue_body(p, method_func_id, class_id, m->body, m, NULL))
            return false;
    }

    memset(&csum, 0, sizeof(csum));
    csum.class_id = class_id;
    csum.parent_class_id = producer_lookup_class(p, cls->super_name);
    if (cls->explicit_final)
        csum.flags |= XG_CLASS_EXPLICIT_FINAL;
    if (cls->is_native)
        csum.flags |= XG_CLASS_NATIVE;
    csum.field_start = cls->field_count > 0 ? p->field_cursor + 1 : 0;
    csum.field_count = (uint32_t) cls->field_count;
    csum.method_start = method_count > 0 ? method_start : 0;
    csum.method_count = method_count;
    csum.interface_start = cls->interface_count > 0 ? p->evidence->ninterface_impls + 1 : 0;
    csum.interface_count = (uint32_t) cls->interface_count;
    csum.decl_kind = (uint8_t) kind;
    p->field_cursor += (uint32_t) cls->field_count;
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
    return producer_register_class(p, cls->name, cls->super_name, class_id,
                                   p->evidence->nclasses - 1);
}

static bool add_interface_decl(XgProducer *p, XgModuleId module_id, const AstNode *node) {
    const InterfaceDeclNode *iface = &node->as.interface_decl;
    XgDeclSummary decl;
    memset(&decl, 0, sizeof(decl));
    decl.module_id = module_id;
    decl.decl_id = (XgDeclId) (p->evidence->ndecls + 1);
    decl.kind = XG_DECL_INTERFACE;
    decl.name_id = hash_name32(iface->name);
    decl.signature_key = (uint32_t) iface->method_count;
    decl.source_span_id = (uint32_t) node->line;
    if (!xg_global_evidence_add_decl(p->evidence, &decl))
        return false;
    return producer_register_interface(p, iface->name, iface);
}

static bool add_enum_decl(XgProducer *p, XgModuleId module_id, const AstNode *node) {
    const EnumDeclNode *e = &node->as.enum_decl;
    XgDeclSummary decl;
    uint32_t derive_flags = attrs_derive_flags(e->attributes, e->attr_count);
    memset(&decl, 0, sizeof(decl));
    decl.module_id = module_id;
    decl.decl_id = (XgDeclId) (p->evidence->ndecls + 1);
    decl.kind = XG_DECL_ENUM;
    decl.name_id = hash_name32(e->name);
    decl.signature_key = (uint32_t) e->member_count;
    decl.source_span_id = (uint32_t) node->line;
    if (derive_flags != 0)
        decl.flags |= XG_DECL_DERIVE;
    return xg_global_evidence_add_decl(p->evidence, &decl) != NULL;
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

static bool add_module_ast(XgProducer *p, XgModuleId module_id, const AstNode *ast) {
    bool has_module_body = false;
    if (!ast || ast->type != AST_PROGRAM)
        return true;
    for (int i = 0; i < ast->as.program.count; i++) {
        const AstNode *stmt = ast->as.program.statements[i];
        if (!stmt)
            continue;
        switch (stmt->type) {
            case AST_FUNCTION_DECL:
                if (!add_function_decl(p, module_id, stmt))
                    return false;
                break;
            case AST_CLASS_DECL:
                if (!add_class_like_decl(p, module_id, stmt, XG_DECL_CLASS))
                    return false;
                break;
            case AST_STRUCT_DECL:
                if (!add_class_like_decl(p, module_id, stmt, XG_DECL_STRUCT))
                    return false;
                break;
            case AST_UNION_DECL:
                if (!add_class_like_decl(p, module_id, stmt, XG_DECL_UNION))
                    return false;
                break;
            case AST_INTERFACE_DECL:
                if (!add_interface_decl(p, module_id, stmt))
                    return false;
                break;
            case AST_ENUM_DECL:
                if (!add_enum_decl(p, module_id, stmt))
                    return false;
                break;
            default:
                if (module_stmt_has_runtime_body(stmt))
                    has_module_body = true;
                break;
        }
    }
    if (has_module_body) {
        XgFuncId module_func_id = producer_next_func_id(p);
        if (!producer_enqueue_body(p, module_func_id, XG_NO_ID, ast, NULL, NULL))
            return false;
    }
    return true;
}

static uint64_t source_hash_for_graph(const XrModuleGraph *graph) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    if (!graph)
        return h;
    for (int ti = 0; ti < graph->topo_count; ti++) {
        int idx = graph->topo_order[ti];
        const XrModuleSpec *spec = &graph->specs[idx];
        size_t len = 0;
        char *source = NULL;
        uint64_t module_id = (uint64_t) (ti + 1);
        h = fold_u64(h, module_id);
        if (spec->source_path)
            h = fold_bytes(h, spec->source_path, strlen(spec->source_path));
        source = spec->source_path ? xr_file_read_all(spec->source_path, "rb", &len) : NULL;
        if (source) {
            h = fold_bytes(h, source, len);
            xr_free(source);
        }
    }
    return h;
}

static uint64_t import_hash_for_graph(const XrModuleGraph *graph) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    if (!graph)
        return h;
    for (int i = 0; i < graph->spec_count; i++) {
        const XrModuleSpec *spec = &graph->specs[i];
        if (spec->canonical)
            h = fold_bytes(h, spec->canonical, strlen(spec->canonical));
        h = fold_u64(h, (uint64_t) spec->dep_count);
        for (int d = 0; d < spec->dep_count; d++)
            h = fold_u64(h, (uint64_t) spec->dep_indices[d]);
    }
    return h;
}

XR_FUNC bool xg_global_evidence_build_from_module_graph(XgGlobalEvidence *evidence,
                                                        const XrModuleGraph *graph,
                                                        uint32_t profile) {
    XgBuildKey key;
    XgProducer producer;
    if (!evidence || !graph)
        return false;
    memset(&key, 0, sizeof(key));
    key.source_hash = source_hash_for_graph(graph);
    key.compiler_semver_hash = UINT64_C(0x0000017100000001);
    key.profile_hash = fold_u64(XR_FNV64_OFFSET_BASIS, profile);
    key.imported_summary_hash = import_hash_for_graph(graph);
    key.module_id = (XgModuleId) (graph->entry_index >= 0 ? graph->entry_index + 1 : 0);
    key.profile = profile;

    xg_global_evidence_init(evidence, key);
    memset(&producer, 0, sizeof(producer));
    producer.evidence = evidence;
    producer.next_func_id = 1;

    for (int ti = 0; ti < graph->topo_count; ti++) {
        int idx = graph->topo_order[ti];
        const XrModuleSpec *spec = &graph->specs[idx];
        if (!add_module_ast(&producer, (XgModuleId) (ti + 1), spec->ast)) {
            xr_free(producer.classes);
            xr_free(producer.interfaces);
            xr_free(producer.funcs);
            xr_free(producer.bodies);
            xg_global_evidence_free(evidence);
            return false;
        }
    }

    producer_finalize_class_graph(&producer);
    if (!producer_emit_body_summaries(&producer)) {
        xr_free(producer.classes);
        xr_free(producer.interfaces);
        xr_free(producer.funcs);
        xr_free(producer.bodies);
        xg_global_evidence_free(evidence);
        return false;
    }
    xr_free(producer.classes);
    xr_free(producer.interfaces);
    xr_free(producer.funcs);
    xr_free(producer.bodies);
    return true;
}
