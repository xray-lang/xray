/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_selection.c - Member/method/index selection table implementation
 *
 * IMPLEMENTATION NOTE:
 *   Same hash-table design as xa_node_table.c: open chaining keyed by
 *   AstNode.node_id, 64-entry initial capacity, 0.75 load-factor growth.
 */

#include "xa_selection.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../base/xhash.h"
#include "../../frontend/parser/xast_nodes.h"
#include <string.h>

typedef struct XaSelEntry {
    uint32_t node_id;
    XaSelection sel;
    struct XaSelEntry *next;
} XaSelEntry;

struct XaSelectionTable {
    XaSelEntry **buckets;
    int bucket_count;
    int size;
};

#define XA_SEL_INITIAL_BUCKETS 64
#define XA_SEL_LOAD_NUM 3
#define XA_SEL_LOAD_DEN 4

static inline uint32_t hash_id(uint32_t id) {
    return xr_hash_int((int64_t) id);
}

static inline int bucket_of(const XaSelectionTable *t, uint32_t id) {
    return (int) (hash_id(id) % (uint32_t) t->bucket_count);
}

XR_FUNC XaSelectionTable *xa_selection_table_new(void) {
    XaSelectionTable *t = (XaSelectionTable *) xr_malloc(sizeof(XaSelectionTable));
    if (!t)
        return NULL;
    t->bucket_count = XA_SEL_INITIAL_BUCKETS;
    t->buckets = (XaSelEntry **) xr_calloc(t->bucket_count, sizeof(XaSelEntry *));
    if (!t->buckets) {
        xr_free(t);
        return NULL;
    }
    t->size = 0;
    return t;
}

XR_FUNC void xa_selection_table_free(XaSelectionTable *t) {
    if (!t)
        return;
    for (int i = 0; i < t->bucket_count; i++) {
        XaSelEntry *e = t->buckets[i];
        while (e) {
            XaSelEntry *next = e->next;
            xr_free(e);
            e = next;
        }
    }
    xr_free(t->buckets);
    xr_free(t);
}

XR_FUNC void xa_selection_table_clear(XaSelectionTable *t) {
    if (!t)
        return;
    for (int i = 0; i < t->bucket_count; i++) {
        XaSelEntry *e = t->buckets[i];
        while (e) {
            XaSelEntry *next = e->next;
            xr_free(e);
            e = next;
        }
        t->buckets[i] = NULL;
    }
    t->size = 0;
}

XR_FUNC int xa_selection_table_size(const XaSelectionTable *t) {
    return t ? t->size : 0;
}

static void grow(XaSelectionTable *t) {
    int new_count = t->bucket_count * 2;
    XaSelEntry **new_buckets = (XaSelEntry **) xr_calloc(new_count, sizeof(XaSelEntry *));
    if (!new_buckets)
        return;

    for (int i = 0; i < t->bucket_count; i++) {
        XaSelEntry *e = t->buckets[i];
        while (e) {
            XaSelEntry *next = e->next;
            int b = (int) (hash_id(e->node_id) % (uint32_t) new_count);
            e->next = new_buckets[b];
            new_buckets[b] = e;
            e = next;
        }
    }
    xr_free(t->buckets);
    t->buckets = new_buckets;
    t->bucket_count = new_count;
}

XR_FUNC void xa_selection_table_set(XaSelectionTable *t, struct AstNode *node,
                                    const XaSelection *sel) {
    XR_DCHECK(t != NULL, "xa_selection_table_set: NULL table");
    XR_DCHECK(node != NULL, "xa_selection_table_set: NULL node");
    XR_DCHECK(sel != NULL, "xa_selection_table_set: NULL selection");

    uint32_t id = node->node_id;
    int b = bucket_of(t, id);

    /* Check for existing entry */
    for (XaSelEntry *e = t->buckets[b]; e; e = e->next) {
        if (e->node_id == id) {
            e->sel = *sel;
            return;
        }
    }

    /* Create new entry */
    XaSelEntry *e = (XaSelEntry *) xr_malloc(sizeof(XaSelEntry));
    if (!e)
        return;
    e->node_id = id;
    e->sel = *sel;
    e->next = t->buckets[b];
    t->buckets[b] = e;
    t->size++;

    if ((int64_t) t->size * XA_SEL_LOAD_DEN > (int64_t) t->bucket_count * XA_SEL_LOAD_NUM) {
        grow(t);
    }
}

XR_FUNC const XaSelection *xa_selection_table_get(const XaSelectionTable *t,
                                                  const struct AstNode *node) {
    if (!t || !node)
        return NULL;

    uint32_t id = node->node_id;
    int b = bucket_of(t, id);

    for (const XaSelEntry *e = t->buckets[b]; e; e = e->next) {
        if (e->node_id == id)
            return &e->sel;
    }
    return NULL;
}
