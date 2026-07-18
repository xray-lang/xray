/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_alloc_effect.c - Canonical analyzer-owned allocation effects
 */

#include "xa_alloc_effect.h"
#include "../../base/xmalloc.h"
#include <stdbool.h>
#include <string.h>

struct XaAllocationDatabase {
    XaAllocationSummary *summaries;
    uint32_t summary_count;
    uint32_t summary_capacity;
};

static uint64_t alloc_hash_bytes(uint64_t hash, const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *) data;
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t alloc_hash_string(uint64_t hash, const char *text) {
    static const uint8_t separator = 0xff;
    if (text)
        hash = alloc_hash_bytes(hash, text, strlen(text));
    return alloc_hash_bytes(hash, &separator, sizeof(separator));
}

uint64_t xa_allocation_summary_fingerprint(const XaAllocationSummary *summary) {
    if (!summary)
        return 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = alloc_hash_bytes(hash, &summary->state, sizeof(summary->state));
    hash = alloc_hash_bytes(hash, &summary->reason_bits, sizeof(summary->reason_bits));
    /* Node and symbol ids are analyzer-session-local.  Evidence/cache users
     * need a fingerprint that is reproducible across processes, so only
     * source-stable coordinates and semantic witness text participate. */
    hash = alloc_hash_bytes(hash, &summary->line, sizeof(summary->line));
    hash = alloc_hash_bytes(hash, &summary->column, sizeof(summary->column));
    hash = alloc_hash_string(hash, summary->cause_kind);
    hash = alloc_hash_string(hash, summary->cause_detail);
    hash = alloc_hash_string(hash, summary->callee_name);
    return hash;
}

static void allocation_summary_release(XaAllocationSummary *summary) {
    if (!summary)
        return;
    xr_free((char *) summary->cause_kind);
    xr_free((char *) summary->cause_detail);
    xr_free((char *) summary->callee_name);
    memset(summary, 0, sizeof(*summary));
}

XaAllocationDatabase *xa_allocation_db_new(void) {
    return (XaAllocationDatabase *) xr_calloc(1, sizeof(XaAllocationDatabase));
}

void xa_allocation_db_clear(XaAllocationDatabase *db) {
    if (!db)
        return;
    for (uint32_t i = 0; i < db->summary_count; i++)
        allocation_summary_release(&db->summaries[i]);
    db->summary_count = 0;
}

void xa_allocation_db_free(XaAllocationDatabase *db) {
    if (!db)
        return;
    xa_allocation_db_clear(db);
    xr_free(db->summaries);
    xr_free(db);
}

static bool allocation_summary_equal(const XaAllocationSummary *a, const XaAllocationSummary *b) {
    if (!a || !b || a->stable_fingerprint != b->stable_fingerprint || a->state != b->state ||
        a->reason_bits != b->reason_bits || a->first_site_node_id != b->first_site_node_id ||
        a->first_callee_symbol_id != b->first_callee_symbol_id || a->line != b->line ||
        a->column != b->column || a->callee_effect_id != b->callee_effect_id)
        return false;
#define SAME_TEXT(lhs, rhs)                                                                        \
    (((lhs) == NULL && (rhs) == NULL) || ((lhs) && (rhs) && strcmp((lhs), (rhs)) == 0))
    return SAME_TEXT(a->cause_kind, b->cause_kind) && SAME_TEXT(a->cause_detail, b->cause_detail) &&
           SAME_TEXT(a->callee_name, b->callee_name);
#undef SAME_TEXT
}

static bool allocation_db_grow(XaAllocationDatabase *db) {
    uint32_t next_capacity = db->summary_capacity ? db->summary_capacity * 2u : 16u;
    XaAllocationSummary *next = (XaAllocationSummary *) xr_realloc(
        db->summaries, (size_t) next_capacity * sizeof(XaAllocationSummary));
    if (!next)
        return false;
    memset(&next[db->summary_capacity], 0,
           (size_t) (next_capacity - db->summary_capacity) * sizeof(XaAllocationSummary));
    db->summaries = next;
    db->summary_capacity = next_capacity;
    return true;
}

XaAllocEffectId xa_allocation_db_intern(XaAllocationDatabase *db,
                                        const XaAllocationSummary *summary) {
    if (!db || !summary)
        return XA_ALLOC_EFFECT_NONE;
    XaAllocationSummary value = *summary;
    value.stable_fingerprint = xa_allocation_summary_fingerprint(&value);
    for (uint32_t i = 0; i < db->summary_count; i++) {
        if (allocation_summary_equal(&db->summaries[i], &value))
            return i + 1u;
    }
    if (db->summary_count >= db->summary_capacity && !allocation_db_grow(db))
        return XA_ALLOC_EFFECT_NONE;
    XaAllocationSummary *dst = &db->summaries[db->summary_count++];
    *dst = value;
    dst->cause_kind = value.cause_kind ? xr_strdup(value.cause_kind) : NULL;
    dst->cause_detail = value.cause_detail ? xr_strdup(value.cause_detail) : NULL;
    dst->callee_name = value.callee_name ? xr_strdup(value.callee_name) : NULL;
    return db->summary_count;
}

const XaAllocationSummary *xa_allocation_db_get(const XaAllocationDatabase *db,
                                                XaAllocEffectId id) {
    if (!db || id == XA_ALLOC_EFFECT_NONE || id > db->summary_count)
        return NULL;
    return &db->summaries[id - 1u];
}

uint32_t xa_allocation_db_summary_count(const XaAllocationDatabase *db) {
    return db ? db->summary_count : 0;
}
