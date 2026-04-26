/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_node_table.c - AST -> XrType side table implementation
 *
 * IMPLEMENTATION NOTE:
 *   The map uses pointer-keyed open chaining (linked list per bucket)
 *   with a 64-entry initial capacity and 0.75 load-factor growth. The
 *   pointer hash is a 64-bit avalanche mix on (uintptr_t)node so the
 *   typically aligned AST pointers do not cluster in low-order bits.
 *
 *   Allocation is via xr_malloc / xr_free per project rules. No arena
 *   here: the table outlives any single AST traversal.
 */

#include "xa_node_table.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../base/xhash.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct XaNodeEntry {
    struct AstNode *node;
    struct XrType *type;
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

// Reuse the project's canonical splitmix64 integer hash (from
// src/base/xhash.h). Casting AstNode * through uintptr_t to int64_t is
// the standard pattern; xr_hash_int absorbs the high pointer bits via
// its finalizer-style multiply-xor.
static inline uint32_t hash_node_ptr(const void *p) {
    return xr_hash_int((int64_t) (uintptr_t) p);
}

static inline int bucket_of(const XaNodeTable *t, const void *p) {
    return (int) (hash_node_ptr(p) % (uint32_t) t->bucket_count);
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
            int b = (int) (hash_node_ptr(e->node) % (uint32_t) new_count);
            e->next = new_buckets[b];
            new_buckets[b] = e;
            e = next;
        }
    }
    xr_free(t->buckets);
    t->buckets = new_buckets;
    t->bucket_count = new_count;
}

void xa_node_table_set_type(XaNodeTable *t, struct AstNode *node, struct XrType *type) {
    if (!t || !node)
        return;

    int b = bucket_of(t, node);
    XaNodeEntry **pp = &t->buckets[b];
    while (*pp) {
        if ((*pp)->node == node) {
            if (type == NULL) {
                // Clear: drop the entry entirely so size and lookup
                // stay symmetric with the "never set" case.
                XaNodeEntry *to_free = *pp;
                *pp = to_free->next;
                xr_free(to_free);
                t->size--;
            } else {
                (*pp)->type = type;
            }
            return;
        }
        pp = &(*pp)->next;
    }

    if (type == NULL)
        return;  // Nothing to clear.

    XaNodeEntry *e = (XaNodeEntry *) xr_malloc(sizeof(XaNodeEntry));
    if (!e)
        return;
    e->node = node;
    e->type = type;
    e->next = t->buckets[b];
    t->buckets[b] = e;
    t->size++;

    // Grow on load-factor breach. Linear over t->bucket_count, so the
    // chain length stays O(1) amortised.
    if ((int64_t) t->size * XA_NODE_TABLE_LOAD_DEN >
        (int64_t) t->bucket_count * XA_NODE_TABLE_LOAD_NUM) {
        xa_node_table_grow(t);
    }
}

struct XrType *xa_node_table_get_type(const XaNodeTable *t, const struct AstNode *node) {
    if (!t || !node)
        return NULL;
    int b = (int) (hash_node_ptr(node) % (uint32_t) t->bucket_count);
    for (XaNodeEntry *e = t->buckets[b]; e; e = e->next) {
        if (e->node == node)
            return e->type;
    }
    return NULL;
}
