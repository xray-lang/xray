/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu
 * Licensed under the MIT License
 */

#include "xa_resolved_call.h"
#include "../../base/xchecks.h"
#include "../../base/xhash.h"
#include "../../base/xmalloc.h"
#include "../parser/xast_nodes.h"

typedef struct XaResolvedCallEntry {
    uint32_t node_id;
    XaResolvedCall call;
    struct XaResolvedCallEntry *next;
} XaResolvedCallEntry;

struct XaResolvedCallTable {
    XaResolvedCallEntry **buckets;
    uint32_t bucket_count;
    int size;
};

static uint32_t bucket_index(const XaResolvedCallTable *table, uint32_t node_id) {
    return xr_hash_int((int64_t) node_id) % table->bucket_count;
}

XaResolvedCallTable *xa_resolved_call_table_new(void) {
    XaResolvedCallTable *table = (XaResolvedCallTable *) xr_calloc(1, sizeof(*table));
    if (!table)
        return NULL;
    table->bucket_count = 64;
    table->buckets =
        (XaResolvedCallEntry **) xr_calloc(table->bucket_count, sizeof(*table->buckets));
    if (!table->buckets) {
        xr_free(table);
        return NULL;
    }
    return table;
}

void xa_resolved_call_table_free(XaResolvedCallTable *table) {
    if (!table)
        return;
    for (uint32_t i = 0; i < table->bucket_count; i++) {
        XaResolvedCallEntry *entry = table->buckets[i];
        while (entry) {
            XaResolvedCallEntry *next = entry->next;
            xr_free(entry);
            entry = next;
        }
    }
    xr_free(table->buckets);
    xr_free(table);
}

static void grow(XaResolvedCallTable *table) {
    uint32_t new_count = table->bucket_count * 2;
    XaResolvedCallEntry **buckets = (XaResolvedCallEntry **) xr_calloc(new_count, sizeof(*buckets));
    if (!buckets)
        return;
    for (uint32_t i = 0; i < table->bucket_count; i++) {
        XaResolvedCallEntry *entry = table->buckets[i];
        while (entry) {
            XaResolvedCallEntry *next = entry->next;
            uint32_t b = xr_hash_int((int64_t) entry->node_id) % new_count;
            entry->next = buckets[b];
            buckets[b] = entry;
            entry = next;
        }
    }
    xr_free(table->buckets);
    table->buckets = buckets;
    table->bucket_count = new_count;
}

void xa_resolved_call_table_set(XaResolvedCallTable *table, const struct AstNode *node,
                                const XaResolvedCall *call) {
    XR_DCHECK(table && node && call, "xa_resolved_call_table_set: invalid input");
    if (!table || !node || !call)
        return;
    uint32_t b = bucket_index(table, node->node_id);
    for (XaResolvedCallEntry *entry = table->buckets[b]; entry; entry = entry->next) {
        if (entry->node_id == node->node_id) {
            entry->call = *call;
            return;
        }
    }
    XaResolvedCallEntry *entry = (XaResolvedCallEntry *) xr_malloc(sizeof(*entry));
    if (!entry)
        return;
    entry->node_id = node->node_id;
    entry->call = *call;
    entry->next = table->buckets[b];
    table->buckets[b] = entry;
    table->size++;
    if ((uint32_t) table->size * 4 > table->bucket_count * 3)
        grow(table);
}

const XaResolvedCall *xa_resolved_call_table_get(const XaResolvedCallTable *table,
                                                 const struct AstNode *node) {
    if (!table || !node)
        return NULL;
    uint32_t b = bucket_index(table, node->node_id);
    for (const XaResolvedCallEntry *entry = table->buckets[b]; entry; entry = entry->next) {
        if (entry->node_id == node->node_id)
            return &entry->call;
    }
    return NULL;
}

int xa_resolved_call_table_size(const XaResolvedCallTable *table) {
    return table ? table->size : 0;
}
