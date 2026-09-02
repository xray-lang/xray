/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "xa_enum_record_plan.h"
#include "../../base/xchecks.h"
#include "../../base/xhash.h"
#include "../../base/xmalloc.h"
#include "../parser/xast_nodes.h"
#include <string.h>

typedef struct XaEnumRecordPlanEntry {
    const struct AstNode *node;
    uint64_t analysis_id;
    XaEnumRecordPlan plan;
    uint16_t *source_to_slot;
    struct XaEnumRecordPlanEntry *next;
} XaEnumRecordPlanEntry;

struct XaEnumRecordPlanTable {
    XaEnumRecordPlanEntry **buckets;
    int bucket_count;
    int size;
    uint64_t analysis_id;
};

#define XA_ENUM_RECORD_PLAN_INITIAL_BUCKETS 64
#define XA_ENUM_RECORD_PLAN_LOAD_NUM 3
#define XA_ENUM_RECORD_PLAN_LOAD_DEN 4

static uint32_t hash_node(const struct AstNode *node) {
    uintptr_t value = (uintptr_t) node;
    return xr_hash_int((int64_t) (value ^ (value >> 32)));
}

static int bucket_of(const XaEnumRecordPlanTable *table, const struct AstNode *node) {
    return (int) (hash_node(node) % (uint32_t) table->bucket_count);
}

XaEnumRecordPlanTable *xa_enum_record_plan_table_new(void) {
    XaEnumRecordPlanTable *table = xr_malloc(sizeof(*table));
    if (!table)
        return NULL;
    table->bucket_count = XA_ENUM_RECORD_PLAN_INITIAL_BUCKETS;
    table->buckets = xr_calloc((size_t) table->bucket_count, sizeof(*table->buckets));
    if (!table->buckets) {
        xr_free(table);
        return NULL;
    }
    table->size = 0;
    table->analysis_id = 1;
    return table;
}

void xa_enum_record_plan_table_clear(XaEnumRecordPlanTable *table) {
    if (!table)
        return;
    for (int i = 0; i < table->bucket_count; i++) {
        XaEnumRecordPlanEntry *entry = table->buckets[i];
        while (entry) {
            XaEnumRecordPlanEntry *next = entry->next;
            xr_free(entry->source_to_slot);
            xr_free(entry);
            entry = next;
        }
        table->buckets[i] = NULL;
    }
    table->size = 0;
}

void xa_enum_record_plan_table_begin_analysis(XaEnumRecordPlanTable *table) {
    if (!table)
        return;
    table->analysis_id++;
    if (table->analysis_id == 0)
        table->analysis_id = 1;
}

void xa_enum_record_plan_table_free(XaEnumRecordPlanTable *table) {
    if (!table)
        return;
    xa_enum_record_plan_table_clear(table);
    xr_free(table->buckets);
    xr_free(table);
}

int xa_enum_record_plan_table_size(const XaEnumRecordPlanTable *table) {
    return table ? table->size : 0;
}

static bool plans_equal(const XaEnumRecordPlan *left, const XaEnumRecordPlan *right) {
    if (!left || !right || left->kind != right->kind ||
        left->enum_symbol_id != right->enum_symbol_id ||
        left->enum_layout_id != right->enum_layout_id ||
        left->variant_ordinal != right->variant_ordinal ||
        left->source_field_count != right->source_field_count ||
        left->declaration_field_count != right->declaration_field_count ||
        left->complete != right->complete)
        return false;
    if (left->source_field_count == 0)
        return true;
    return left->source_to_slot && right->source_to_slot &&
           memcmp(left->source_to_slot, right->source_to_slot,
                  (size_t) left->source_field_count * sizeof(*left->source_to_slot)) == 0;
}

static bool plan_mapping_is_valid(const XaEnumRecordPlan *plan) {
    if (!plan || plan->source_field_count > plan->declaration_field_count)
        return false;
    if (plan->declaration_field_count == 0)
        return plan->kind == XA_ENUM_VARIANT_PATTERN && plan->source_field_count == 0 &&
               plan->source_to_slot == NULL;
    if (plan->kind == XA_ENUM_RECORD_CONSTRUCT &&
        plan->source_field_count != plan->declaration_field_count)
        return false;
    if (plan->kind != XA_ENUM_RECORD_CONSTRUCT && plan->kind != XA_ENUM_VARIANT_PATTERN)
        return false;

    for (uint16_t i = 0; i < plan->source_field_count; i++) {
        uint16_t slot = plan->source_to_slot[i];
        if (slot >= plan->declaration_field_count)
            return false;
        for (uint16_t j = 0; j < i; j++) {
            if (plan->source_to_slot[j] == slot)
                return false;
        }
    }
    return true;
}

static void grow(XaEnumRecordPlanTable *table) {
    int new_count = table->bucket_count * 2;
    XaEnumRecordPlanEntry **buckets = xr_calloc((size_t) new_count, sizeof(*buckets));
    if (!buckets)
        return;
    for (int i = 0; i < table->bucket_count; i++) {
        XaEnumRecordPlanEntry *entry = table->buckets[i];
        while (entry) {
            XaEnumRecordPlanEntry *next = entry->next;
            int bucket = (int) (hash_node(entry->node) % (uint32_t) new_count);
            entry->next = buckets[bucket];
            buckets[bucket] = entry;
            entry = next;
        }
    }
    xr_free(table->buckets);
    table->buckets = buckets;
    table->bucket_count = new_count;
}

bool xa_enum_record_plan_table_set(XaEnumRecordPlanTable *table, struct AstNode *node,
                                   const XaEnumRecordPlan *plan) {
    XR_DCHECK(table != NULL, "xa_enum_record_plan_table_set: NULL table");
    XR_DCHECK(node != NULL, "xa_enum_record_plan_table_set: NULL node");
    XR_DCHECK(plan != NULL, "xa_enum_record_plan_table_set: NULL plan");
    if (!table || !node || !plan || !plan->complete || plan->enum_symbol_id == 0 ||
        (plan->source_field_count > 0 && !plan->source_to_slot) ||
        !plan_mapping_is_valid(plan))
        return false;

    int bucket = bucket_of(table, node);
    for (XaEnumRecordPlanEntry *entry = table->buckets[bucket]; entry; entry = entry->next) {
        if (entry->node != node)
            continue;
        if (entry->analysis_id == table->analysis_id)
            return plans_equal(&entry->plan, plan);
        uint16_t *mapping = NULL;
        if (plan->source_field_count > 0) {
            mapping = xr_malloc((size_t) plan->source_field_count * sizeof(*mapping));
            if (!mapping)
                return false;
            memcpy(mapping, plan->source_to_slot,
                   (size_t) plan->source_field_count * sizeof(*mapping));
        }
        xr_free(entry->source_to_slot);
        entry->source_to_slot = mapping;
        entry->plan = *plan;
        entry->plan.source_to_slot = mapping;
        entry->analysis_id = table->analysis_id;
        return true;
    }

    XaEnumRecordPlanEntry *entry = xr_calloc(1, sizeof(*entry));
    if (!entry)
        return false;
    if (plan->source_field_count > 0) {
        entry->source_to_slot =
            xr_malloc((size_t) plan->source_field_count * sizeof(*entry->source_to_slot));
        if (!entry->source_to_slot) {
            xr_free(entry);
            return false;
        }
        memcpy(entry->source_to_slot, plan->source_to_slot,
               (size_t) plan->source_field_count * sizeof(*entry->source_to_slot));
    }
    entry->node = node;
    entry->analysis_id = table->analysis_id;
    entry->plan = *plan;
    entry->plan.source_to_slot = entry->source_to_slot;
    entry->next = table->buckets[bucket];
    table->buckets[bucket] = entry;
    table->size++;

    if ((int64_t) table->size * XA_ENUM_RECORD_PLAN_LOAD_DEN >
        (int64_t) table->bucket_count * XA_ENUM_RECORD_PLAN_LOAD_NUM)
        grow(table);
    return true;
}

const XaEnumRecordPlan *
xa_enum_record_plan_table_get(const XaEnumRecordPlanTable *table, const struct AstNode *node) {
    if (!table || !node)
        return NULL;
    int bucket = bucket_of(table, node);
    for (const XaEnumRecordPlanEntry *entry = table->buckets[bucket]; entry;
         entry = entry->next) {
        if (entry->node == node)
            return &entry->plan;
    }
    return NULL;
}
