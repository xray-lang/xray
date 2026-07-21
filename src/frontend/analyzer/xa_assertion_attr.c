/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_assertion_attr.c - X-macro instantiation and shared lookups for the
 * @no_* system assertion attribute family (task 217 §3.1).
 */

#include "../parser/xa_assertion_attr.h"
#include "../parser/xast_nodes.h"
#include <string.h>

static const XaAssertionAttrInfo k_assertion_attrs[] = {
#define XA_ASSERTION_ATTR(name_id, attr_enum, proof, stage, positions, fn_type)                    \
    {#name_id, sizeof(#name_id) - 1,   (attr_enum), (proof),                                       \
     (stage),  (unsigned) (positions), (fn_type)},
#include "xa_assertion_attr.def"
#undef XA_ASSERTION_ATTR
};

static const size_t k_assertion_attr_count =
    sizeof(k_assertion_attrs) / sizeof(k_assertion_attrs[0]);

const XaAssertionAttrInfo *xa_assertion_attr_by_name(const char *name, int length) {
    if (!name || length < 0)
        return NULL;
    for (size_t i = 0; i < k_assertion_attr_count; i++) {
        const XaAssertionAttrInfo *info = &k_assertion_attrs[i];
        if ((size_t) length == info->name_length &&
            memcmp(name, info->name, info->name_length) == 0)
            return info;
    }
    return NULL;
}

const XaAssertionAttrInfo *xa_assertion_attr_by_kind(AttributeKind kind) {
    for (size_t i = 0; i < k_assertion_attr_count; i++) {
        if (k_assertion_attrs[i].kind == kind)
            return &k_assertion_attrs[i];
    }
    return NULL;
}

bool xa_attribute_kind_is_assertion(AttributeKind kind) {
    return xa_assertion_attr_by_kind(kind) != NULL;
}

size_t xa_assertion_attr_count(void) {
    return k_assertion_attr_count;
}

const XaAssertionAttrInfo *xa_assertion_attr_at(size_t index) {
    return index < k_assertion_attr_count ? &k_assertion_attrs[index] : NULL;
}

bool xa_assertion_attr_allows_position(AttributeKind kind, XaAssertionPosition position) {
    const XaAssertionAttrInfo *info = xa_assertion_attr_by_kind(kind);
    return info && (info->positions & (unsigned) position) != 0;
}

bool xa_decl_attribute_list(const AstNode *node, XrAttribute ***out_attrs, int *out_count) {
    if (!node)
        return false;
    XrAttribute **attrs = NULL;
    int count = 0;
    switch (node->type) {
        case AST_FUNCTION_DECL:
            attrs = node->as.function_decl.attributes;
            count = node->as.function_decl.attr_count;
            break;
        case AST_FUNCTION_EXPR:
            attrs = node->as.function_expr.attributes;
            count = node->as.function_expr.attr_count;
            break;
        case AST_METHOD_DECL:
            attrs = node->as.method_decl.attributes;
            count = node->as.method_decl.attr_count;
            break;
        default:
            return false;
    }
    if (out_attrs)
        *out_attrs = attrs;
    if (out_count)
        *out_count = count;
    return true;
}

bool xa_decl_has_attribute(const AstNode *node, AttributeKind kind) {
    XrAttribute **attrs = NULL;
    int count = 0;
    if (!xa_decl_attribute_list(node, &attrs, &count))
        return false;
    for (int i = 0; i < count; i++) {
        if (attrs[i] && attrs[i]->kind == kind)
            return true;
    }
    return false;
}
