/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_parallel_call_plan.c - Analyzer-owned parallel intrinsic call plan table
 */

#include "xa_parallel_call_plan.h"
#include "../../base/xchecks.h"
#include "../../base/xhash.h"
#include "../../base/xmalloc.h"
#include "../../frontend/parser/xast_nodes.h"

typedef struct XaParCallPlanEntry {
    uint32_t node_id;
    XaParallelCallPlan plan;
    struct XaParCallPlanEntry *next;
} XaParCallPlanEntry;

struct XaParallelCallPlanTable {
    XaParCallPlanEntry **buckets;
    int bucket_count;
    int size;
};

#define XA_PAR_CALL_PLAN_INITIAL_BUCKETS 64
#define XA_PAR_CALL_PLAN_LOAD_NUM 3
#define XA_PAR_CALL_PLAN_LOAD_DEN 4

static inline uint32_t hash_id(uint32_t id) {
    return xr_hash_int((int64_t) id);
}

static inline int bucket_of(const XaParallelCallPlanTable *t, uint32_t id) {
    return (int) (hash_id(id) % (uint32_t) t->bucket_count);
}

XR_FUNC XaParallelCallKind xa_parallel_call_kind_from_intrinsic(XaIntrinsicId intrinsic_id) {
    switch (intrinsic_id) {
        case XA_INTRINSIC_PARALLEL_FOR_EACH:
        case XA_INTRINSIC_PARALLEL_PLAN_FOR_EACH:
            return XA_PAR_CALL_FOR_EACH;
        case XA_INTRINSIC_PARALLEL_MAP:
        case XA_INTRINSIC_PARALLEL_PLAN_MAP:
            return XA_PAR_CALL_MAP;
        case XA_INTRINSIC_PARALLEL_MAP_INTO:
        case XA_INTRINSIC_PARALLEL_PLAN_MAP_INTO:
            return XA_PAR_CALL_MAP_INTO;
        case XA_INTRINSIC_PARALLEL_REDUCE:
        case XA_INTRINSIC_PARALLEL_PLAN_REDUCE:
            return XA_PAR_CALL_REDUCE;
        default:
            return XA_PAR_CALL_NONE;
    }
}

XR_FUNC const char *xa_parallel_call_kind_name(XaParallelCallKind kind) {
    switch (kind) {
        case XA_PAR_CALL_FOR_EACH:
            return "forEach";
        case XA_PAR_CALL_MAP:
            return "map";
        case XA_PAR_CALL_MAP_INTO:
            return "mapInto";
        case XA_PAR_CALL_REDUCE:
            return "reduce";
        case XA_PAR_CALL_NONE:
        default:
            return NULL;
    }
}

XR_FUNC XaParallelCallPlanTable *xa_parallel_call_plan_table_new(void) {
    XaParallelCallPlanTable *t =
        (XaParallelCallPlanTable *) xr_malloc(sizeof(XaParallelCallPlanTable));
    if (!t)
        return NULL;
    t->bucket_count = XA_PAR_CALL_PLAN_INITIAL_BUCKETS;
    t->buckets = (XaParCallPlanEntry **) xr_calloc(t->bucket_count, sizeof(XaParCallPlanEntry *));
    if (!t->buckets) {
        xr_free(t);
        return NULL;
    }
    t->size = 0;
    return t;
}

XR_FUNC void xa_parallel_call_plan_table_free(XaParallelCallPlanTable *t) {
    if (!t)
        return;
    xa_parallel_call_plan_table_clear(t);
    xr_free(t->buckets);
    xr_free(t);
}

XR_FUNC void xa_parallel_call_plan_table_clear(XaParallelCallPlanTable *t) {
    if (!t)
        return;
    for (int i = 0; i < t->bucket_count; i++) {
        XaParCallPlanEntry *e = t->buckets[i];
        while (e) {
            XaParCallPlanEntry *next = e->next;
            xr_free(e);
            e = next;
        }
        t->buckets[i] = NULL;
    }
    t->size = 0;
}

XR_FUNC int xa_parallel_call_plan_table_size(const XaParallelCallPlanTable *t) {
    return t ? t->size : 0;
}

static void grow(XaParallelCallPlanTable *t) {
    int new_count = t->bucket_count * 2;
    XaParCallPlanEntry **new_buckets =
        (XaParCallPlanEntry **) xr_calloc(new_count, sizeof(XaParCallPlanEntry *));
    if (!new_buckets)
        return;

    for (int i = 0; i < t->bucket_count; i++) {
        XaParCallPlanEntry *e = t->buckets[i];
        while (e) {
            XaParCallPlanEntry *next = e->next;
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

XR_FUNC void xa_parallel_call_plan_table_set(XaParallelCallPlanTable *t, struct AstNode *node,
                                             const XaParallelCallPlan *plan) {
    XR_DCHECK(t != NULL, "xa_parallel_call_plan_table_set: NULL table");
    XR_DCHECK(node != NULL, "xa_parallel_call_plan_table_set: NULL node");
    XR_DCHECK(plan != NULL, "xa_parallel_call_plan_table_set: NULL plan");

    if (plan->kind == XA_PAR_CALL_NONE)
        return;

    uint32_t id = node->node_id;
    int b = bucket_of(t, id);
    for (XaParCallPlanEntry *e = t->buckets[b]; e; e = e->next) {
        if (e->node_id == id) {
            e->plan = *plan;
            return;
        }
    }

    XaParCallPlanEntry *e = (XaParCallPlanEntry *) xr_malloc(sizeof(XaParCallPlanEntry));
    if (!e)
        return;
    e->node_id = id;
    e->plan = *plan;
    e->next = t->buckets[b];
    t->buckets[b] = e;
    t->size++;

    if ((int64_t) t->size * XA_PAR_CALL_PLAN_LOAD_DEN >
        (int64_t) t->bucket_count * XA_PAR_CALL_PLAN_LOAD_NUM) {
        grow(t);
    }
}

XR_FUNC const XaParallelCallPlan *xa_parallel_call_plan_table_get(const XaParallelCallPlanTable *t,
                                                                  const struct AstNode *node) {
    if (!t || !node)
        return NULL;

    uint32_t id = node->node_id;
    int b = bucket_of(t, id);
    for (const XaParCallPlanEntry *e = t->buckets[b]; e; e = e->next) {
        if (e->node_id == id)
            return &e->plan;
    }
    return NULL;
}
