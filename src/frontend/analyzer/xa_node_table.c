/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_node_table.c - AST -> semantic info side table implementation
 *
 * IMPLEMENTATION NOTE:
 *   The map uses node_id-keyed open chaining (linked list per bucket)
 *   with a 64-entry initial capacity and 0.75 load-factor growth.
 *   Keys are uint32_t AstNode.node_id (stable monotonic IDs).
 *
 *   Allocation is via xr_malloc / xr_free per project rules. No arena
 *   here: the table outlives any single AST traversal.
 */

#include "xa_node_table.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../base/xhash.h"
#include "../../frontend/parser/xast_nodes.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct XaNodeEntry {
    uint32_t node_id;  // key: AstNode.node_id
    struct XrType *type;
    struct XaScope *scope;    // enclosing scope at this node
    struct XaSymbol *symbol;  // resolved symbol (NULL for non-binding nodes)
    bool has_ct_value;
    XrCtValue ct_value;
    bool has_conversion;
    XrConversionWitness conversion;
    struct XaNodeEntry *next;
} XaNodeEntry;

struct XaNodeTable {
    XaNodeEntry **buckets;
    int bucket_count;
    int size;
};

#define XA_NODE_TABLE_INITIAL_BUCKETS 64
#define XA_NODE_TABLE_LOAD_NUM 3
#define XA_NODE_TABLE_LOAD_DEN 4
#define XA_NODE_TABLE_GROWTH 2

static inline uint32_t hash_node_id(uint32_t id) {
    return xr_hash_int((int64_t) id);
}

static inline int bucket_of(const XaNodeTable *t, uint32_t id) {
    return (int) (hash_node_id(id) % (uint32_t) t->bucket_count);
}

XaNodeTable *xa_node_table_new(void) {
    XaNodeTable *t = (XaNodeTable *) xr_malloc(sizeof(XaNodeTable));
    if (!t)
        return NULL;
    t->bucket_count = XA_NODE_TABLE_INITIAL_BUCKETS;
    t->buckets = (XaNodeEntry **) xr_calloc(t->bucket_count, sizeof(XaNodeEntry *));
    if (!t->buckets) {
        xr_free(t);
        return NULL;
    }
    t->size = 0;
    return t;
}

void xa_node_table_free(XaNodeTable *t) {
    if (!t)
        return;
    for (int i = 0; i < t->bucket_count; i++) {
        XaNodeEntry *e = t->buckets[i];
        while (e) {
            XaNodeEntry *next = e->next;
            xr_free(e);
            e = next;
        }
    }
    xr_free(t->buckets);
    xr_free(t);
}

void xa_node_table_clear(XaNodeTable *t) {
    if (!t)
        return;
    for (int i = 0; i < t->bucket_count; i++) {
        XaNodeEntry *e = t->buckets[i];
        while (e) {
            XaNodeEntry *next = e->next;
            xr_free(e);
            e = next;
        }
        t->buckets[i] = NULL;
    }
    t->size = 0;
}

int xa_node_table_size(const XaNodeTable *t) {
    return t ? t->size : 0;
}

static void xa_node_table_grow(XaNodeTable *t) {
    int new_count = t->bucket_count * XA_NODE_TABLE_GROWTH;
    XaNodeEntry **new_buckets = (XaNodeEntry **) xr_calloc(new_count, sizeof(XaNodeEntry *));
    if (!new_buckets)
        return;  // Best-effort: keep old buckets, take the
                 // hit on chain length.

    for (int i = 0; i < t->bucket_count; i++) {
        XaNodeEntry *e = t->buckets[i];
        while (e) {
            XaNodeEntry *next = e->next;
            int b = (int) (hash_node_id(e->node_id) % (uint32_t) new_count);
            e->next = new_buckets[b];
            new_buckets[b] = e;
            e = next;
        }
    }
    xr_free(t->buckets);
    t->buckets = new_buckets;
    t->bucket_count = new_count;
}

/* Find or create an entry for the given node_id. Returns NULL only on
 * allocation failure when creating a new entry. */
static XaNodeEntry *find_or_create(XaNodeTable *t, uint32_t id) {
    int b = bucket_of(t, id);
    for (XaNodeEntry *e = t->buckets[b]; e; e = e->next) {
        if (e->node_id == id)
            return e;
    }

    XaNodeEntry *e = (XaNodeEntry *) xr_malloc(sizeof(XaNodeEntry));
    if (!e)
        return NULL;
    memset(e, 0, sizeof(*e));
    e->node_id = id;
    e->next = t->buckets[b];
    t->buckets[b] = e;
    t->size++;

    if ((int64_t) t->size * XA_NODE_TABLE_LOAD_DEN >
        (int64_t) t->bucket_count * XA_NODE_TABLE_LOAD_NUM) {
        xa_node_table_grow(t);
    }
    return e;
}

static const XaNodeEntry *find_entry(const XaNodeTable *t, uint32_t id) {
    int b = bucket_of(t, id);
    for (const XaNodeEntry *e = t->buckets[b]; e; e = e->next) {
        if (e->node_id == id)
            return e;
    }
    return NULL;
}

static bool entry_has_no_facts(const XaNodeEntry *e) {
    return e && !e->type && !e->scope && !e->symbol && !e->has_ct_value && !e->has_conversion;
}

static void remove_entry_by_id(XaNodeTable *t, uint32_t id) {
    if (!t)
        return;
    int b = bucket_of(t, id);
    XaNodeEntry **pp = &t->buckets[b];
    while (*pp) {
        if ((*pp)->node_id == id) {
            XaNodeEntry *to_free = *pp;
            *pp = to_free->next;
            xr_free(to_free);
            t->size--;
            return;
        }
        pp = &(*pp)->next;
    }
}

void xa_node_table_set_type(XaNodeTable *t, struct AstNode *node, struct XrType *type) {
    if (!t || !node)
        return;
    uint32_t id = node->node_id;

    if (type == NULL) {
        /* Historical API: clearing the type drops the whole node entry. */
        remove_entry_by_id(t, id);
        return;
    }

    XaNodeEntry *e = find_or_create(t, id);
    if (e)
        e->type = type;
}

struct XrType *xa_node_table_get_type(const XaNodeTable *t, const struct AstNode *node) {
    if (!t || !node)
        return NULL;
    const XaNodeEntry *e = find_entry(t, node->node_id);
    return e ? e->type : NULL;
}

void xa_node_table_set(XaNodeTable *t, struct AstNode *node, struct XrType *type,
                       struct XaScope *scope, struct XaSymbol *symbol) {
    if (!t || !node)
        return;
    XaNodeEntry *e = find_or_create(t, node->node_id);
    if (!e)
        return;
    e->type = type;
    e->scope = scope;
    e->symbol = symbol;
}

struct XaScope *xa_node_table_get_scope(const XaNodeTable *t, const struct AstNode *node) {
    if (!t || !node)
        return NULL;
    const XaNodeEntry *e = find_entry(t, node->node_id);
    return e ? e->scope : NULL;
}

struct XaSymbol *xa_node_table_get_symbol(const XaNodeTable *t, const struct AstNode *node) {
    if (!t || !node)
        return NULL;
    const XaNodeEntry *e = find_entry(t, node->node_id);
    return e ? e->symbol : NULL;
}

void xa_node_table_set_ct_value(XaNodeTable *t, const struct AstNode *node,
                                const XrCtValue *value) {
    if (!t || !node)
        return;
    if (!value) {
        XaNodeEntry *e = (XaNodeEntry *) find_entry(t, node->node_id);
        if (!e)
            return;
        e->has_ct_value = false;
        e->ct_value = (XrCtValue) {0};
        if (entry_has_no_facts(e))
            remove_entry_by_id(t, node->node_id);
        return;
    }

    XaNodeEntry *e = find_or_create(t, node->node_id);
    if (!e)
        return;
    e->has_ct_value = true;
    e->ct_value = *value;
}

bool xa_node_table_get_ct_value(const XaNodeTable *t, const struct AstNode *node,
                                XrCtValue *out_value) {
    if (!t || !node)
        return false;
    const XaNodeEntry *e = find_entry(t, node->node_id);
    if (!e || !e->has_ct_value)
        return false;
    if (out_value)
        *out_value = e->ct_value;
    return true;
}

void xa_node_table_set_conversion(XaNodeTable *t, const struct AstNode *node,
                                  const XrConversionWitness *witness) {
    if (!t || !node)
        return;
    if (!witness || witness->kind == XR_CONVERSION_NONE) {
        XaNodeEntry *e = (XaNodeEntry *) find_entry(t, node->node_id);
        if (!e)
            return;
        e->has_conversion = false;
        e->conversion = (XrConversionWitness) {0};
        if (entry_has_no_facts(e))
            remove_entry_by_id(t, node->node_id);
        return;
    }
    XaNodeEntry *e = find_or_create(t, node->node_id);
    if (!e)
        return;
    e->has_conversion = true;
    e->conversion = *witness;
}

bool xa_node_table_get_conversion(const XaNodeTable *t, const struct AstNode *node,
                                  XrConversionWitness *out_witness) {
    if (!t || !node)
        return false;
    const XaNodeEntry *e = find_entry(t, node->node_id);
    if (!e || !e->has_conversion)
        return false;
    if (out_witness)
        *out_witness = e->conversion;
    return true;
}

static int compare_node_conversion_entry(const void *left, const void *right) {
    const XaNodeConversionEntry *a = (const XaNodeConversionEntry *) left;
    const XaNodeConversionEntry *b = (const XaNodeConversionEntry *) right;
    return a->node_id < b->node_id ? -1 : a->node_id > b->node_id ? 1 : 0;
}

bool xa_node_table_snapshot_conversions(const XaNodeTable *t,
                                        XaNodeConversionEntry **out_entries,
                                        uint32_t *out_count) {
    if (!out_entries || !out_count)
        return false;
    *out_entries = NULL;
    *out_count = 0;
    if (!t)
        return false;

    uint32_t count = 0;
    for (int i = 0; i < t->bucket_count; i++) {
        for (const XaNodeEntry *entry = t->buckets[i]; entry; entry = entry->next) {
            if (entry->has_conversion)
                count++;
        }
    }
    if (count == 0)
        return true;

    XaNodeConversionEntry *entries =
        (XaNodeConversionEntry *) xr_malloc(sizeof(*entries) * (size_t) count);
    if (!entries)
        return false;
    uint32_t index = 0;
    for (int i = 0; i < t->bucket_count; i++) {
        for (const XaNodeEntry *entry = t->buckets[i]; entry; entry = entry->next) {
            if (!entry->has_conversion)
                continue;
            entries[index].node_id = entry->node_id;
            entries[index].witness = entry->conversion;
            index++;
        }
    }
    qsort(entries, count, sizeof(*entries), compare_node_conversion_entry);
    *out_entries = entries;
    *out_count = count;
    return true;
}
