/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_memory_effect_db.c - Canonical root-relative memory effect summaries
 */

#include "xa_memory_effect_db.h"
#include "../../base/xmalloc.h"
#include "../../shared/xr_hash_core.h"
#include <stddef.h>
#include <string.h>

struct XaMemoryEffectDatabase {
    XaMemoryEffectSummary *summaries;
    uint32_t count;
    uint32_t capacity;
    uint32_t next_revision;
};

static bool memory_grow(void **items, uint32_t *capacity, size_t item_size, uint32_t required) {
    if (*capacity >= required)
        return true;
    uint32_t next_capacity = *capacity ? *capacity * 2u : 4u;
    while (next_capacity < required)
        next_capacity *= 2u;
    void *next = xr_realloc(*items, (size_t) next_capacity * item_size);
    if (!next)
        return false;
    *items = next;
    *capacity = next_capacity;
    return true;
}

static int memory_root_compare(XaMemoryRootRef a, XaMemoryRootRef b) {
    if (a.kind != b.kind)
        return (a.kind > b.kind) - (a.kind < b.kind);
    return (a.index > b.index) - (a.index < b.index);
}

static int memory_root_index(const XaMemoryEffectSummary *summary, XaMemoryRootRef root) {
    if (!summary)
        return -1;
    for (uint32_t i = 0; i < summary->root_count; i++) {
        int order = memory_root_compare(summary->roots[i].root, root);
        if (order == 0)
            return (int) i;
        if (order > 0)
            break;
    }
    return -1;
}

static bool memory_ensure_root(XaMemoryEffectSummary *summary, XaMemoryRootRef root,
                               uint32_t *out_index) {
    if (!summary || root.kind < XA_MEMORY_ROOT_PARAM || root.kind > XA_MEMORY_ROOT_FOREIGN_HANDLE)
        return false;
    int existing = memory_root_index(summary, root);
    if (existing >= 0) {
        if (out_index)
            *out_index = (uint32_t) existing;
        return true;
    }
    if (!memory_grow((void **) &summary->roots, &summary->root_capacity, sizeof(XaMemoryRootEffect),
                     summary->root_count + 1u))
        return false;
    uint32_t position = summary->root_count;
    for (uint32_t i = 0; i < summary->root_count; i++) {
        if (memory_root_compare(summary->roots[i].root, root) > 0) {
            position = i;
            break;
        }
    }
    if (position < summary->root_count) {
        memmove(&summary->roots[position + 1u], &summary->roots[position],
                (size_t) (summary->root_count - position) * sizeof(XaMemoryRootEffect));
    }
    memset(&summary->roots[position], 0, sizeof(XaMemoryRootEffect));
    summary->roots[position].root = root;
    summary->root_count++;
    summary->fingerprint = 0;
    if (out_index)
        *out_index = position;
    return true;
}

static bool memory_add_write_to_root(XaMemoryRootEffect *effect, XaMemoryPlacePathId path) {
    if (!effect || path == XA_MEMORY_PLACE_PATH_NONE)
        return false;
    uint32_t position = 0;
    while (position < effect->write_count && effect->writes[position] < path)
        position++;
    if (position < effect->write_count && effect->writes[position] == path)
        return true;
    if (!memory_grow((void **) &effect->writes, &effect->write_capacity,
                     sizeof(XaMemoryPlacePathId), effect->write_count + 1u))
        return false;
    if (position < effect->write_count) {
        memmove(&effect->writes[position + 1u], &effect->writes[position],
                (size_t) (effect->write_count - position) * sizeof(XaMemoryPlacePathId));
    }
    effect->writes[position] = path;
    effect->write_count++;
    return true;
}

XaMemoryEffectDatabase *xa_memory_effect_db_new(void) {
    return (XaMemoryEffectDatabase *) xr_calloc(1, sizeof(XaMemoryEffectDatabase));
}

void xa_memory_effect_summary_init(XaMemoryEffectSummary *summary) {
    if (!summary)
        return;
    memset(summary, 0, sizeof(*summary));
    summary->completeness = XA_EFFECT_COMPLETE;
}

void xa_memory_effect_summary_clear(XaMemoryEffectSummary *summary) {
    if (!summary)
        return;
    for (uint32_t i = 0; i < summary->root_count; i++)
        xr_free(summary->roots[i].writes);
    xr_free(summary->roots);
    xa_memory_effect_summary_init(summary);
}

void xa_memory_effect_db_clear(XaMemoryEffectDatabase *db) {
    if (!db)
        return;
    for (uint32_t i = 0; i < db->count; i++)
        xa_memory_effect_summary_clear(&db->summaries[i]);
    xr_free(db->summaries);
    memset(db, 0, sizeof(*db));
}

void xa_memory_effect_db_free(XaMemoryEffectDatabase *db) {
    if (!db)
        return;
    xa_memory_effect_db_clear(db);
    xr_free(db);
}

bool xa_memory_effect_summary_add_write(XaMemoryEffectSummary *summary, XaMemoryRootRef root,
                                        XaMemoryPlacePathId path) {
    uint32_t index = 0;
    if (!memory_ensure_root(summary, root, &index))
        return false;
    if (!memory_add_write_to_root(&summary->roots[index], path))
        return false;
    summary->fingerprint = 0;
    return true;
}

bool xa_memory_effect_summary_mark_descriptor_rebind(XaMemoryEffectSummary *summary,
                                                     XaMemoryRootRef root) {
    uint32_t index = 0;
    if (!memory_ensure_root(summary, root, &index))
        return false;
    summary->roots[index].descriptor_rebind = true;
    summary->fingerprint = 0;
    return true;
}

bool xa_memory_effect_summary_mark_relocation(XaMemoryEffectSummary *summary,
                                              XaMemoryRootRef root) {
    uint32_t index = 0;
    if (!memory_ensure_root(summary, root, &index))
        return false;
    summary->roots[index].relocation = XA_MEMORY_MAY_RELOCATE;
    summary->fingerprint = 0;
    return true;
}

bool xa_memory_effect_summary_mark_shortening(XaMemoryEffectSummary *summary, XaMemoryRootRef root,
                                              XaMemoryRangeExprId range) {
    uint32_t index = 0;
    if (!memory_ensure_root(summary, root, &index))
        return false;
    bool was_shortening = summary->roots[index].shortening == XA_MEMORY_MAY_SHORTEN;
    summary->roots[index].shortening = XA_MEMORY_MAY_SHORTEN;
    if (!was_shortening)
        summary->roots[index].shortening_range = range;
    else if (summary->roots[index].shortening_range != range)
        summary->roots[index].shortening_range = XA_MEMORY_RANGE_EXPR_MIXED;
    summary->fingerprint = 0;
    return true;
}

bool xa_memory_effect_summary_mark_invalidation(XaMemoryEffectSummary *summary,
                                                XaMemoryRootRef root) {
    uint32_t index = 0;
    if (!memory_ensure_root(summary, root, &index))
        return false;
    summary->roots[index].invalidation = XA_MEMORY_INVALIDATES_VIEWS;
    summary->fingerprint = 0;
    return true;
}

void xa_memory_effect_summary_mark_incomplete(XaMemoryEffectSummary *summary,
                                              XaUnknownReason reason) {
    if (!summary || reason == XA_UNKNOWN_NONE)
        return;
    summary->completeness = XA_EFFECT_INCOMPLETE;
    summary->unknown_reasons |= (XaUnknownReasonSet) reason;
    summary->fingerprint = 0;
}

bool xa_memory_effect_summary_add_summary(XaMemoryEffectSummary *summary,
                                          const XaMemoryEffectSummary *source) {
    if (!summary || !source)
        return false;
    if (source->completeness == XA_EFFECT_INCOMPLETE) {
        summary->completeness = XA_EFFECT_INCOMPLETE;
        summary->unknown_reasons |= source->unknown_reasons;
    }
    for (uint32_t i = 0; i < source->root_count; i++) {
        const XaMemoryRootEffect *input = &source->roots[i];
        for (uint32_t p = 0; p < input->write_count; p++) {
            if (!xa_memory_effect_summary_add_write(summary, input->root, input->writes[p]))
                return false;
        }
        if (input->descriptor_rebind &&
            !xa_memory_effect_summary_mark_descriptor_rebind(summary, input->root))
            return false;
        if (input->relocation == XA_MEMORY_MAY_RELOCATE &&
            !xa_memory_effect_summary_mark_relocation(summary, input->root))
            return false;
        if (input->shortening == XA_MEMORY_MAY_SHORTEN &&
            !xa_memory_effect_summary_mark_shortening(summary, input->root,
                                                      input->shortening_range))
            return false;
        if (input->invalidation == XA_MEMORY_INVALIDATES_VIEWS &&
            !xa_memory_effect_summary_mark_invalidation(summary, input->root))
            return false;
    }
    summary->fingerprint = 0;
    return true;
}

bool xa_memory_effect_summary_is_complete(const XaMemoryEffectSummary *summary) {
    return summary && summary->completeness == XA_EFFECT_COMPLETE &&
           summary->unknown_reasons == XA_UNKNOWN_NONE;
}

bool xa_memory_effect_summary_has_resource_failure(const XaMemoryEffectSummary *summary) {
    return summary && (summary->unknown_reasons & XA_UNKNOWN_ANALYSIS_RESOURCE_FAILURE) != 0;
}

bool xa_memory_effect_summary_invalidates_live_view(const XaMemoryEffectSummary *summary,
                                                    XaMemoryRootRef root) {
    if (!xa_memory_effect_summary_is_complete(summary))
        return true;
    int index = memory_root_index(summary, root);
    if (index < 0)
        return false;
    const XaMemoryRootEffect *effect = &summary->roots[index];
    return effect->relocation == XA_MEMORY_MAY_RELOCATE ||
           effect->shortening == XA_MEMORY_MAY_SHORTEN ||
           effect->invalidation == XA_MEMORY_INVALIDATES_VIEWS;
}

static uint64_t memory_hash_u64(uint64_t hash, uint64_t value) {
    for (uint32_t i = 0; i < 8u; i++) {
        hash ^= (uint8_t) (value >> (i * 8u));
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

uint64_t xa_memory_effect_summary_fingerprint(const XaMemoryEffectSummary *summary) {
    if (!summary)
        return 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = memory_hash_u64(hash, UINT64_C(0x78616d6566667631)); /* "xameffv1" */
    hash = memory_hash_u64(hash, summary->completeness);
    hash = memory_hash_u64(hash, summary->unknown_reasons);
    hash = memory_hash_u64(hash, summary->root_count);
    for (uint32_t i = 0; i < summary->root_count; i++) {
        const XaMemoryRootEffect *effect = &summary->roots[i];
        hash = memory_hash_u64(hash, effect->root.kind);
        hash = memory_hash_u64(hash, effect->root.index);
        hash = memory_hash_u64(hash, effect->descriptor_rebind ? 1u : 0u);
        hash = memory_hash_u64(hash, effect->relocation);
        hash = memory_hash_u64(hash, effect->shortening);
        hash = memory_hash_u64(hash, effect->shortening_range);
        hash = memory_hash_u64(hash, effect->invalidation);
        hash = memory_hash_u64(hash, effect->write_count);
        for (uint32_t p = 0; p < effect->write_count; p++)
            hash = memory_hash_u64(hash, effect->writes[p]);
    }
    hash = xr_hash_core_mix_u64(hash);
    return hash ? hash : 1u;
}

static bool memory_summary_copy(XaMemoryEffectSummary *target,
                                const XaMemoryEffectSummary *source) {
    xa_memory_effect_summary_init(target);
    target->completeness = source->completeness;
    target->unknown_reasons = source->unknown_reasons;
    if (!xa_memory_effect_summary_add_summary(target, source)) {
        xa_memory_effect_summary_clear(target);
        return false;
    }
    target->fingerprint = source->fingerprint;
    target->revision = source->revision;
    return true;
}

static bool memory_summary_equal(const XaMemoryEffectSummary *a, const XaMemoryEffectSummary *b) {
    if (a == b)
        return true;
    if (!a || !b || a->completeness != b->completeness ||
        a->unknown_reasons != b->unknown_reasons || a->root_count != b->root_count)
        return false;
    for (uint32_t i = 0; i < a->root_count; i++) {
        const XaMemoryRootEffect *x = &a->roots[i];
        const XaMemoryRootEffect *y = &b->roots[i];
        if (memory_root_compare(x->root, y->root) != 0 ||
            x->descriptor_rebind != y->descriptor_rebind || x->relocation != y->relocation ||
            x->shortening != y->shortening || x->shortening_range != y->shortening_range ||
            x->invalidation != y->invalidation || x->write_count != y->write_count)
            return false;
        for (uint32_t p = 0; p < x->write_count; p++) {
            if (x->writes[p] != y->writes[p])
                return false;
        }
    }
    return true;
}

XaMemoryEffectId xa_memory_effect_db_intern(XaMemoryEffectDatabase *db,
                                            const XaMemoryEffectSummary *summary) {
    if (!db || !summary || xa_memory_effect_summary_has_resource_failure(summary))
        return XA_MEMORY_EFFECT_NONE;
    XaMemoryEffectSummary copy;
    if (!memory_summary_copy(&copy, summary))
        return XA_MEMORY_EFFECT_NONE;
    copy.fingerprint = xa_memory_effect_summary_fingerprint(&copy);
    for (uint32_t i = 0; i < db->count; i++) {
        if (db->summaries[i].fingerprint == copy.fingerprint &&
            memory_summary_equal(&db->summaries[i], &copy)) {
            xa_memory_effect_summary_clear(&copy);
            return i + 1u;
        }
    }
    if (!memory_grow((void **) &db->summaries, &db->capacity, sizeof(XaMemoryEffectSummary),
                     db->count + 1u)) {
        xa_memory_effect_summary_clear(&copy);
        return XA_MEMORY_EFFECT_NONE;
    }
    copy.revision = ++db->next_revision;
    db->summaries[db->count++] = copy;
    return db->count;
}

const XaMemoryEffectSummary *xa_memory_effect_db_get(const XaMemoryEffectDatabase *db,
                                                     XaMemoryEffectId id) {
    if (!db || id == XA_MEMORY_EFFECT_NONE || id > db->count)
        return NULL;
    return &db->summaries[id - 1u];
}

uint32_t xa_memory_effect_db_summary_count(const XaMemoryEffectDatabase *db) {
    return db ? db->count : 0;
}
