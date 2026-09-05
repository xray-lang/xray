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
    bool has_call_error_effect;
    XaCallErrorEffectFact call_error_effect;
    bool has_function_expr_effect;
    XaFunctionExprEffectFact function_expr_effect;
    bool has_target_query;
    XaTargetQueryFact target_query;
    bool has_suspend_point;
    XaSuspendPointFact suspend_point;
    bool has_callable_target_set;
    XaCallableTargetSetFact callable_target_set;
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
            xr_free((void *) e->callable_target_set.targets);
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
            xr_free((void *) e->callable_target_set.targets);
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
    return e && !e->type && !e->scope && !e->symbol && !e->has_ct_value && !e->has_conversion &&
           !e->has_call_error_effect && !e->has_function_expr_effect && !e->has_target_query &&
           !e->has_suspend_point && !e->has_callable_target_set;
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
            xr_free((void *) to_free->callable_target_set.targets);
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

bool xa_node_table_set_call_error_effect(XaNodeTable *t, const struct AstNode *node,
                                         const XaCallErrorEffectFact *fact) {
    if (!t || !node || !fact)
        return false;
    XaNodeEntry *entry = find_or_create(t, node->node_id);
    if (!entry)
        return false;
    entry->has_call_error_effect = true;
    entry->call_error_effect = *fact;
    return true;
}

bool xa_node_table_get_call_error_effect(const XaNodeTable *t, const struct AstNode *node,
                                         XaCallErrorEffectFact *out_fact) {
    if (!t || !node)
        return false;
    const XaNodeEntry *entry = find_entry(t, node->node_id);
    if (!entry || !entry->has_call_error_effect)
        return false;
    if (out_fact)
        *out_fact = entry->call_error_effect;
    return true;
}

static int compare_node_call_error_effect_entry(const void *left, const void *right) {
    const XaNodeCallErrorEffectEntry *a = (const XaNodeCallErrorEffectEntry *) left;
    const XaNodeCallErrorEffectEntry *b = (const XaNodeCallErrorEffectEntry *) right;
    return a->node_id < b->node_id ? -1 : a->node_id > b->node_id ? 1 : 0;
}

bool xa_node_table_snapshot_call_error_effects(const XaNodeTable *t,
                                               XaNodeCallErrorEffectEntry **out_entries,
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
            if (entry->has_call_error_effect)
                count++;
        }
    }
    if (count == 0)
        return true;

    XaNodeCallErrorEffectEntry *entries =
        (XaNodeCallErrorEffectEntry *) xr_malloc(sizeof(*entries) * (size_t) count);
    if (!entries)
        return false;
    uint32_t index = 0;
    for (int i = 0; i < t->bucket_count; i++) {
        for (const XaNodeEntry *entry = t->buckets[i]; entry; entry = entry->next) {
            if (!entry->has_call_error_effect)
                continue;
            entries[index].node_id = entry->node_id;
            entries[index].fact = entry->call_error_effect;
            index++;
        }
    }
    qsort(entries, count, sizeof(*entries), compare_node_call_error_effect_entry);
    *out_entries = entries;
    *out_count = count;
    return true;
}

void xa_node_table_clear_call_error_effect(XaNodeTable *t, const struct AstNode *node) {
    if (!t || !node)
        return;
    XaNodeEntry *entry = (XaNodeEntry *) find_entry(t, node->node_id);
    if (!entry || !entry->has_call_error_effect)
        return;
    entry->has_call_error_effect = false;
    entry->call_error_effect = (XaCallErrorEffectFact) {0};
    if (entry_has_no_facts(entry))
        remove_entry_by_id(t, node->node_id);
}

static bool function_expr_effect_fact_valid(const XaFunctionExprEffectFact *fact) {
    if (!fact || fact->effect_id == XA_EFFECT_NONE ||
        (fact->throw_effect != XR_FN_EFFECT_NO_THROW &&
         fact->throw_effect != XR_FN_EFFECT_MAY_THROW) ||
        (fact->completeness != XA_EFFECT_COMPLETE && fact->completeness != XA_EFFECT_INCOMPLETE))
        return false;
    return fact->completeness != XA_EFFECT_COMPLETE || fact->unknown_reasons == XA_UNKNOWN_NONE;
}

bool xa_node_table_set_function_expr_effect(XaNodeTable *t, const struct AstNode *node,
                                            const XaFunctionExprEffectFact *fact) {
    if (!t || !node || node->type != AST_FUNCTION_EXPR || !function_expr_effect_fact_valid(fact))
        return false;
    XaNodeEntry *entry = find_or_create(t, node->node_id);
    if (!entry)
        return false;
    entry->has_function_expr_effect = true;
    entry->function_expr_effect = *fact;
    return true;
}

bool xa_node_table_get_function_expr_effect(const XaNodeTable *t, const struct AstNode *node,
                                            XaFunctionExprEffectFact *out_fact) {
    if (!t || !node || node->type != AST_FUNCTION_EXPR)
        return false;
    const XaNodeEntry *entry = find_entry(t, node->node_id);
    if (!entry || !entry->has_function_expr_effect)
        return false;
    if (out_fact)
        *out_fact = entry->function_expr_effect;
    return true;
}

static int compare_node_function_expr_effect_entry(const void *left, const void *right) {
    const XaNodeFunctionExprEffectEntry *a = (const XaNodeFunctionExprEffectEntry *) left;
    const XaNodeFunctionExprEffectEntry *b = (const XaNodeFunctionExprEffectEntry *) right;
    return a->node_id < b->node_id ? -1 : a->node_id > b->node_id ? 1 : 0;
}

bool xa_node_table_snapshot_function_expr_effects(const XaNodeTable *t,
                                                  XaNodeFunctionExprEffectEntry **out_entries,
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
            if (entry->has_function_expr_effect)
                count++;
        }
    }
    if (count == 0)
        return true;

    XaNodeFunctionExprEffectEntry *entries =
        (XaNodeFunctionExprEffectEntry *) xr_malloc(sizeof(*entries) * (size_t) count);
    if (!entries)
        return false;
    uint32_t index = 0;
    for (int i = 0; i < t->bucket_count; i++) {
        for (const XaNodeEntry *entry = t->buckets[i]; entry; entry = entry->next) {
            if (!entry->has_function_expr_effect)
                continue;
            entries[index].node_id = entry->node_id;
            entries[index].fact = entry->function_expr_effect;
            index++;
        }
    }
    qsort(entries, count, sizeof(*entries), compare_node_function_expr_effect_entry);
    *out_entries = entries;
    *out_count = count;
    return true;
}

void xa_node_table_clear_function_expr_effect(XaNodeTable *t, const struct AstNode *node) {
    if (!t || !node)
        return;
    XaNodeEntry *entry = (XaNodeEntry *) find_entry(t, node->node_id);
    if (!entry || !entry->has_function_expr_effect)
        return;
    entry->has_function_expr_effect = false;
    entry->function_expr_effect = (XaFunctionExprEffectFact) {0};
    if (entry_has_no_facts(entry))
        remove_entry_by_id(t, node->node_id);
}

static bool target_query_fact_valid(const XaTargetQueryFact *fact) {
    return fact && fact->namespace_id == XA_TARGET_NAMESPACE_TARGET &&
           fact->query_id >= XA_TARGET_QUERY_POINTER_BITS &&
           fact->query_id <= XA_TARGET_QUERY_ENDIANNESS &&
           fact->result_native_type == XR_NATIVE_U16 &&
           fact->complete == 1;
}

bool xa_node_table_set_target_query(XaNodeTable *t, const struct AstNode *node,
                                    const XaTargetQueryFact *fact) {
    if (!t || !node || node->type != AST_MEMBER_ACCESS || !target_query_fact_valid(fact))
        return false;
    XaNodeEntry *entry = find_or_create(t, node->node_id);
    if (!entry)
        return false;
    entry->has_target_query = true;
    entry->target_query = *fact;
    return true;
}

bool xa_node_table_get_target_query(const XaNodeTable *t, const struct AstNode *node,
                                    XaTargetQueryFact *out_fact) {
    if (!t || !node || node->type != AST_MEMBER_ACCESS)
        return false;
    const XaNodeEntry *entry = find_entry(t, node->node_id);
    if (!entry || !entry->has_target_query)
        return false;
    if (out_fact)
        *out_fact = entry->target_query;
    return true;
}

static int compare_node_target_query_entry(const void *left, const void *right) {
    const XaNodeTargetQueryEntry *a = (const XaNodeTargetQueryEntry *) left;
    const XaNodeTargetQueryEntry *b = (const XaNodeTargetQueryEntry *) right;
    return a->node_id < b->node_id ? -1 : a->node_id > b->node_id ? 1 : 0;
}

bool xa_node_table_snapshot_target_queries(const XaNodeTable *t,
                                           XaNodeTargetQueryEntry **out_entries,
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
            if (entry->has_target_query)
                count++;
        }
    }
    if (count == 0)
        return true;

    XaNodeTargetQueryEntry *entries =
        (XaNodeTargetQueryEntry *) xr_malloc(sizeof(*entries) * (size_t) count);
    if (!entries)
        return false;
    uint32_t index = 0;
    for (int i = 0; i < t->bucket_count; i++) {
        for (const XaNodeEntry *entry = t->buckets[i]; entry; entry = entry->next) {
            if (!entry->has_target_query)
                continue;
            entries[index].node_id = entry->node_id;
            entries[index].fact = entry->target_query;
            index++;
        }
    }
    qsort(entries, count, sizeof(*entries), compare_node_target_query_entry);
    *out_entries = entries;
    *out_count = count;
    return true;
}

void xa_node_table_clear_target_query(XaNodeTable *t, const struct AstNode *node) {
    if (!t || !node)
        return;
    XaNodeEntry *entry = (XaNodeEntry *) find_entry(t, node->node_id);
    if (!entry || !entry->has_target_query)
        return;
    entry->has_target_query = false;
    entry->target_query = (XaTargetQueryFact) {0};
    if (entry_has_no_facts(entry))
        remove_entry_by_id(t, node->node_id);
}

static bool suspend_point_fact_valid(const XaSuspendPointFact *fact) {
    return fact && fact->kind == XA_SUSPEND_POINT_COOPERATIVE_YIELD && fact->may_suspend == 1 &&
           fact->complete == 1;
}

bool xa_node_table_set_suspend_point(XaNodeTable *t, const struct AstNode *node,
                                     const XaSuspendPointFact *fact) {
    if (!t || !node || node->type != AST_CALL_EXPR || !suspend_point_fact_valid(fact))
        return false;
    XaNodeEntry *entry = find_or_create(t, node->node_id);
    if (!entry)
        return false;
    entry->has_suspend_point = true;
    entry->suspend_point = *fact;
    return true;
}

bool xa_node_table_get_suspend_point(const XaNodeTable *t, const struct AstNode *node,
                                     XaSuspendPointFact *out_fact) {
    if (!t || !node || node->type != AST_CALL_EXPR)
        return false;
    const XaNodeEntry *entry = find_entry(t, node->node_id);
    if (!entry || !entry->has_suspend_point)
        return false;
    if (out_fact)
        *out_fact = entry->suspend_point;
    return true;
}

static int compare_node_suspend_point_entry(const void *left, const void *right) {
    const XaNodeSuspendPointEntry *a = (const XaNodeSuspendPointEntry *) left;
    const XaNodeSuspendPointEntry *b = (const XaNodeSuspendPointEntry *) right;
    return a->node_id < b->node_id ? -1 : a->node_id > b->node_id ? 1 : 0;
}

bool xa_node_table_snapshot_suspend_points(const XaNodeTable *t,
                                           XaNodeSuspendPointEntry **out_entries,
                                           uint32_t *out_count) {
    if (!out_entries || !out_count)
        return false;
    *out_entries = NULL;
    *out_count = 0;
    if (!t)
        return false;
    uint32_t count = 0;
    for (int i = 0; i < t->bucket_count; i++)
        for (const XaNodeEntry *entry = t->buckets[i]; entry; entry = entry->next)
            count += entry->has_suspend_point ? 1u : 0u;
    if (count == 0)
        return true;
    XaNodeSuspendPointEntry *entries =
        (XaNodeSuspendPointEntry *) xr_malloc(sizeof(*entries) * (size_t) count);
    if (!entries)
        return false;
    uint32_t index = 0;
    for (int i = 0; i < t->bucket_count; i++) {
        for (const XaNodeEntry *entry = t->buckets[i]; entry; entry = entry->next) {
            if (!entry->has_suspend_point)
                continue;
            entries[index].node_id = entry->node_id;
            entries[index].fact = entry->suspend_point;
            index++;
        }
    }
    qsort(entries, count, sizeof(*entries), compare_node_suspend_point_entry);
    *out_entries = entries;
    *out_count = count;
    return true;
}

void xa_node_table_clear_suspend_point(XaNodeTable *t, const struct AstNode *node) {
    if (!t || !node)
        return;
    XaNodeEntry *entry = (XaNodeEntry *) find_entry(t, node->node_id);
    if (!entry || !entry->has_suspend_point)
        return;
    entry->has_suspend_point = false;
    entry->suspend_point = (XaSuspendPointFact) {0};
    if (entry_has_no_facts(entry))
        remove_entry_by_id(t, node->node_id);
}

static int callable_target_compare(const XaCallableTarget *a, const XaCallableTarget *b) {
    if (a->function_node_id != b->function_node_id)
        return a->function_node_id < b->function_node_id ? -1 : 1;
    if (a->symbol_id != b->symbol_id)
        return a->symbol_id < b->symbol_id ? -1 : 1;
    return 0;
}

static bool callable_target_set_valid(const XaCallableTargetSetFact *fact) {
    if (!fact)
        return false;
    if (!fact->complete)
        return fact->target_count == 0 && fact->structural_signature_key == 0 && !fact->targets;
    if (fact->target_count == 0 || fact->structural_signature_key == 0 || !fact->targets)
        return false;
    for (uint32_t i = 0; i < fact->target_count; i++) {
        const XaCallableTarget *target = &fact->targets[i];
        if ((target->function_node_id == 0 && target->symbol_id == 0) ||
            target->structural_signature_key != fact->structural_signature_key ||
            (i > 0 && callable_target_compare(&fact->targets[i - 1], target) >= 0))
            return false;
    }
    return true;
}

bool xa_node_table_set_callable_target_set(XaNodeTable *t, const struct AstNode *node,
                                           const XaCallableTargetSetFact *fact) {
    if (!t || !node || !callable_target_set_valid(fact))
        return false;
    XaNodeEntry *entry = find_or_create(t, node->node_id);
    if (!entry)
        return false;
    XaCallableTarget *targets = NULL;
    if (fact->target_count > 0) {
        targets = (XaCallableTarget *) xr_malloc(sizeof(*targets) * (size_t) fact->target_count);
        if (!targets)
            return false;
        memcpy(targets, fact->targets, sizeof(*targets) * (size_t) fact->target_count);
    }
    xr_free((void *) entry->callable_target_set.targets);
    entry->has_callable_target_set = true;
    entry->callable_target_set = *fact;
    entry->callable_target_set.targets = targets;
    return true;
}

bool xa_node_table_get_callable_target_set(const XaNodeTable *t, const struct AstNode *node,
                                           XaCallableTargetSetFact *out_fact) {
    if (!t || !node)
        return false;
    const XaNodeEntry *entry = find_entry(t, node->node_id);
    if (!entry || !entry->has_callable_target_set)
        return false;
    if (out_fact)
        *out_fact = entry->callable_target_set;
    return true;
}

static int compare_node_callable_target_set_entry(const void *left, const void *right) {
    const XaNodeCallableTargetSetEntry *a = (const XaNodeCallableTargetSetEntry *) left;
    const XaNodeCallableTargetSetEntry *b = (const XaNodeCallableTargetSetEntry *) right;
    return a->node_id < b->node_id ? -1 : a->node_id > b->node_id ? 1 : 0;
}

bool xa_node_table_snapshot_callable_target_sets(const XaNodeTable *t,
                                                 XaNodeCallableTargetSetEntry **out_entries,
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
            if (entry->has_callable_target_set)
                count++;
        }
    }
    if (count == 0)
        return true;
    XaNodeCallableTargetSetEntry *entries =
        (XaNodeCallableTargetSetEntry *) xr_calloc(count, sizeof(*entries));
    if (!entries)
        return false;
    uint32_t index = 0;
    for (int i = 0; i < t->bucket_count; i++) {
        for (const XaNodeEntry *entry = t->buckets[i]; entry; entry = entry->next) {
            if (!entry->has_callable_target_set)
                continue;
            entries[index].node_id = entry->node_id;
            entries[index].fact = entry->callable_target_set;
            if (entry->callable_target_set.target_count > 0) {
                XaCallableTarget *targets = (XaCallableTarget *) xr_malloc(
                    sizeof(*targets) * (size_t) entry->callable_target_set.target_count);
                if (!targets) {
                    xa_node_callable_target_set_entries_free(entries, count);
                    return false;
                }
                memcpy(targets, entry->callable_target_set.targets,
                       sizeof(*targets) * (size_t) entry->callable_target_set.target_count);
                entries[index].fact.targets = targets;
            }
            index++;
        }
    }
    qsort(entries, count, sizeof(*entries), compare_node_callable_target_set_entry);
    *out_entries = entries;
    *out_count = count;
    return true;
}

void xa_node_callable_target_set_entries_free(XaNodeCallableTargetSetEntry *entries,
                                              uint32_t count) {
    if (!entries)
        return;
    for (uint32_t i = 0; i < count; i++)
        xr_free((void *) entries[i].fact.targets);
    xr_free(entries);
}

void xa_node_table_clear_callable_target_set(XaNodeTable *t, const struct AstNode *node) {
    if (!t || !node)
        return;
    XaNodeEntry *entry = (XaNodeEntry *) find_entry(t, node->node_id);
    if (!entry || !entry->has_callable_target_set)
        return;
    entry->has_callable_target_set = false;
    xr_free((void *) entry->callable_target_set.targets);
    entry->callable_target_set = (XaCallableTargetSetFact) {0};
    if (entry_has_no_facts(entry))
        remove_entry_by_id(t, node->node_id);
}
