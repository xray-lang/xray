/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_cha.c - Class Hierarchy Analysis implementation
 */

#include "xanalyzer_cha.h"
#include "../../base/xmalloc.h"
#include <string.h>

/* ========== Internal Helpers ========== */

static XaChaNode *find_node_by_name(const XaClassHierarchy *cha, const char *name) {
    if (!cha || !name)
        return NULL;
    for (uint32_t i = 0; i < cha->nnodes; i++) {
        XaChaNode *node = &cha->nodes[i];
        if (node->info && node->info->name && strcmp(node->info->name, name) == 0)
            return node;
    }
    return NULL;
}

static bool add_child(XaChaNode *parent, XaChaNode *child) {
    if (!parent || !child)
        return false;
    if (parent->nchildren >= XA_CHA_MAX_SUBCLASSES)
        return false;
    if (parent->nchildren >= parent->children_cap) {
        uint32_t new_cap = parent->children_cap ? parent->children_cap * 2 : 4;
        if (new_cap > XA_CHA_MAX_SUBCLASSES)
            new_cap = XA_CHA_MAX_SUBCLASSES;
        XaChaNode **new_children =
            (XaChaNode **) xr_realloc(parent->children, new_cap * sizeof(XaChaNode *));
        if (!new_children)
            return false;
        parent->children = new_children;
        parent->children_cap = new_cap;
    }
    parent->children[parent->nchildren++] = child;
    parent->is_leaf = false;
    return true;
}

static uint32_t count_subclasses_recursive(const XaChaNode *node) {
    if (!node)
        return 0;
    uint32_t count = node->nchildren;
    for (uint32_t i = 0; i < node->nchildren; i++) {
        count += count_subclasses_recursive(node->children[i]);
    }
    return count;
}

/* Check if a class directly defines (not inherits) a method with the
 * given name.  Uses the XrClassInfo vtable if available. */
static bool class_defines_method(const XrClassInfo *info, const char *method_name) {
    if (!info || !method_name)
        return false;
    if (info->vtable) {
        for (int i = 0; i < info->vtable_size; i++) {
            if (info->vtable[i].name && strcmp(info->vtable[i].name, method_name) == 0)
                return true;
        }
    }
    return false;
}

/* Collect all implementors of a method among a node and its descendants. */
static uint32_t collect_implementors(const XaChaNode *node, const char *method_name,
                                     const XrClassInfo **out, uint32_t max_out) {
    if (!node || max_out == 0)
        return 0;
    uint32_t count = 0;
    if (class_defines_method(node->info, method_name)) {
        out[count++] = node->info;
        if (count >= max_out)
            return count;
    }
    for (uint32_t i = 0; i < node->nchildren; i++) {
        count += collect_implementors(node->children[i], method_name, out + count, max_out - count);
        if (count >= max_out)
            return count;
    }
    return count;
}

/* ========== Public API ========== */

XR_FUNC bool xa_cha_build(XaClassHierarchy *cha, XrClassInfo **infos, uint32_t ninfos) {
    if (!cha)
        return false;
    memset(cha, 0, sizeof(*cha));
    if (ninfos == 0 || !infos)
        return true;

    cha->nodes = (XaChaNode *) xr_calloc(ninfos, sizeof(XaChaNode));
    if (!cha->nodes)
        return false;
    cha->nodes_cap = ninfos;

    for (uint32_t i = 0; i < ninfos; i++) {
        if (!infos[i])
            continue;
        XaChaNode *node = &cha->nodes[cha->nnodes++];
        node->info = infos[i];
        node->parent = NULL;
        node->children = NULL;
        node->nchildren = 0;
        node->children_cap = 0;
        node->is_leaf = true;
    }

    /* Link parent-child relationships based on base_name. */
    for (uint32_t i = 0; i < cha->nnodes; i++) {
        XaChaNode *node = &cha->nodes[i];
        if (!node->info->base_name)
            continue;
        XaChaNode *parent = find_node_by_name(cha, node->info->base_name);
        if (!parent)
            continue;
        node->parent = parent;
        add_child(parent, node);
        if (node->info->base == NULL)
            node->info->base = parent->info;
        parent->info->has_subclass = true;
    }

    cha->version = 1;
    return true;
}

XR_FUNC void xa_cha_free(XaClassHierarchy *cha) {
    if (!cha)
        return;
    for (uint32_t i = 0; i < cha->nnodes; i++) {
        xr_free(cha->nodes[i].children);
    }
    xr_free(cha->nodes);
    memset(cha, 0, sizeof(*cha));
}

XR_FUNC bool xa_cha_is_leaf(const XaClassHierarchy *cha, const XrClassInfo *info) {
    if (!cha || !info)
        return false;
    XaChaNode *node = xa_cha_find_node(cha, info);
    return node ? node->is_leaf : false;
}

XR_FUNC const XrClassInfo *xa_cha_single_implementor(const XaClassHierarchy *cha,
                                                     const XrClassInfo *info,
                                                     const char *method_name) {
    if (!cha || !info || !method_name)
        return NULL;
    XaChaNode *node = xa_cha_find_node(cha, info);
    if (!node)
        return NULL;

    const XrClassInfo *results[2] = {NULL, NULL};
    uint32_t count = collect_implementors(node, method_name, results, 2);
    if (count == 1)
        return results[0];
    return NULL;
}

XR_FUNC uint32_t xa_cha_subclass_count(const XaClassHierarchy *cha, const XrClassInfo *info) {
    if (!cha || !info)
        return 0;
    XaChaNode *node = xa_cha_find_node(cha, info);
    if (!node)
        return 0;
    return count_subclasses_recursive(node);
}

XR_FUNC void xa_cha_invalidate(XaClassHierarchy *cha) {
    if (cha)
        cha->version++;
}

XR_FUNC XaChaNode *xa_cha_find_node(const XaClassHierarchy *cha, const XrClassInfo *info) {
    if (!cha || !info)
        return NULL;
    for (uint32_t i = 0; i < cha->nnodes; i++) {
        if (cha->nodes[i].info == info)
            return &cha->nodes[i];
    }
    return NULL;
}
