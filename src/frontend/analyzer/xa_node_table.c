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
#include "../../frontend/parser/xtype_ref.h"
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
    bool has_body_effect;
    XaBodyEffectFact body_effect;
    bool has_target_query;
    XaTargetQueryFact target_query;
    bool has_suspend_point;
    XaSuspendPointFact suspend_point;
    bool has_callable_target_set;
    XaCallableTargetSetFact callable_target_set;
    bool has_generic_specialization;
    XaGenericSpecializationFact generic_specialization;
    struct XaNodeEntry *next;
} XaNodeEntry;

typedef struct XaTypeRefEntry {
    const struct XrTypeRef *type_ref;
    struct XrType *type;
    uint64_t syntax_key;
    struct XaTypeRefEntry *next;
} XaTypeRefEntry;

struct XaNodeTable {
    XaNodeEntry **buckets;
    int bucket_count;
    int size;
    XaTypeRefEntry **type_ref_buckets;
    int type_ref_bucket_count;
    int type_ref_size;
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
    t->type_ref_bucket_count = XA_NODE_TABLE_INITIAL_BUCKETS;
    t->type_ref_buckets =
        (XaTypeRefEntry **) xr_calloc(t->type_ref_bucket_count, sizeof(XaTypeRefEntry *));
    if (!t->type_ref_buckets) {
        xr_free(t->buckets);
        xr_free(t);
        return NULL;
    }
    t->type_ref_size = 0;
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
            xr_free(e->generic_specialization.receiver_type_args);
            xr_free(e->generic_specialization.declaration_type_args);
            xr_free(e);
            e = next;
        }
    }
    for (int i = 0; i < t->type_ref_bucket_count; i++) {
        XaTypeRefEntry *e = t->type_ref_buckets[i];
        while (e) {
            XaTypeRefEntry *next = e->next;
            xr_free(e);
            e = next;
        }
    }
    xr_free(t->type_ref_buckets);
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
            xr_free(e->generic_specialization.receiver_type_args);
            xr_free(e->generic_specialization.declaration_type_args);
            xr_free(e);
            e = next;
        }
        t->buckets[i] = NULL;
    }
    t->size = 0;
    xa_node_table_clear_type_ref_types(t);
}

void xa_node_table_clear_type_ref_types(XaNodeTable *t) {
    if (!t)
        return;
    for (int i = 0; i < t->type_ref_bucket_count; i++) {
        XaTypeRefEntry *e = t->type_ref_buckets[i];
        while (e) {
            XaTypeRefEntry *next = e->next;
            xr_free(e);
            e = next;
        }
        t->type_ref_buckets[i] = NULL;
    }
    t->type_ref_size = 0;
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
           !e->has_call_error_effect && !e->has_body_effect && !e->has_target_query &&
           !e->has_suspend_point && !e->has_callable_target_set && !e->has_generic_specialization;
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
            xr_free(to_free->generic_specialization.receiver_type_args);
            xr_free(to_free->generic_specialization.declaration_type_args);
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

static inline uint32_t hash_type_ref(const struct XrTypeRef *type_ref) {
    uintptr_t value = (uintptr_t) type_ref;
    return xr_hash_int((int64_t) (value ^ (value >> 32)));
}

static uint64_t type_ref_syntax_fold(uint64_t hash, uint64_t value) {
    for (uint32_t shift = 0u; shift < 64u; shift += 8u) {
        hash ^= (uint8_t) (value >> shift);
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t type_ref_syntax_string(uint64_t hash, const char *value) {
    size_t length = value ? strlen(value) : 0u;
    hash = type_ref_syntax_fold(hash, (uint64_t) length);
    for (size_t i = 0; i < length; ++i) {
        hash ^= (unsigned char) value[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool type_ref_syntax_key_inner(const XrTypeRef *type_ref, int depth, uint64_t *hash) {
    if (!type_ref || !hash || depth > 64)
        return false;
    *hash = type_ref_syntax_fold(*hash, (uint64_t) type_ref->kind);
    *hash = type_ref_syntax_fold(*hash, (uint64_t) type_ref->nchildren);
    *hash = type_ref_syntax_fold(*hash, (uint64_t) type_ref->scalar_rep);
    *hash = type_ref_syntax_fold(*hash, (uint64_t) type_ref->requires_nothrow);
    *hash = type_ref_syntax_fold(*hash, (uint64_t) (uint32_t) type_ref->fixed_length);
    *hash = type_ref_syntax_fold(
        *hash, type_ref->fixed_length_expr ? (uint64_t) type_ref->fixed_length_expr->node_id : 0u);
    *hash = type_ref_syntax_string(*hash, type_ref->name);
    *hash = type_ref_syntax_fold(*hash, (uint64_t) type_ref->borrow_origin_syntax);
    *hash = type_ref_syntax_fold(*hash, (uint64_t) (uint32_t) type_ref->borrow_origin_count);
    for (int i = 0; i < type_ref->borrow_origin_count; ++i) {
        if (!type_ref->borrow_origins)
            return false;
        *hash = type_ref_syntax_fold(*hash, (uint64_t) type_ref->borrow_origins[i].kind);
        *hash = type_ref_syntax_string(*hash, type_ref->borrow_origins[i].name);
    }
    for (uint8_t i = 0; i < type_ref->nchildren; ++i) {
        if (!type_ref->children ||
            !type_ref_syntax_key_inner(type_ref->children[i], depth + 1, hash))
            return false;
        if (type_ref->kind == XR_TREF_OBJECT) {
            if (!type_ref->field_names)
                return false;
            *hash = type_ref_syntax_string(*hash, type_ref->field_names[i]);
            *hash = type_ref_syntax_fold(
                *hash, (uint64_t) (type_ref->field_readonly && type_ref->field_readonly[i]));
        } else if (type_ref->kind == XR_TREF_FUNCTION) {
            XrParamMode mode = XR_PARAM_READ;
            if (i + 1u < type_ref->nchildren && type_ref->function_param_modes)
                mode = type_ref->function_param_modes[i];
            *hash = type_ref_syntax_fold(*hash, (uint64_t) mode);
            if (i + 1u < type_ref->nchildren)
                *hash = type_ref_syntax_string(*hash, type_ref->function_param_names
                                                          ? type_ref->function_param_names[i]
                                                          : NULL);
        }
    }
    return true;
}

static uint64_t type_ref_syntax_key(const XrTypeRef *type_ref) {
    uint64_t hash = UINT64_C(1469598103934665603);
    return type_ref_syntax_key_inner(type_ref, 0, &hash) ? hash : 0u;
}

static inline int type_ref_bucket_of(const XaNodeTable *t, const struct XrTypeRef *type_ref) {
    return (int) (hash_type_ref(type_ref) % (uint32_t) t->type_ref_bucket_count);
}

static void type_ref_table_grow(XaNodeTable *t) {
    int new_count = t->type_ref_bucket_count * XA_NODE_TABLE_GROWTH;
    XaTypeRefEntry **new_buckets =
        (XaTypeRefEntry **) xr_calloc(new_count, sizeof(XaTypeRefEntry *));
    if (!new_buckets)
        return;
    for (int i = 0; i < t->type_ref_bucket_count; i++) {
        XaTypeRefEntry *e = t->type_ref_buckets[i];
        while (e) {
            XaTypeRefEntry *next = e->next;
            int bucket = (int) (hash_type_ref(e->type_ref) % (uint32_t) new_count);
            e->next = new_buckets[bucket];
            new_buckets[bucket] = e;
            e = next;
        }
    }
    xr_free(t->type_ref_buckets);
    t->type_ref_buckets = new_buckets;
    t->type_ref_bucket_count = new_count;
}

bool xa_node_table_set_type_ref_type(XaNodeTable *t, const struct XrTypeRef *type_ref,
                                     struct XrType *type) {
    if (!t || !type_ref || !type)
        return false;
    uint64_t syntax_key = type_ref_syntax_key(type_ref);
    if (syntax_key == 0u)
        return false;
    int bucket = type_ref_bucket_of(t, type_ref);
    for (XaTypeRefEntry *e = t->type_ref_buckets[bucket]; e; e = e->next) {
        if (e->type_ref == type_ref) {
            e->type = type;
            e->syntax_key = syntax_key;
            return true;
        }
    }
    XaTypeRefEntry *entry = (XaTypeRefEntry *) xr_calloc(1, sizeof(*entry));
    if (!entry)
        return false;
    entry->type_ref = type_ref;
    entry->type = type;
    entry->syntax_key = syntax_key;
    entry->next = t->type_ref_buckets[bucket];
    t->type_ref_buckets[bucket] = entry;
    t->type_ref_size++;
    if ((int64_t) t->type_ref_size * XA_NODE_TABLE_LOAD_DEN >
        (int64_t) t->type_ref_bucket_count * XA_NODE_TABLE_LOAD_NUM)
        type_ref_table_grow(t);
    return true;
}

struct XrType *xa_node_table_get_type_ref_type(const XaNodeTable *t,
                                               const struct XrTypeRef *type_ref) {
    if (!t || !type_ref)
        return NULL;
    uint64_t syntax_key = type_ref_syntax_key(type_ref);
    if (syntax_key == 0u)
        return NULL;
    int bucket = type_ref_bucket_of(t, type_ref);
    for (XaTypeRefEntry *e = t->type_ref_buckets[bucket]; e; e = e->next) {
        if (e->type_ref == type_ref && e->syntax_key == syntax_key)
            return e->type;
    }
    return NULL;
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

static bool body_effect_fact_valid(const XaBodyEffectFact *fact) {
    if (!fact || fact->effect_id == XA_EFFECT_NONE ||
        (fact->throw_effect != XR_FN_EFFECT_NO_THROW &&
         fact->throw_effect != XR_FN_EFFECT_MAY_THROW) ||
        (fact->completeness != XA_EFFECT_COMPLETE && fact->completeness != XA_EFFECT_INCOMPLETE))
        return false;
    return fact->completeness != XA_EFFECT_COMPLETE || fact->unknown_reasons == XA_UNKNOWN_NONE;
}

bool xa_node_table_set_body_effect(XaNodeTable *t, const struct AstNode *node,
                                   const XaBodyEffectFact *fact) {
    if (!t || !node || (node->type != AST_FUNCTION_EXPR && node->type != AST_PROGRAM) ||
        !body_effect_fact_valid(fact))
        return false;
    XaNodeEntry *entry = find_or_create(t, node->node_id);
    if (!entry)
        return false;
    entry->has_body_effect = true;
    entry->body_effect = *fact;
    return true;
}

bool xa_node_table_get_body_effect(const XaNodeTable *t, const struct AstNode *node,
                                   XaBodyEffectFact *out_fact) {
    if (!t || !node || (node->type != AST_FUNCTION_EXPR && node->type != AST_PROGRAM))
        return false;
    const XaNodeEntry *entry = find_entry(t, node->node_id);
    if (!entry || !entry->has_body_effect)
        return false;
    if (out_fact)
        *out_fact = entry->body_effect;
    return true;
}

static int compare_node_body_effect_entry(const void *left, const void *right) {
    const XaNodeBodyEffectEntry *a = (const XaNodeBodyEffectEntry *) left;
    const XaNodeBodyEffectEntry *b = (const XaNodeBodyEffectEntry *) right;
    return a->node_id < b->node_id ? -1 : a->node_id > b->node_id ? 1 : 0;
}

bool xa_node_table_snapshot_body_effects(const XaNodeTable *t, XaNodeBodyEffectEntry **out_entries,
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
            if (entry->has_body_effect)
                count++;
        }
    }
    if (count == 0)
        return true;

    XaNodeBodyEffectEntry *entries =
        (XaNodeBodyEffectEntry *) xr_malloc(sizeof(*entries) * (size_t) count);
    if (!entries)
        return false;
    uint32_t index = 0;
    for (int i = 0; i < t->bucket_count; i++) {
        for (const XaNodeEntry *entry = t->buckets[i]; entry; entry = entry->next) {
            if (!entry->has_body_effect)
                continue;
            entries[index].node_id = entry->node_id;
            entries[index].fact = entry->body_effect;
            index++;
        }
    }
    qsort(entries, count, sizeof(*entries), compare_node_body_effect_entry);
    *out_entries = entries;
    *out_count = count;
    return true;
}

void xa_node_table_clear_body_effect(XaNodeTable *t, const struct AstNode *node) {
    if (!t || !node)
        return;
    XaNodeEntry *entry = (XaNodeEntry *) find_entry(t, node->node_id);
    if (!entry || !entry->has_body_effect)
        return;
    entry->has_body_effect = false;
    entry->body_effect = (XaBodyEffectFact) {0};
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

static bool generic_specialization_tuple_valid(struct XrTypeRef **type_args, uint32_t count) {
    if ((count == 0u) != (type_args == NULL))
        return false;
    for (uint32_t i = 0u; i < count; ++i) {
        if (!type_args[i])
            return false;
    }
    return true;
}

static uint32_t generic_specialization_owner_arity(const struct AstNode *owner) {
    if (!owner)
        return 0u;
    if (owner->type == AST_CLASS_DECL)
        return owner->as.class_decl.type_param_count < 0
                   ? UINT32_MAX
                   : (uint32_t) owner->as.class_decl.type_param_count;
    if (owner->type == AST_STRUCT_DECL)
        return owner->as.struct_decl.type_param_count < 0
                   ? UINT32_MAX
                   : (uint32_t) owner->as.struct_decl.type_param_count;
    return UINT32_MAX;
}

static bool generic_specialization_owner_contains_method(const struct AstNode *owner,
                                                         const struct AstNode *method) {
    if (!owner || !method || method->type != AST_METHOD_DECL ||
        (owner->type != AST_CLASS_DECL && owner->type != AST_STRUCT_DECL))
        return false;
    const ClassDeclNode *decl =
        owner->type == AST_CLASS_DECL ? &owner->as.class_decl : &owner->as.struct_decl;
    for (int i = 0; decl->methods && i < decl->method_count; ++i) {
        if (decl->methods[i] == method)
            return true;
    }
    return false;
}

static uint32_t generic_specialization_decl_arity(const struct AstNode *decl) {
    if (!decl)
        return UINT32_MAX;
    switch (decl->type) {
        case AST_FUNCTION_DECL:
            return decl->as.function_decl.type_param_count < 0
                       ? UINT32_MAX
                       : (uint32_t) decl->as.function_decl.type_param_count;
        case AST_METHOD_DECL:
            return decl->as.method_decl.type_param_count < 0
                       ? UINT32_MAX
                       : (uint32_t) decl->as.method_decl.type_param_count;
        case AST_CLASS_DECL:
            return decl->as.class_decl.type_param_count < 0
                       ? UINT32_MAX
                       : (uint32_t) decl->as.class_decl.type_param_count;
        case AST_STRUCT_DECL:
            return decl->as.struct_decl.type_param_count < 0
                       ? UINT32_MAX
                       : (uint32_t) decl->as.struct_decl.type_param_count;
        default:
            return UINT32_MAX;
    }
}

static bool generic_specialization_decl_has_effect_dimension(const struct AstNode *decl) {
    XrParamNode **params = NULL;
    int count = 0;
    if (decl && decl->type == AST_FUNCTION_DECL) {
        params = decl->as.function_decl.params;
        count = decl->as.function_decl.param_count;
    } else if (decl && decl->type == AST_METHOD_DECL) {
        params = decl->as.method_decl.params;
        count = decl->as.method_decl.param_count;
    }
    for (int i = 0; params && i < count; ++i) {
        const XrParamNode *param = params[i];
        if (param && param->type && param->type->kind == XR_TREF_FUNCTION &&
            !param->type->requires_nothrow)
            return true;
    }
    return false;
}

bool xa_generic_specialization_fact_valid(const XaGenericSpecializationFact *fact) {
    if (!fact || !fact->generic_decl ||
        (int) fact->effect < (int) XA_GENERIC_SPECIALIZATION_EFFECT_NONE ||
        fact->effect > XA_GENERIC_SPECIALIZATION_EFFECT_NO_THROW ||
        !generic_specialization_tuple_valid(fact->receiver_type_args,
                                            fact->receiver_type_arg_count) ||
        !generic_specialization_tuple_valid(fact->declaration_type_args,
                                            fact->declaration_type_arg_count))
        return false;
    bool has_effect_dimension =
        generic_specialization_decl_has_effect_dimension(fact->generic_decl);
    if ((fact->effect == XA_GENERIC_SPECIALIZATION_EFFECT_NONE) != !has_effect_dimension)
        return false;
    uint32_t decl_arity = generic_specialization_decl_arity(fact->generic_decl);
    if (decl_arity == UINT32_MAX || decl_arity != fact->declaration_type_arg_count)
        return false;
    if (fact->generic_decl->type == AST_METHOD_DECL) {
        if (!generic_specialization_owner_contains_method(fact->owner_decl, fact->generic_decl))
            return false;
        uint32_t owner_arity = generic_specialization_owner_arity(fact->owner_decl);
        return owner_arity != UINT32_MAX && owner_arity == fact->receiver_type_arg_count &&
               (owner_arity > 0u || decl_arity > 0u);
    }
    return !fact->owner_decl && fact->receiver_type_arg_count == 0u && decl_arity > 0u &&
           (fact->generic_decl->type == AST_FUNCTION_DECL ||
            fact->effect == XA_GENERIC_SPECIALIZATION_EFFECT_NONE);
}

static struct XrTypeRef **generic_specialization_tuple_copy(struct XrTypeRef **source,
                                                            uint32_t count) {
    if (count == 0u)
        return NULL;
    struct XrTypeRef **copy = (struct XrTypeRef **) xr_malloc(sizeof(*copy) * (size_t) count);
    if (copy)
        memcpy(copy, source, sizeof(*copy) * (size_t) count);
    return copy;
}

bool xa_node_table_set_generic_specialization(XaNodeTable *t, const struct AstNode *node,
                                              const XaGenericSpecializationFact *fact) {
    if (!t || !node || !xa_generic_specialization_fact_valid(fact))
        return false;
    struct XrTypeRef **receiver_type_args =
        generic_specialization_tuple_copy(fact->receiver_type_args, fact->receiver_type_arg_count);
    if (fact->receiver_type_arg_count > 0u && !receiver_type_args)
        return false;
    struct XrTypeRef **declaration_type_args = generic_specialization_tuple_copy(
        fact->declaration_type_args, fact->declaration_type_arg_count);
    if (fact->declaration_type_arg_count > 0u && !declaration_type_args) {
        xr_free(receiver_type_args);
        return false;
    }
    XaNodeEntry *entry = find_or_create(t, node->node_id);
    if (!entry) {
        xr_free(receiver_type_args);
        xr_free(declaration_type_args);
        return false;
    }
    xr_free(entry->generic_specialization.receiver_type_args);
    xr_free(entry->generic_specialization.declaration_type_args);
    entry->has_generic_specialization = true;
    entry->generic_specialization = *fact;
    entry->generic_specialization.receiver_type_args = receiver_type_args;
    entry->generic_specialization.declaration_type_args = declaration_type_args;
    return true;
}

bool xa_node_table_get_generic_specialization(const XaNodeTable *t, const struct AstNode *node,
                                              XaGenericSpecializationFact *out_fact) {
    if (!t || !node)
        return false;
    const XaNodeEntry *entry = find_entry(t, node->node_id);
    if (!entry || !entry->has_generic_specialization)
        return false;
    if (out_fact)
        *out_fact = entry->generic_specialization;
    return true;
}

void xa_node_table_clear_generic_specializations(XaNodeTable *t) {
    if (!t)
        return;
    for (int bucket = 0; bucket < t->bucket_count; ++bucket) {
        XaNodeEntry **link = &t->buckets[bucket];
        while (*link) {
            XaNodeEntry *entry = *link;
            if (entry->has_generic_specialization) {
                xr_free(entry->generic_specialization.receiver_type_args);
                xr_free(entry->generic_specialization.declaration_type_args);
                entry->generic_specialization = (XaGenericSpecializationFact) {0};
                entry->has_generic_specialization = false;
            }
            if (entry_has_no_facts(entry)) {
                *link = entry->next;
                xr_free(entry);
                t->size--;
            } else {
                link = &entry->next;
            }
        }
    }
}
